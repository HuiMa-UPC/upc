#include "ovf_worker.hpp"
#include "ovf_config.hpp"
#include "parser_config.hpp"
#include "rs_log.hpp"
#include "parser_context.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <libgen.h>
#include <thread>

namespace upc::parser {

namespace {

void int_to_u16_bytes(int value, uint8_t* low_byte, uint8_t* high_byte)
{
    if(value > 65535) {
        value = 65535;
    }
    if(value < 0) {
        value = 0;
    }
    *low_byte  = static_cast<uint8_t>(value & 0xFF);
    *high_byte = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void coordinate_to_offset_bytes(float coordinate_value, float image_rang_gain,
                                uint8_t* low_byte, uint8_t* high_byte)
{
    if(image_rang_gain < 1e-6f) {
        image_rang_gain = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    if(coordinate_value > image_rang_gain) {
        coordinate_value = image_rang_gain;
    }
    if(coordinate_value < -image_rang_gain) {
        coordinate_value = -image_rang_gain;
    }

    const float normalized = coordinate_value / image_rang_gain;
    const int offset_value = static_cast<int>(normalized * 32767.0f + 32768.0f);
    int_to_u16_bytes(offset_value, low_byte, high_byte);
}

uint16_t ovf_z_mm_to_layer_um(float z_mm)
{
    if(z_mm <= 0.f) {
        return 0;
    }
    return gcode_z_to_layer_um(z_mm * 1000.f);
}

} // namespace

OvfWorker& OvfWorker::instance()
{
    static OvfWorker inst;
    return inst;
}

OvfWorker::OvfWorker() = default;

OvfWorker::~OvfWorker()
{
    close_file();
}

void OvfWorker::apply_gain_defaults()
{
    if(image_rang_gain_ < 1e-6f) {
        image_rang_gain_ = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    if(image_scale_gain_ < 1e-6f) {
        image_scale_gain_ = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
    }
    if(speed_gain_ < 1e-6f) {
        speed_gain_ = GCODE_DEFAULT_SPEED_GAIN;
    }
}

void OvfWorker::apply_calibration_from_context()
{
    const ParserContext& ctx = ParserContext::instance();

    if(ctx.cal_valid()) {
        cal_data_ = ctx.cal_data();
    } else {
        cal_data_clear(&cal_data_);
    }

    image_scale_gain_ = ctx.scale_gain();
    image_rang_gain_  = ctx.range_gain();
    speed_gain_       = ctx.speed_gain();

    apply_gain_defaults();
}

void OvfWorker::sync_parse_progress(int segments, int packets)
{
    parsed_lines_.store(segments);
    total_motion_packets_.store(packets);
}

void OvfWorker::parser_chunk_yield()
{
    std::this_thread::sleep_for(
        std::chrono::milliseconds(OVF_PARSE_CHUNK_INTERVAL_MS));
}

int OvfWorker::count_motion_segments(const open_vector_format::Job& job)
{
    int segments = 0;
    for(int wp = 0; wp < job.work_planes_size(); ++wp) {
        const auto& plane = job.work_planes(wp);
        for(int vb = 0; vb < plane.vector_blocks_size(); ++vb) {
            const auto& block = plane.vector_blocks(vb);
            if(block.has_line_sequence()) {
                const int pts = block.line_sequence().points_size();
                if(pts >= 4) {
                    segments += (pts / 2) - 1;
                }
            } else if(block.has_line_sequence_3d()) {
                const int pts = block.line_sequence_3d().points_size();
                if(pts >= 6) {
                    segments += (pts / 3) - 1;
                }
            }
        }
    }
    return segments;
}

bool OvfWorker::push_layer_mark(uint16_t layer_um)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_LAYER_MARK;
    std::memset(pkt.data, 0, sizeof(pkt.data));
    rb_layer_mark_set_height_um(pkt, layer_um);
    return rb_.put_wait(pkt, stop_req_);
}

bool OvfWorker::build_and_push_packet(float x, float y, float feed_mm_min,
                                      int laser_on)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    pkt.data[0] = 0x01;

    float out_x = 0.0f;
    float out_y = 0.0f;
    cal_apply_xy(&cal_data_, x, y, &out_x, &out_y);

    const float ox = out_x * image_scale_gain_;
    const float oy = out_y * image_scale_gain_;

    uint8_t lo = 0;
    uint8_t hi = 0;

    coordinate_to_offset_bytes(ox, image_rang_gain_, &lo, &hi);
    pkt.data[1] = lo;
    pkt.data[2] = hi;

    coordinate_to_offset_bytes(oy, image_rang_gain_, &lo, &hi);
    pkt.data[3] = lo;
    pkt.data[4] = hi;

    pkt.data[5] = 0;
    pkt.data[6] = 0;

    int speed_int = static_cast<int>(feed_mm_min * speed_gain_);
    int_to_u16_bytes(speed_int, &lo, &hi);
    pkt.data[7] = lo;
    pkt.data[8] = hi;

    int_to_u16_bytes(laser_on ? 1 : 0, &lo, &hi);
    pkt.data[9]  = lo;
    pkt.data[10] = hi;

    return rb_.put_wait(pkt, stop_req_);
}

bool OvfWorker::process_vector_block(const open_vector_format::VectorBlock& block,
                                     int* segments, int* packets)
{
    if(block.has_line_sequence()) {
        const auto& seq = block.line_sequence();
        const int pts = seq.points_size();
        if(pts < 4) {
            return true;
        }

        for(int i = 2; i + 1 < pts; i += 2) {
            if(stop_req_.load()) {
                return false;
            }

            const float x = seq.points(i);
            const float y = seq.points(i + 1);
            if(!build_and_push_packet(x, y, OVF_DEFAULT_FEED_MM_MIN, 1)) {
                stop_req_.store(true);
                return false;
            }
            (*segments)++;
            (*packets)++;
        }
        return !stop_req_.load();
    }

    if(block.has_line_sequence_3d()) {
        const auto& seq = block.line_sequence_3d();
        const int pts = seq.points_size();
        if(pts < 6) {
            return true;
        }

        for(int i = 0; i + 5 < pts; i += 3) {
            if(stop_req_.load()) {
                return false;
            }

            const float x = seq.points(i);
            const float y = seq.points(i + 1);
            if(!build_and_push_packet(x, y, OVF_DEFAULT_FEED_MM_MIN, 1)) {
                stop_req_.store(true);
                return false;
            }
            (*segments)++;
            (*packets)++;
        }
        return !stop_req_.load();
    }

    UPC_LOG_OVF_WORKER(Debug, "skip unsupported vector block type");
    return true;
}

bool OvfWorker::process_work_plane(int plane_index, int* segments, int* packets)
{
    const auto& plane = job_.work_planes(plane_index);

    uint16_t layer_um = ovf_z_mm_to_layer_um(plane.z_pos_in_mm());
    if(layer_um == 0) {
        layer_um = last_layer_height_um_;
    }
    last_layer_height_um_ = layer_um;

    if(!push_layer_mark(layer_um)) {
        stop_req_.store(true);
        return false;
    }

    for(int vb = 0; vb < plane.vector_blocks_size(); ++vb) {
        if(stop_req_.load()) {
            return false;
        }
        if(!process_vector_block(plane.vector_blocks(vb), segments, packets)) {
            return false;
        }
    }
    return true;
}

void OvfWorker::parser_thread_fn()
{
    int segments = 0;
    int packets  = 0;
    int chunk_segments = 0;
    int since_sync = 0;

    for(int wp = 0;
        wp < job_.work_planes_size() &&
        running_.load(std::memory_order_relaxed) &&
        !stop_req_.load(std::memory_order_relaxed);
        ++wp)
    {
        const int seg_before = segments;
        if(!process_work_plane(wp, &segments, &packets)) {
            break;
        }

        const int added = segments - seg_before;
        since_sync += added;
        chunk_segments += added;

        if(since_sync >= OVF_PARSE_SEND_SYNC_SEGMENTS) {
            sync_parse_progress(segments, packets);
            since_sync = 0;
        }

        if(chunk_segments >= OVF_PARSE_CHUNK_SEGMENTS) {
            sync_parse_progress(segments, packets);
            chunk_segments = 0;
            parser_chunk_yield();
        }
    }

    sync_parse_progress(segments, packets);
    file_read_exhausted_.store(true);
    running_.store(false);
}

int OvfWorker::open_file(const std::string& filepath)
{
    close_file();
    apply_calibration_from_context();

    std::ifstream in(filepath, std::ios::binary);
    if(!in) {
        return -1;
    }

    job_.Clear();
    if(!job_.ParseFromIstream(&in)) {
        UPC_LOG_OVF_WORKER(Error, "protobuf parse failed: %s", filepath.c_str());
        job_.Clear();
        return -1;
    }

    filepath_ = filepath;
    const int segments = count_motion_segments(job_);
    total_lines_.store(segments);
    total_motion_packets_.store(0);
    parsed_lines_.store(0);
    file_read_exhausted_.store(false);
    last_layer_height_um_ = OVF_DEFAULT_LAYER_UM;

    if(job_.work_planes_size() > 0) {
        const uint16_t um =
            ovf_z_mm_to_layer_um(job_.work_planes(0).z_pos_in_mm());
        if(um > 0) {
            last_layer_height_um_ = um;
        }
    }

    {
        char path_copy[512];
        std::snprintf(path_copy, sizeof(path_copy), "%s", filepath.c_str());
        const char* base = basename(path_copy);
        UPC_LOG_OVF_WORKER(Info,
            "open file: %s (work_planes=%d segments=%d)",
            base, job_.work_planes_size(), segments);
    }
    return 0;
}

void OvfWorker::start_parse()
{
    if(filepath_.empty()) {
        return;
    }
    stop_parse();

    stop_req_.store(false);
    running_.store(true);
    parsed_lines_.store(0);
    total_motion_packets_.store(0);
    file_read_exhausted_.store(false);

    th_ = std::thread(&OvfWorker::parser_thread_fn, this);
}

void OvfWorker::stop_parse()
{
    stop_req_.store(true);
    rb_.clear();
    if(th_.joinable()) {
        th_.join();
    }
    running_.store(false);
}

void OvfWorker::close_file()
{
    stop_parse();
    job_.Clear();
    filepath_.clear();
    total_lines_.store(0);
    total_motion_packets_.store(0);
    parsed_lines_.store(0);
    file_read_exhausted_.store(false);
}

int OvfWorker::get_total_motion_packets() const
{
    return total_motion_packets_.load();
}

int OvfWorker::get_total_lines() const
{
    return total_lines_.load();
}

int OvfWorker::get_parsed_lines() const
{
    return parsed_lines_.load();
}

bool OvfWorker::is_parsing() const
{
    return running_.load();
}

bool OvfWorker::is_file_read_exhausted() const
{
    return file_read_exhausted_.load();
}

bool OvfWorker::is_file_open() const
{
    return !filepath_.empty();
}

RingBuffer& OvfWorker::ring_buffer()
{
    return rb_;
}

uint16_t OvfWorker::get_layer_height_um() const
{
    return last_layer_height_um_;
}

} // namespace upc::parser
