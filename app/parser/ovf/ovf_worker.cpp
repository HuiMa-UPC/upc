#include "ovf_worker.hpp"
#include "ovf_official_io.h"
#include "parser_config.hpp"
#include "rs_log.hpp"
#include "parser_context.hpp"

#include <chrono>
#include <cstring>
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

/** 将 W 单位功率 (0-1000W) 映射为 16-bit DAC 值 (0-65535) */
uint16_t power_w_to_dac(float power_w)
{
    if(power_w <= 0.0f) {
        return 0;
    }
    // 65535 对应最大功率 1000W（可根据实际设备调整）
    const float dac = power_w * 65535.0f / 1000.0f;
    if(dac > 65535.0f) {
        return 65535;
    }
    return static_cast<uint16_t>(dac + 0.5f);
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

const open_vector_format::MarkingParams* OvfWorker::lookup_marking_params(int32_t key) const
{
    if(key < 0) {
        return nullptr;
    }
    const auto& mp_map = job_.marking_params_map();
    auto it = mp_map.find(key);
    if(it != mp_map.end()) {
        return &it->second;
    }
    return nullptr;
}

void OvfWorker::ensure_marking_params(int32_t key, int* packets)
{
    if(key == current_mark_key_) {
        return;  // 已应用
    }

    const auto* mp = lookup_marking_params(key);

    // 更新功率
    uint16_t new_power = 0;
    float new_feed = OVF_DEFAULT_FEED_MM_MIN;

    if(mp) {
        new_power = power_w_to_dac(mp->laser_power_in_w());
        // OVF 中 laser_speed_in_mm_per_s 转换为 mm/min
        new_feed = mp->laser_speed_in_mm_per_s() * 60.0f;
        if(new_feed < 1.0f) {
            new_feed = OVF_DEFAULT_FEED_MM_MIN;
        }
    }

    if(new_power != current_power_) {
        if(!push_power_packet(new_power)) {
            stop_req_.store(true);
            return;
        }
        (*packets)++;
        current_power_ = new_power;
    }

    current_feed_mm_min_ = new_feed;
    current_mark_key_ = key;

    UPC_LOG_OVF_WORKER(Debug,
        "marking params changed: key=%d power=%u feed=%.1f",
        key, new_power, new_feed);
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
                    segments += pts / 2;  // 每点一个运动包
                }
            } else if(block.has_line_sequence_3d()) {
                const int pts = block.line_sequence_3d().points_size();
                if(pts >= 6) {
                    segments += pts / 3;
                }
            } else if(block.has__hatches()) {
                const int pts = block._hatches().points_size();
                if(pts >= 4) {
                    segments += pts / 2;  // 每条线 2 个包（jump+mark）
                }
            } else if(block.has_hatches_3d()) {
                const int pts = block.hatches_3d().points_size();
                if(pts >= 6) {
                    segments += pts / 3;
                }
            } else if(block.has_point_sequence()) {
                const int pts = block.point_sequence().points_size();
                if(pts >= 2) {
                    segments += pts / 2;
                }
            } else if(block.has_point_sequence_3d()) {
                const int pts = block.point_sequence_3d().points_size();
                if(pts >= 3) {
                    segments += pts / 3;
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

bool OvfWorker::push_power_packet(uint16_t power)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    std::memset(pkt.data, 0, sizeof(pkt.data));
    pkt.data[0] = 0x02;  // 功率包标识
    pkt.data[9]  = static_cast<uint8_t>(power & 0xFF);
    pkt.data[10] = static_cast<uint8_t>(power >> 8);
    return rb_.put_wait(pkt, stop_req_);
}

bool OvfWorker::build_and_push_packet(float x, float y, float feed_mm_min,
                                      int laser_on)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    pkt.data[0] = 0x01;  // 运动包标识

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

// ============================================================================
// 矢量块处理
// ============================================================================

bool OvfWorker::process_line_sequence(const open_vector_format::VectorBlock& block,
                                       int* segments, int* packets)
{
    const auto& seq = block.line_sequence();
    const int pts = seq.points_size();
    if(pts < 4) {
        return true;
    }

    // 第一个点作为 jump（laser_on=0），后续点作为 mark（laser_on=1）
    for(int i = 0; i + 1 < pts; i += 2) {
        if(stop_req_.load()) {
            return false;
        }

        const float x = seq.points(i);
        const float y = seq.points(i + 1);
        const int laser_on = (i == 0) ? 0 : 1;

        if(!build_and_push_packet(x, y, current_feed_mm_min_, laser_on)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;
    }
    return !stop_req_.load();
}

bool OvfWorker::process_line_sequence_3d(const open_vector_format::VectorBlock& block,
                                          int* segments, int* packets)
{
    const auto& seq = block.line_sequence_3d();
    const int pts = seq.points_size();
    if(pts < 6) {
        return true;
    }

    for(int i = 0; i + 2 < pts; i += 3) {
        if(stop_req_.load()) {
            return false;
        }

        const float x = seq.points(i);
        const float y = seq.points(i + 1);
        const int laser_on = (i == 0) ? 0 : 1;

        if(!build_and_push_packet(x, y, current_feed_mm_min_, laser_on)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;
    }
    return !stop_req_.load();
}

bool OvfWorker::process_hatches(const open_vector_format::VectorBlock& block,
                                 int* segments, int* packets)
{
    const auto& hatches = block._hatches();
    const int pts = hatches.points_size();
    if(pts < 4) {
        return true;
    }

    // 每 4 个点一组：(x0,y0) → (x1,y1)
    for(int i = 0; i + 3 < pts; i += 4) {
        if(stop_req_.load()) {
            return false;
        }

        // 起点：jump（laser_on=0）
        const float x0 = hatches.points(i);
        const float y0 = hatches.points(i + 1);
        if(!build_and_push_packet(x0, y0, current_feed_mm_min_, 0)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;

        // 终点：mark（laser_on=1）
        const float x1 = hatches.points(i + 2);
        const float y1 = hatches.points(i + 3);
        if(!build_and_push_packet(x1, y1, current_feed_mm_min_, 1)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;
    }
    return !stop_req_.load();
}

bool OvfWorker::process_hatches_3d(const open_vector_format::VectorBlock& block,
                                    int* segments, int* packets)
{
    const auto& hatches = block.hatches_3d();
    const int pts = hatches.points_size();
    if(pts < 6) {
        return true;
    }

    // 每 6 个点一组：(x0,y0,z0) → (x1,y1,z1)
    for(int i = 0; i + 5 < pts; i += 6) {
        if(stop_req_.load()) {
            return false;
        }

        // 起点：jump（laser_on=0）
        const float x0 = hatches.points(i);
        const float y0 = hatches.points(i + 1);
        if(!build_and_push_packet(x0, y0, current_feed_mm_min_, 0)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;

        // 终点：mark（laser_on=1）
        const float x1 = hatches.points(i + 3);
        const float y1 = hatches.points(i + 4);
        if(!build_and_push_packet(x1, y1, current_feed_mm_min_, 1)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;
    }
    return !stop_req_.load();
}

bool OvfWorker::process_point_sequence(const open_vector_format::VectorBlock& block,
                                        int* segments, int* packets)
{
    const auto& seq = block.point_sequence();
    const int pts = seq.points_size();
    if(pts < 2) {
        return true;
    }

    // 点序列：每个点直接标记（laser_on=1），前一个点作为 jump
    float prev_x = 0.0f;
    float prev_y = 0.0f;
    bool has_prev = false;

    for(int i = 0; i + 1 < pts; i += 2) {
        if(stop_req_.load()) {
            return false;
        }

        const float x = seq.points(i);
        const float y = seq.points(i + 1);

        if(has_prev) {
            // 先 jump 到新位置
            if(!build_and_push_packet(prev_x, prev_y, current_feed_mm_min_, 0)) {
                stop_req_.store(true);
                return false;
            }
            (*segments)++;
            (*packets)++;
        }

        // mark 当前点
        if(!build_and_push_packet(x, y, current_feed_mm_min_, 1)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;

        prev_x = x;
        prev_y = y;
        has_prev = true;
    }
    return !stop_req_.load();
}

bool OvfWorker::process_point_sequence_3d(const open_vector_format::VectorBlock& block,
                                           int* segments, int* packets)
{
    const auto& seq = block.point_sequence_3d();
    const int pts = seq.points_size();
    if(pts < 3) {
        return true;
    }

    float prev_x = 0.0f;
    float prev_y = 0.0f;
    bool has_prev = false;

    for(int i = 0; i + 2 < pts; i += 3) {
        if(stop_req_.load()) {
            return false;
        }

        const float x = seq.points(i);
        const float y = seq.points(i + 1);

        if(has_prev) {
            if(!build_and_push_packet(prev_x, prev_y, current_feed_mm_min_, 0)) {
                stop_req_.store(true);
                return false;
            }
            (*segments)++;
            (*packets)++;
        }

        if(!build_and_push_packet(x, y, current_feed_mm_min_, 1)) {
            stop_req_.store(true);
            return false;
        }
        (*segments)++;
        (*packets)++;

        prev_x = x;
        prev_y = y;
        has_prev = true;
    }
    return !stop_req_.load();
}

bool OvfWorker::process_vector_block(const open_vector_format::VectorBlock& block,
                                     int* segments, int* packets)
{
    // 先确保 marking params 已应用
    ensure_marking_params(block.marking_params_key(), packets);
    if(stop_req_.load()) {
        return false;
    }

    // 分派到具体类型处理器
    if(block.has_line_sequence()) {
        return process_line_sequence(block, segments, packets);
    }
    if(block.has_line_sequence_3d()) {
        return process_line_sequence_3d(block, segments, packets);
    }
    if(block.has__hatches()) {
        return process_hatches(block, segments, packets);
    }
    if(block.has_hatches_3d()) {
        return process_hatches_3d(block, segments, packets);
    }
    if(block.has_point_sequence()) {
        return process_point_sequence(block, segments, packets);
    }
    if(block.has_point_sequence_3d()) {
        return process_point_sequence_3d(block, segments, packets);
    }

    // 不支持的类型跳过
    UPC_LOG_OVF_WORKER(Debug,
        "skip unsupported vector block type (key=%d)",
        block.marking_params_key());
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

    UPC_LOG_OVF_WORKER(Debug,
        "processing work plane %d: z=%.3fmm (layer=%uum, blocks=%d)",
        plane_index, plane.z_pos_in_mm(), layer_um,
        plane.vector_blocks_size());

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

    // 重置 marking params 状态
    current_mark_key_ = -1;
    current_power_ = 0;
    current_feed_mm_min_ = OVF_DEFAULT_FEED_MM_MIN;

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

    UPC_LOG_OVF_WORKER(Info,
        "parse complete: segments=%d packets=%d", segments, packets);
}

int OvfWorker::open_file(const std::string& filepath)
{
    close_file();
    apply_calibration_from_context();

    job_.Clear();
    std::string err;
    if(!open_vector_format::readOfficialOvfFile(filepath, job_, &err)) {
        UPC_LOG_OVF_WORKER(Error, "ovf read failed: %s - %s", filepath.c_str(), err.c_str());
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

    // 重置 marking params 状态
    current_mark_key_ = -1;
    current_power_ = 0;
    current_feed_mm_min_ = OVF_DEFAULT_FEED_MM_MIN;

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
            "open file: %s (work_planes=%d segments=%d marking_params=%d)",
            base, job_.work_planes_size(), segments,
            job_.marking_params_map_size());
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

    current_mark_key_ = -1;
    current_power_ = 0;
    current_feed_mm_min_ = OVF_DEFAULT_FEED_MM_MIN;
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
