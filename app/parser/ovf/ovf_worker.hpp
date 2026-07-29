#ifndef OVF_WORKER_HPP
#define OVF_WORKER_HPP

#include "parser_worker.hpp"
#include "cal_parser.hpp"
#include "parser_config.hpp"
#include "open_vector_format.pb.h"
#include <atomic>
#include <string>
#include <thread>

namespace upc::parser {

/** OVF 默认层厚（μm），WorkPlane 无 z_pos_in_mm 时使用 */
#ifndef OVF_DEFAULT_LAYER_UM
#define OVF_DEFAULT_LAYER_UM  GCODE_DEFAULT_LAYER_UM
#endif

#ifndef OVF_PARSE_CHUNK_SEGMENTS
#define OVF_PARSE_CHUNK_SEGMENTS        100
#endif

#ifndef OVF_PARSE_SEND_SYNC_SEGMENTS
#define OVF_PARSE_SEND_SYNC_SEGMENTS    50
#endif

#ifndef OVF_PARSE_CHUNK_INTERVAL_MS
#define OVF_PARSE_CHUNK_INTERVAL_MS     10
#endif

/** 默认进给（mm/min），MarkingParams 未解析时使用 */
#ifndef OVF_DEFAULT_FEED_MM_MIN
#define OVF_DEFAULT_FEED_MM_MIN         1200.0f
#endif

/** Open Vector Format（protobuf Job）解析器 */
class OvfWorker : public IParserWorker {
public:
    static OvfWorker& instance();

    int  open_file(const std::string& filepath) override;
    void start_parse() override;
    void stop_parse() override;
    void close_file() override;

    int  get_total_lines() const override;
    int  get_parsed_lines() const override;
    int  get_total_motion_packets() const override;
    bool is_parsing() const override;
    bool is_file_read_exhausted() const override;
    bool is_file_open() const override;

    RingBuffer& ring_buffer() override;
    uint16_t get_layer_height_um() const override;

private:
    OvfWorker();
    ~OvfWorker() override;
    OvfWorker(const OvfWorker&) = delete;
    OvfWorker& operator=(const OvfWorker&) = delete;

    void parser_thread_fn();
    void apply_calibration_from_context();
    void apply_gain_defaults();
    void sync_parse_progress(int segments, int packets);
    void parser_chunk_yield();

    /** 从 MarkingParamsMap 查找指定 key 的参数，返回默认值如果未找到 */
    const open_vector_format::MarkingParams* lookup_marking_params(int32_t key) const;

    /** 确保当前 marking params 的功率/速度已输出 */
    void ensure_marking_params(int32_t key, int* packets);

    bool push_layer_mark(uint16_t layer_um);
    bool push_power_packet(uint16_t power);
    bool build_and_push_packet(float x, float y, float feed_mm_min, int laser_on);

    /** 处理单个矢量块（自动分派到具体类型处理器） */
    bool process_vector_block(const open_vector_format::VectorBlock& block,
                              int* segments, int* packets);

    /** 各类型矢量块的具体处理 */
    bool process_line_sequence(const open_vector_format::VectorBlock& block,
                               int* segments, int* packets);
    bool process_line_sequence_3d(const open_vector_format::VectorBlock& block,
                                  int* segments, int* packets);
    bool process_hatches(const open_vector_format::VectorBlock& block,
                         int* segments, int* packets);
    bool process_hatches_3d(const open_vector_format::VectorBlock& block,
                            int* segments, int* packets);
    bool process_point_sequence(const open_vector_format::VectorBlock& block,
                                int* segments, int* packets);
    bool process_point_sequence_3d(const open_vector_format::VectorBlock& block,
                                   int* segments, int* packets);

    bool process_work_plane(int plane_index, int* segments, int* packets);

    static int count_motion_segments(const open_vector_format::Job& job);

    open_vector_format::Job job_;
    std::string filepath_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_req_{false};
    std::atomic<int> total_lines_{0};
    std::atomic<int> total_motion_packets_{0};
    std::atomic<int> parsed_lines_{0};
    std::atomic<bool> file_read_exhausted_{false};

    float image_scale_gain_ = 1.0f;
    float image_rang_gain_  = 1.0f;
    float speed_gain_       = 1.0f;

    CalData cal_data_;
    RingBuffer rb_;
    uint16_t last_layer_height_um_ = OVF_DEFAULT_LAYER_UM;

    /** 当前生效的 marking params key（用于检测变化并输出功率包） */
    int32_t current_mark_key_ = -1;
    /** 当前激光功率（0-65535） */
    uint16_t current_power_ = 0;
    /** 当前进给速度（mm/min） */
    float current_feed_mm_min_ = OVF_DEFAULT_FEED_MM_MIN;
};

} // namespace upc::parser

#endif // OVF_WORKER_HPP
