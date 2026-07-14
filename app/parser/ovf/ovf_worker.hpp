#ifndef OVF_WORKER_HPP
#define OVF_WORKER_HPP

#include "parser_worker.hpp"
#include "cal_parser.hpp"
#include "ovf_config.hpp"
#include "open_vector_format.pb.h"
#include <atomic>
#include <string>
#include <thread>

namespace upc::parser {

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

    bool push_layer_mark(uint16_t layer_um);
    bool build_and_push_packet(float x, float y, float feed_mm_min, int laser_on);
    bool process_work_plane(int plane_index, int* segments, int* packets);
    bool process_vector_block(const open_vector_format::VectorBlock& block,
                              int* segments, int* packets);

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
};

} // namespace upc::parser

#endif // OVF_WORKER_HPP
