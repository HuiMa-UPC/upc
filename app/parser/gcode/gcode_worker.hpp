#ifndef GCODE_WORKER_HPP
#define GCODE_WORKER_HPP

#include "parser_worker.hpp"
#include "cal_parser.hpp"
#include "parser_config.hpp"
#include <atomic>
#include <string>
#include <thread>

namespace upc::parser {

/**
 * G-code 切片文件解析器（单例）。
 *
 * 在独立解析线程中逐行读取 .gcode / .nc 文件，将 G0/G1 运动、M3000 换层、
 * 独立 S 功率行转换为 RingBuffer 中的 rb_packet_t（RB_PKT_MOTION / RB_PKT_LAYER_MARK）。
 *
 * 坐标经 ParserContext 校准表与增益缩放后，编码为 MCU 11 字节运动子包。
 * 通过 ParserRegistry 静态注册，由 ParserManager 按后缀选择。
 */
class GcodeWorker : public IParserWorker {
public:
    /** @return 进程内唯一 GcodeWorker 实例（Meyers 单例） */
    static GcodeWorker& instance();

    /** @brief 为下一次 open_file 提供已知总行数，跳过重复整文件统计。 */
    void set_total_lines_hint(int total_lines) override;

    /**
     * 打开切片文件并预统计总行数。
     * @param filepath 绝对或相对路径
     * @return 0 成功，-1 打开/统计失败
     */
    int  open_file(const std::string& filepath) override;

    /** 启动解析线程（需先 open_file）；会 rewind 文件并清空进度计数 */
    void start_parse() override;

    /** 请求停止解析线程、清空 RingBuffer 并 join */
    void stop_parse() override;

    /** stop_parse + fclose，重置全部状态 */
    void close_file() override;

    /** @return open_file 时统计的文件总行数 */
    int  get_total_lines() const override;

    /** @return 已解析行数（周期性同步，非逐行实时） */
    int  get_parsed_lines() const override;

    /** @return 已产出的运动/功率包总数 */
    int  get_total_motion_packets() const override;

    /** @return 解析线程是否正在运行 */
    bool is_parsing() const override;

    /** @return 是否已读到文件末尾（EOF） */
    bool is_file_read_exhausted() const override;

    /** @return 是否已通过 open_file 打开文件 */
    bool is_file_open() const override;

    /** @return 本 worker 绑定的运动包输出队列 */
    RingBuffer& ring_buffer() override;

    /** @return 最近一次 Z 坐标推导的层厚（μm），无 Z 时为默认值 */
    uint16_t get_layer_height_um() const override;

private:
    GcodeWorker() = default;
    ~GcodeWorker() override;
    GcodeWorker(const GcodeWorker&) = delete;
    GcodeWorker& operator=(const GcodeWorker&) = delete;

    /** parse_line_all 可识别的单行事件类型（每行最多一种） */
    enum class LineEmitKind : uint8_t {
        None   = 0, /**< 空行/注释/无法识别，不发射包 */
        Motion = 1, /**< G0/G1 运动 → build_and_push_packet */
        Layer  = 2, /**< M3000 换层 → push_layer_mark */
        Power  = 3, /**< 独立 S 行 → push_power_packet */
    };

    /** 单行解析结果 */
    struct LineEvent {
        LineEmitKind kind = LineEmitKind::None;
        uint16_t power = 0; /**< kind==Power 时有效 */
    };

    /**
     * 解析线程主循环：分块读行 → parse_line_all → emit_parsed_line → RingBuffer。
     * 每块 GCODE_PARSE_CHUNK_LINES 行后 yield，避免独占 CPU。
     */
    void parser_thread_fn();

    /** 重置 X/Y/F/E 运动状态机为初始值（不含层厚） */
    void reset_parser_motion_state();

    /** 从 ParserContext 加载校准表与图像/速度增益，并钳位默认值 */
    void apply_calibration_from_context();

    /**
     * 原子更新已解析行数与运动包计数（供进度 API 查询）。
     * @param lines   累计已解析行
     * @param packets 累计已产出包数（运动 + 功率）
     */
    void sync_parse_progress(int lines, int packets);

    /**
     * 阻塞写入 RingBuffer，可被 stop_req_ 中断。
     * @param pkt 待写入包
     * @return put_wait 成功 true
     */
    bool put_packet(rb_packet_t pkt);

    /**
     * 根据当前 X/Y/F/E 状态构建 RB_PKT_MOTION 运动包（data[0]=0x01）并写入队列。
     * 坐标经校准与 scale/range 增益编码为 data[1..10]。
     * @return put_packet 成功 true
     */
    bool build_and_push_packet();

    /**
     * 构建功率子包（data[0]=0x02，功率在 data[9..10]）并写入队列。
     * @param power 激光功率（0–65535）
     */
    bool push_power_packet(uint16_t power);

    /** 构建 RB_PKT_LAYER_MARK 换层标记包（层厚取自 last_layer_height_um_） */
    bool push_layer_mark();

    /**
     * 解析单行 G-code，识别运动 / 换层 / 功率事件。
     * @param raw_line fgets 读入的原始行（可含注释）
     * @param out      输出事件类型；无事件时 kind==None
     */
    void parse_line_all(const char* raw_line, LineEvent* out);

    /**
     * 解析 G0/G1 行，更新内部 X/Y/F/E 状态；Z 仅用于更新层厚。
     * @param normalized normalize_command_line 后的有效子串
     * @return 识别为 G0/G1 且状态有效 true（首行须含坐标/速度/E 之一）
     */
    bool try_parse_motion_line(const std::string& normalized);

    /**
     * 将单行解析结果发射到 RingBuffer。
     * @param parsed  parse_line_all 输出
     * @param packets 运动/功率包计数器（遇 Motion/Power 时递增）
     * @return false 表示 stop_req 或 put_packet 失败
     */
    bool emit_parsed_line(const LineEvent& parsed, int* packets);

    FILE* fp_ = nullptr;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_req_{false};
    std::atomic<int> total_lines_{0};
    std::atomic<int> total_motion_packets_{0};
    std::atomic<int> parsed_lines_{0};
    std::atomic<bool> file_read_exhausted_{false};
    int total_lines_hint_ = -1;

    float image_scale_gain_ = 1.0f; /**< 图像坐标缩放增益 */
    float image_rang_gain_  = 1.0f; /**< 坐标满量程（mm），用于编码偏移 */
    float speed_gain_       = 1.0f; /**< F 速度增益 */

    float x_ = 0.0f; /**< 当前 X（mm） */
    float y_ = 0.0f; /**< 当前 Y（mm） */
    float f_ = 0.0f; /**< 当前进给速度 F */
    int   e_ = 0;    /**< 挤出标志：E>0 为 1，否则 0 */
    bool  state_initialized_ = false; /**< 是否已收到首个有效 G0/G1 参数 */

    CalData cal_data_;                /**< 校准表副本 */
    RingBuffer rb_;                   /**< 运动包输出队列 */
    uint16_t last_layer_height_um_ = GCODE_DEFAULT_LAYER_UM; /**< 最近 Z 推导层厚 */
};

} // namespace upc::parser

#endif // GCODE_WORKER_HPP
