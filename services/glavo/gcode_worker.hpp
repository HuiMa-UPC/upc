#ifndef GCODE_WORKER_HPP
#define GCODE_WORKER_HPP

#include "ring_buffer.hpp"
#include "cal_parser.hpp"
#include <string>
#include <atomic>
#include <thread>

namespace services {

class GcodeWorker {
public:
    static GcodeWorker& instance();

    /** 打开并预处理 G-code 文件（统计行数）
     *  @return 0 成功，-1 失败 */
    int open_file(const std::string& filepath);

    /** 启动解析线程，解析结果写入内部 RingBuffer */
    void start_parse();

    /** 请求停止解析线程并等待其结束 */
    void stop_parse();

    /** 关闭文件并停止解析 */
    void close_file();

    /** 查询进度 */
    int  get_total_lines(void) const;
    int  get_parsed_lines(void) const;
    bool is_parsing(void) const;
    bool is_file_read_exhausted(void) const;
    bool is_file_open(void) const;

    /** 访问解析结果队列（供消费者读取） */
    RingBuffer& ring_buffer();

    /** 设置解析增益参数 */
    void set_parse_gains(float speed_gain, float scale_gain, float rang_gain);

    /** 设置中心偏移（用于坐标映射） */
    void set_center_offset(float dx, float dy);

    /** 加载校准文件并应用增益/偏移矩阵（服务初始化时调用） */
    void parser_load_gains();

private:
    GcodeWorker();
    ~GcodeWorker();
    GcodeWorker(const GcodeWorker&) = delete;
    GcodeWorker& operator=(const GcodeWorker&) = delete;

    void parser_thread_fn();
    void reset_parser_motion_state();
    void apply_gain_defaults();
    void sync_parse_progress(int lines, int packets);
    void parser_set_not_parsing();
    void parser_chunk_yield();

    bool build_and_push_packet();
    bool push_layer_mark();
    bool emit_line_parse_result(const struct LineParseOut *parsed, int *packets);

    // 解析相关（原自由函数，改为成员以访问私有状态）
    void parse_line_all(const char *raw_line, struct LineParseOut *out);
    bool parse_g1_segment(const std::string &segment, bool *out_motion);

    // 文件与线程
    FILE* fp_ = nullptr;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_req_{false};     //解析线程停止请求
    std::atomic<int> total_lines_{0};       //总行数
    std::atomic<int> parsed_lines_{0};      //已解析的行数
    std::atomic<bool> file_read_exhausted_{false};  //文件是否已经读取完毕

    // 解析参数
    float image_scale_gain_ = 1.0f;
    float image_rang_gain_  = 1.0f;
    float speed_gain_       = 1.0f;
    float center_offset_x_  = 0.0f;
    float center_offset_y_  = 0.0f;

    // 当前运动状态（模态）
    float x_ = 0.0f;
    float y_ = 0.0f;
    float z_ = 0.0f;
    float f_ = 0.0f;
    int   e_ = 0;
    bool  state_initialized_ = false;

    // 校准数据（二维线性插补补偿）
    CalData cal_data_;
    bool    enable_cal_ = false;

    // 解析结果队列
    RingBuffer rb_;
};

} // namespace services

#endif // GCODE_WORKER_HPP
