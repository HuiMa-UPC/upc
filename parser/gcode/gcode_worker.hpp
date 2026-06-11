#ifndef GCODE_WORKER_HPP
#define GCODE_WORKER_HPP

#include "parser_worker.hpp"
#include "cal_parser.hpp"
#include "parser_config.hpp"
#include <atomic>
#include <string>
#include <thread>

namespace upc::parser {

class GcodeWorker : public IParserWorker {
public:
    static GcodeWorker& instance();

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
    GcodeWorker();
    ~GcodeWorker() override;
    GcodeWorker(const GcodeWorker&) = delete;
    GcodeWorker& operator=(const GcodeWorker&) = delete;

    void parser_thread_fn();
    void reset_parser_motion_state();
    void apply_calibration_from_context();
    void apply_gain_defaults();
    void sync_parse_progress(int lines, int packets);
    void parser_chunk_yield();

    bool build_and_push_packet();
    bool push_power_packet(uint16_t power);
    bool push_layer_mark();
    bool emit_line_parse_result(const struct LineParseOut* parsed, int* packets);

    void parse_line_all(const char* raw_line, struct LineParseOut* out);
    bool parse_g1_segment(const std::string& segment, bool* out_motion);
    int  prescan_emit_packets();

    FILE* fp_ = nullptr;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_req_{false};
    std::atomic<int> total_lines_{0};
    std::atomic<int> prescan_emit_packets_{0};
    std::atomic<int> total_motion_packets_{0};
    std::atomic<int> parsed_lines_{0};
    std::atomic<bool> file_read_exhausted_{false};

    float image_scale_gain_ = 1.0f;
    float image_rang_gain_  = 1.0f;
    float speed_gain_       = 1.0f;

    float x_ = 0.0f;
    float y_ = 0.0f;
    float z_ = 0.0f;
    float f_ = 0.0f;
    int   e_ = 0;
    bool  state_initialized_ = false;

    CalData cal_data_;

    RingBuffer rb_;

    uint16_t last_layer_height_um_ = GCODE_DEFAULT_LAYER_UM;
};

} // namespace upc::parser

#endif // GCODE_WORKER_HPP
