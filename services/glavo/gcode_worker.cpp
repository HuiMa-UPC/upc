#include "gcode_worker.hpp"
#include "gcode_config.hpp"
#include "cal_parser.hpp"
#include "print_job_state.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <libgen.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <regex>
#include <algorithm>
#include <cctype>

namespace services {

/* ---------- 单例 ---------- */
GcodeWorker& GcodeWorker::instance() {
    static GcodeWorker inst;
    return inst;
}

GcodeWorker::GcodeWorker() = default;
GcodeWorker::~GcodeWorker() {
    close_file();
}

/* ---------- 内部枚举与结构 ---------- */
enum LineEmitKind : uint8_t {
    LINE_EMIT_MOTION = 1,
    LINE_EMIT_LAYER  = 2,
};

struct LineParseOut {
    uint8_t kinds[2];
    int count;
};


struct GcodeLineRegex {
    const std::regex m3000_only;
    const std::regex g1_then_m3000;
    const std::regex g_cmd;
    const std::regex coord;
    const std::regex speed;
    const std::regex e_param;

    GcodeLineRegex()
        : m3000_only(R"(^M\s*0*3000\s*$)",
                     std::regex_constants::ECMAScript | std::regex_constants::icase)
        , g1_then_m3000(R"(^(.+)\bM\s*0*3000\s*$)",
                        std::regex_constants::ECMAScript | std::regex_constants::icase)
        , g_cmd(R"(^G0*(\d+))",
                std::regex_constants::ECMAScript | std::regex_constants::icase)
        , coord(R"(([XYZ])([+-]?(?:\d+\.?\d*|\.\d+)))",
                std::regex_constants::ECMAScript | std::regex_constants::icase)
        , speed(R"([Ff]([+-]?(?:\d+\.?\d*|\.\d+)))",
                std::regex_constants::ECMAScript)
        , e_param(R"([Ee]([+-]?(?:\d+\.?\d*|\.\d+)))",
                  std::regex_constants::ECMAScript)
    {}
};

static const GcodeLineRegex &gcode_regex(void)
{
    static const GcodeLineRegex k_re;
    return k_re;
}

/* ---------- 辅助函数 ---------- */
static void emit_parse_kind(LineParseOut *out, uint8_t kind)
{
    if(out->count < 2) {
        out->kinds[out->count++] = kind;
    }
}

static void trim_string(std::string &s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

static std::string normalize_command_line(const std::string &line)
{
    std::string s = line;
    trim_string(s);

    const size_t semi = s.find(';');
    if(semi != std::string::npos) {
        s = s.substr(0, semi);
        trim_string(s);
    }
    const size_t lparen = s.find('(');
    if(lparen != std::string::npos) {
        s = s.substr(0, lparen);
        trim_string(s);
    }
    return s;
}

/** 将 int 截断/饱和到 16bit 后拆分为低字节和高字节 */
static void int_to_u16_bytes(int value, uint8_t *low_byte, uint8_t *high_byte)
{
    if(value > 65535) value = 65535;
    if(value < 0)     value = 0;
    *low_byte  = static_cast<uint8_t>(value & 0xFF);
    *high_byte = static_cast<uint8_t>((value >> 8) & 0xFF);
}

/** 坐标映射到 16 位偏移 */
static void coordinate_to_offset_bytes(float coordinate_value, float image_rang_gain,
                                       uint8_t *low_byte, uint8_t *high_byte)
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

/* ---------- GcodeWorker 方法实现 ---------- */

void GcodeWorker::reset_parser_motion_state()
{
    x_ = y_ = z_ = f_ = 0.0f;
    e_ = 0;
    state_initialized_ = false;
}

void GcodeWorker::apply_gain_defaults()
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

void GcodeWorker::parser_load_gains()
{
    cal_data_clear(&cal_data_);
    enable_cal_ = false;

    if (cal_parse_csv(CAL_DEFAULT_FILE_PATH, &cal_data_) == 0 && cal_data_.valid) {
        enable_cal_ = true;
        if (cal_data_.has_gains) {
            image_scale_gain_ = cal_data_.scale_gain;
            image_rang_gain_  = cal_data_.range_gain;
            speed_gain_       = cal_data_.speed_gain;
        } else {
            image_scale_gain_ = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
            image_rang_gain_  = GCODE_DEFAULT_IMAGE_RANG_GAIN;
            speed_gain_       = GCODE_DEFAULT_SPEED_GAIN;
        }
#if SERVICES_DEBUG
        printf("[GcodeWorker] CAL loaded: scale=%.3f range=%.3f speed=%.3f grid=%dx%d\n",
               image_scale_gain_, image_rang_gain_, speed_gain_,
               cal_data_.nx, cal_data_.ny);
#endif
    } else {
        image_scale_gain_ = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
        image_rang_gain_  = GCODE_DEFAULT_IMAGE_RANG_GAIN;
        speed_gain_       = GCODE_DEFAULT_SPEED_GAIN;
#if SERVICES_DEBUG
        printf("[GcodeWorker] CAL load failed, using defaults: scale=%.3f range=%.3f speed=%.3f\n",
               image_scale_gain_, image_rang_gain_, speed_gain_);
#endif
    }
}

//上报
void GcodeWorker::sync_parse_progress(int lines, int packets)
{
    parsed_lines_.store(lines);
    total_motion_packets_.store(packets);
    PrintJobState::instance().set_total_packets(packets);

    if (progress_cb_) {
        const int total = total_lines_.load();
        int pct = 0;
        if (total > 0) {
            pct = lines * 100 / total;
            if (pct > 100) pct = 100;
        }
        const int last = progress_last_reported_.load();
        // 每 5% 变化或完成时上报，避免消息风暴
        if (pct >= 100 || pct >= last + 5) {
            progress_cb_(lines, total);
            progress_last_reported_.store(pct);
        }
    }

#if SERVICES_DEBUG
    printf("[GcodeWorker] progress %d/%d lines, packets=%d\n",
           lines, total_lines_.load(), packets);
#endif
}

void GcodeWorker::parser_set_not_parsing()
{
    // nothing to do in this architecture
    (void)0;
}

void GcodeWorker::parser_chunk_yield()
{
    // RingBuffer 不满时，避免解析线程空转占满 CPU
    // （RingBuffer 满时 put_wait 已经阻塞休眠，不会走到这里）
    std::this_thread::sleep_for(
        std::chrono::milliseconds(GCODE_PARSE_CHUNK_INTERVAL_MS));
}

/* ---------- 解析 ---------- */
bool GcodeWorker::parse_g1_segment(const std::string &segment, bool *out_motion)
{
    *out_motion = false;

    std::string s = segment;
    trim_string(s);
    if(s.empty()) {
        return false;
    }

    const GcodeLineRegex &re = gcode_regex();
    std::smatch cmd_match;
    if(!std::regex_search(s, cmd_match, re.g_cmd) || cmd_match.position() != 0) {
        return false;
    }

    const int gnum = std::atoi(cmd_match[1].str().c_str());
    if(gnum != 0 && gnum != 1) {
        return false;
    }

    bool has_any = false;

    for(std::sregex_iterator it(s.begin(), s.end(), re.coord), end; it != end; ++it) {
        const std::string axis = (*it)[1].str();
        const float value = std::strtof((*it)[2].str().c_str(), nullptr);
        has_any = true;
        switch(axis[0]) {
            case 'X': case 'x': x_ = value; break;
            case 'Y': case 'y': y_ = value; break;
            case 'Z': case 'z':
                z_ = value;
                {
                    const uint16_t um = gcode_z_to_layer_um(z_);
                    if(um > 0) {
                        last_layer_height_um_ = um;
                    }
                }
                break;
            default: break;
        }
    }

    std::smatch speed_match;
    if(std::regex_search(s, speed_match, re.speed)) {
        f_ = std::strtof(speed_match[1].str().c_str(), nullptr);
        has_any = true;
    }

    std::smatch e_match;
    if(std::regex_search(s, e_match, re.e_param)) {
        const float e_val = std::strtof(e_match[1].str().c_str(), nullptr);
        e_ = (e_val > 0.0f) ? 1 : 0;
        has_any = true;
    }

    if(!state_initialized_) {
        if(!has_any) {
            return false;
        }
        state_initialized_ = true;
    }

    *out_motion = true;
    return true;
}

void GcodeWorker::parse_line_all(const char *raw_line, LineParseOut *out)
{
    out->count = 0;
    //去除空白，注释等
    const std::string normalized = normalize_command_line(raw_line);
    if(normalized.empty()) {
        return;
    }

    const GcodeLineRegex &re = gcode_regex();
    std::smatch split_match;

    if(std::regex_match(normalized, split_match, re.g1_then_m3000)) {
        std::string before = split_match[1].str();
        trim_string(before);
        if(before.empty()) {
            emit_parse_kind(out, LINE_EMIT_LAYER);
            return;
        }
        bool motion = false;
        if(parse_g1_segment(before, &motion) && motion) {
            emit_parse_kind(out, LINE_EMIT_MOTION);
        }
        emit_parse_kind(out, LINE_EMIT_LAYER);
        return;
    }

    if(std::regex_match(normalized, re.m3000_only)) {
        emit_parse_kind(out, LINE_EMIT_LAYER);
        return;
    }

    bool motion = false;
    if(parse_g1_segment(normalized, &motion) && motion) {
        emit_parse_kind(out, LINE_EMIT_MOTION);
    }
}

/* ---------- 包组装与入队 ---------- */
bool GcodeWorker::build_and_push_packet()
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    pkt.data[0] = 0x01;

    float out_x = x_;
    float out_y = y_;

    // 校准补偿：根据校准矩阵做二维线性插补
    if (enable_cal_ && cal_data_.valid) {
        double dx = 0.0, dy = 0.0;
        if (cal_interpolate_delta(&cal_data_, x_, y_, &dx, &dy)) {
            out_x += static_cast<float>(dx);
            out_y += static_cast<float>(dy);
        }
    }

    float cal_x = out_x;
    float cal_y = out_y;

    float center_dx = center_offset_x_;
    float center_dy = center_offset_y_;
    out_x += center_dx;
    out_y += center_dy;

    const float ox = out_x * image_scale_gain_;
    const float oy = out_y * image_scale_gain_;
    const float oz = z_ * image_scale_gain_;

#if SERVICES_DEBUG
    GCODE_MOTION_COORD_DBG(
        "parse(%.4f,%.4f) cal(%.4f,%.4f) center(%.4f,%.4f) off(%.4f,%.4f) scale(%.4f,%.4f) scale_gain=%.4f",
        x_, y_, cal_x, cal_y, out_x, out_y, center_dx, center_dy, ox, oy, image_scale_gain_);
#endif

    uint8_t lo = 0;
    uint8_t hi = 0;

    coordinate_to_offset_bytes(ox, image_rang_gain_, &lo, &hi);
    pkt.data[1] = lo;
    pkt.data[2] = hi;

    coordinate_to_offset_bytes(oy, image_rang_gain_, &lo, &hi);
    pkt.data[3] = lo;
    pkt.data[4] = hi;

    coordinate_to_offset_bytes(oz, image_rang_gain_, &lo, &hi);
    pkt.data[5] = lo;
    pkt.data[6] = hi;

#if SERVICES_DEBUG
    {
        const uint16_t map_x = static_cast<uint16_t>(pkt.data[1]) |
                               (static_cast<uint16_t>(pkt.data[2]) << 8);
        const uint16_t map_y = static_cast<uint16_t>(pkt.data[3]) |
                               (static_cast<uint16_t>(pkt.data[4]) << 8);
        const uint16_t map_z = static_cast<uint16_t>(pkt.data[5]) |
                               (static_cast<uint16_t>(pkt.data[6]) << 8);
        GCODE_MOTION_COORD_DBG(
            "mapped u16 X=%u Y=%u Z=%u rang_gain=%.4f (pre_map %.4f,%.4f,%.4f)",
            (unsigned)map_x, (unsigned)map_y, (unsigned)map_z,
            image_rang_gain_, ox, oy, oz);
    }
#endif

    int speed_int = static_cast<int>(f_ * speed_gain_);
    int_to_u16_bytes(speed_int, &lo, &hi);
    pkt.data[7] = lo;
    pkt.data[8] = hi;

    int_to_u16_bytes(e_, &lo, &hi);
    pkt.data[9]  = lo;
    pkt.data[10] = hi;

    return rb_.put_wait(pkt, stop_req_);
}

bool GcodeWorker::push_layer_mark()
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_LAYER_MARK;
    std::memset(pkt.data, 0, sizeof(pkt.data));
    rb_layer_mark_set_height_um(pkt, last_layer_height_um_);
    return rb_.put_wait(pkt, stop_req_);
}

bool GcodeWorker::emit_line_parse_result(const LineParseOut *parsed, int *packets)
{
    for(int i = 0; i < parsed->count; i++) {
        if(stop_req_.load()) {
            return false;
        }
        if(parsed->kinds[i] == LINE_EMIT_LAYER) {
            if(!push_layer_mark()) {
                stop_req_.store(true);
                return false;
            }
        } else if(parsed->kinds[i] == LINE_EMIT_MOTION) {
            if(!build_and_push_packet()) {
                stop_req_.store(true);
                return false;
            }
            (*packets)++;
        }
    }
    return !stop_req_.load();
}

static int count_file_lines(FILE *fp)
{
    int cnt = 0;
    char buf[256];
    while(fgets(buf, sizeof(buf), fp)) {
        cnt++;
    }
    return cnt;
}

/* ---------- 解析线程 ---------- */
void GcodeWorker::parser_thread_fn()
{
    char line[256];
    int lines = 0;
    int packets = 0;
    int since_sync = 0;

    if(!fp_) {
        running_.store(false);
        return;
    }

    while(running_.load(std::memory_order_relaxed) &&
          !stop_req_.load(std::memory_order_relaxed))
    {
        int chunk_lines = 0;
        const int chunk_limit = GCODE_PARSE_CHUNK_LINES;

        while(chunk_lines < chunk_limit &&
              !stop_req_.load(std::memory_order_relaxed))
        {
            if(!fgets(line, sizeof(line), fp_)) {
                if(feof(fp_)) {
                    file_read_exhausted_.store(true);
                }
                // ferror 的情况直接 break，外层循环会退出
                break;
            }

            LineParseOut parsed;
            parse_line_all(line, &parsed);  //解析行
            if(!emit_line_parse_result(&parsed, &packets)) {
                break;
            }
            total_motion_packets_.store(packets);

            lines++;
            chunk_lines++;
            since_sync++;

            if(since_sync >= GCODE_PARSE_SEND_SYNC_LINES) {
                sync_parse_progress(lines, packets);
                since_sync = 0;
            }
        }

        // chunk 结束：补一次 sync（如果有未同步的进度）
        if(since_sync > 0) {
            sync_parse_progress(lines, packets);
            since_sync = 0;
        }

        // 文件读完或停止请求，退出外层循环
        if(file_read_exhausted_.load(std::memory_order_relaxed) ||
           stop_req_.load(std::memory_order_relaxed)) {
            break;
        }

        parser_chunk_yield();
    }

    sync_parse_progress(lines, packets);
    total_motion_packets_.store(packets);

    running_.store(false);
}

/* ---------- 公共接口 ---------- */

void GcodeWorker::set_progress_callback(ProgressCb cb) {
    progress_cb_ = std::move(cb);
}

void GcodeWorker::clear_progress_callback() {
    progress_cb_ = nullptr;
}

int GcodeWorker::open_file(const std::string& filepath)
{
    close_file();
    fp_ = fopen(filepath.c_str(), "r");
    if (!fp_) return -1;

    const int cnt = count_file_lines(fp_);
    total_lines_.store(cnt);
    total_motion_packets_.store(0);
    parsed_lines_.store(0);
    file_read_exhausted_.store(false);
    // rewind 放在 start_parse() 中，支持"重新解析"场景
    
    reset_parser_motion_state();
    last_layer_height_um_ = GCODE_DEFAULT_LAYER_UM;

#if SERVICES_DEBUG
    {
        char path_copy[512];
        snprintf(path_copy, sizeof(path_copy), "%s", filepath.c_str());
        const char *base = basename(path_copy);
        printf("[GcodeWorker] open file: %s (lines=%d)\n", base, cnt);
    }
#endif
    return 0;
}

void GcodeWorker::start_parse()
{
    if(!fp_) {
        parser_set_not_parsing();
        return;
    }
    stop_parse();

    stop_req_.store(false);
    running_.store(true);
    parsed_lines_.store(0);
    total_motion_packets_.store(0);
    file_read_exhausted_.store(false);
    progress_last_reported_.store(0);
    rewind(fp_);

    th_ = std::thread(&GcodeWorker::parser_thread_fn, this);
}

void GcodeWorker::stop_parse()
{
    stop_req_.store(true);
    rb_.clear();
    if(th_.joinable()) {
        th_.join();
    }
    running_.store(false);
}

void GcodeWorker::close_file()
{
    stop_parse();
    if (fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
    total_lines_.store(0);
    total_motion_packets_.store(0);
    parsed_lines_.store(0);
    file_read_exhausted_.store(false);
}

int GcodeWorker::get_total_motion_packets() const
{
    return total_motion_packets_.load();
}

int GcodeWorker::get_total_lines() const
{
    return total_lines_.load();
}

int GcodeWorker::get_parsed_lines() const
{
    return parsed_lines_.load();
}

bool GcodeWorker::is_parsing() const
{
    return running_.load();
}

bool GcodeWorker::is_file_read_exhausted() const
{
    return file_read_exhausted_.load();
}

bool GcodeWorker::is_file_open() const
{
    return fp_ != nullptr;
}

RingBuffer& GcodeWorker::ring_buffer()
{
    return rb_;
}

void GcodeWorker::set_parse_gains(float speed_gain, float scale_gain, float rang_gain)
{
    speed_gain_       = speed_gain;
    image_scale_gain_ = scale_gain;
    image_rang_gain_  = rang_gain;
    apply_gain_defaults();
}

void GcodeWorker::set_center_offset(float dx, float dy)
{
    center_offset_x_ = dx;
    center_offset_y_ = dy;
}

uint16_t GcodeWorker::get_layer_height_um() const
{
    return last_layer_height_um_;
}

} // namespace services
