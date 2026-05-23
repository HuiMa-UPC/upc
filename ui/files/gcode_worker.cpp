#include "gcode_worker.hpp"
#include "job_status.hpp"
#include "protocol_mcu.hpp"
#include "pipeline_tune.hpp"
#include "gcode_parse_gains.hpp"

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
#include <climits>

/* ---------- G-code 解析状态 ---------- */
static struct {
    FILE *fp = nullptr;
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_req{false};
    std::atomic<int> total_lines{0};
    std::atomic<int> parsed_lines{0};
    std::atomic<bool> file_read_exhausted{false};
    RingBuffer *target_rb = nullptr;

    /* 当前运动状态（未出现的字段保持前值） */
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float f = 0.0f;
    int   e = 0;
    bool  state_initialized = false;

    float image_scale_gain = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
    float image_rang_gain  = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    float speed_gain       = GCODE_DEFAULT_SPEED_GAIN;
} g_w;

/*
 * 解析规则对齐 docs/Qt/gcodeparserworker.cpp (GCodeLineConverter)，使用 std::regex
 * - 空行 / 仅空白：跳过
 * - 注释：; 后整行丢弃；( 后整行丢弃
 * - M3000：^M\s*0*3000\s*$ 或 ^(.+)\bM\s*0*3000\s*$
 * - G0/G1：^G0*(\d+)；X/Y/Z/F/E 用 global regex 提取
 * - E>0 记为 1，否则 0
 */

enum LineEmitKind : uint8_t {
    LINE_EMIT_MOTION = 1,
    LINE_EMIT_LAYER  = 2,
};

struct LineParseOut {
    uint8_t kinds[2];
    int count;
};

/** 与 Qt 静态 QRegularExpression 对应，编译一次复用 */
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

static void trim_string(std::string &s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

/** Qt: trim + ';' + '(' */
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

static void line_parse_push(LineParseOut *out, LineEmitKind kind)
{
    if(out->count < 2) {
        out->kinds[out->count++] = static_cast<uint8_t>(kind);
    }
}

/** 解析一段 normalized 文本是否为 G0/G1 并更新模态；返回是否应下发运动包 */
static bool parse_g1_segment(const std::string &segment, bool *out_motion)
{
    *out_motion = false;

    std::string s = segment;
    trim_string(s);
    if(s.empty()) {
        return false;
    }

    const GcodeLineRegex &re = gcode_regex();
    std::smatch cmd_match;
    /* 与 Qt QRegularExpression::match 一致：只要求行首为 G0/G1，允许后续 X/Y/Z… */
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
            case 'X': case 'x': g_w.x = value; break;
            case 'Y': case 'y': g_w.y = value; break;
            case 'Z': case 'z': g_w.z = value; break;
            default: break;
        }
    }

    std::smatch speed_match;
    if(std::regex_search(s, speed_match, re.speed)) {
        g_w.f = std::strtof(speed_match[1].str().c_str(), nullptr);
        has_any = true;
    }

    std::smatch e_match;
    if(std::regex_search(s, e_match, re.e_param)) {
        const float e_val = std::strtof(e_match[1].str().c_str(), nullptr);
        g_w.e = (e_val > 0.0f) ? 1 : 0;
        has_any = true;
    }

    if(!g_w.state_initialized) {
        if(!has_any) {
            return false;
        }
        g_w.state_initialized = true;
    }

    *out_motion = true;
    return true;
}

/** 单行可产生 0~2 条：G1 包 + M3000 标记（顺序与 Qt 一致） */
static void parse_line_all(const char *raw_line, LineParseOut *out)
{
    out->count = 0;

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
            line_parse_push(out, LINE_EMIT_LAYER);
            return;
        }
        bool motion = false;
        if(parse_g1_segment(before, &motion) && motion) {
            line_parse_push(out, LINE_EMIT_MOTION);
        }
        line_parse_push(out, LINE_EMIT_LAYER);
        return;
    }

    if(std::regex_match(normalized, re.m3000_only)) {
        line_parse_push(out, LINE_EMIT_LAYER);
        return;
    }

    bool motion = false;
    if(parse_g1_segment(normalized, &motion) && motion) {
        line_parse_push(out, LINE_EMIT_MOTION);
    }
}

/**
 * 坐标映射到 16 位偏移（对齐 Qt GCodeLineConverter::coordinateToOffsetBinary）
 * normalized = coord / imageRangGain，offset = normalized * 32767 + 32768
 */
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
    int offset_value = static_cast<int>(normalized * 32767.0f + 32768.0f);
    if(offset_value > 65535) {
        offset_value = 65535;
    }
    if(offset_value < 0) {
        offset_value = 0;
    }

    *low_byte  = static_cast<uint8_t>(offset_value & 0xFF);
    *high_byte = static_cast<uint8_t>((offset_value >> 8) & 0xFF);
}

static void int_to_u16_bytes(int value, uint8_t *low_byte, uint8_t *high_byte)
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

/** 每次 Open/重新解析前从 job_status 读取 Control 页增益 */
static void parser_load_gains_from_job(void)
{
    JOB_LOCK();
    g_w.image_scale_gain = g_job_status.parse_image_scale_gain;
    g_w.image_rang_gain  = g_job_status.parse_image_rang_gain;
    g_w.speed_gain       = g_job_status.parse_speed_gain;
    if(g_w.image_rang_gain < 1e-6f) {
        g_w.image_rang_gain = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    if(g_w.image_scale_gain < 1e-6f) {
        g_w.image_scale_gain = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
    }
    if(g_w.speed_gain < 1e-6f) {
        g_w.speed_gain = GCODE_DEFAULT_SPEED_GAIN;
    }
}

static bool build_and_push_packet(RingBuffer *rb)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    pkt.data[0] = 0x01;

    const float ox = g_w.x * g_w.image_scale_gain;
    const float oy = g_w.y * g_w.image_scale_gain;
    const float oz = g_w.z * g_w.image_scale_gain;

    uint8_t lo = 0;
    uint8_t hi = 0;

    coordinate_to_offset_bytes(ox, g_w.image_rang_gain, &lo, &hi);
    pkt.data[1] = lo;
    pkt.data[2] = hi;

    coordinate_to_offset_bytes(oy, g_w.image_rang_gain, &lo, &hi);
    pkt.data[3] = lo;
    pkt.data[4] = hi;

    coordinate_to_offset_bytes(oz, g_w.image_rang_gain, &lo, &hi);
    pkt.data[5] = lo;
    pkt.data[6] = hi;

    int speed_int = static_cast<int>(g_w.f * g_w.speed_gain);
    int_to_u16_bytes(speed_int, &lo, &hi);
    pkt.data[7] = lo;
    pkt.data[8] = hi;

    int_to_u16_bytes(g_w.e, &lo, &hi);
    pkt.data[9]  = lo;
    pkt.data[10] = hi;

    return rb->put_wait(pkt, &g_w.stop_req);
}

static bool push_layer_mark(RingBuffer *rb)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_LAYER_MARK;
    memset(pkt.data, 0, sizeof(pkt.data));
    return rb->put_wait(pkt, &g_w.stop_req);
}

static bool pipeline_is_sending(void)
{
    JOB_LOCK();
    return g_job_status.is_sending;
}

static void sync_parse_progress(int lines, int packets)
{
    g_w.parsed_lines.store(lines);
    JOB_LOCK();
    g_job_status.parsed_lines = lines;
    g_job_status.total_packets = packets;
}

/** 预填阶段 yield；发送期连续解析（Ring 满时 put_wait 阻塞，无需 sleep） */
static void parser_chunk_yield(RingBuffer *rb)
{
    if(pipeline_is_sending()) {
        return;
    }

    int delay_ms = GCODE_PARSE_CHUNK_INTERVAL_MS;
    if(rb && rb->count() + 64 >= rb->capacity()) {
        delay_ms = GCODE_PRELOAD_IDLE_MS;
    }
    if(delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

/* ---------- 解析线程：Ring 满则 put_wait 阻塞，有空位立即续解析 ---------- */
static void parser_thread_fn(void)
{
    char line[256];
    int lines = 0;
    int packets = 0;

    if(!g_w.fp || !g_w.target_rb) {
        g_w.running.store(false);
        JOB_LOCK();
        g_job_status.is_parsing = false;
        return;
    }

    parser_load_gains_from_job();

    g_w.file_read_exhausted.store(false);
    rewind(g_w.fp);

    while(g_w.running && !g_w.stop_req.load() && !g_w.file_read_exhausted.load()) {
        job_status_update_ring();

        int chunk_lines = 0;
        int since_sync  = 0;
        const bool sending = pipeline_is_sending();
        const int chunk_limit = sending ? INT_MAX : GCODE_PARSE_CHUNK_LINES;

        while(chunk_lines < chunk_limit &&
              g_w.running && !g_w.stop_req.load() &&
              !g_w.file_read_exhausted.load()) {

            if(!fgets(line, sizeof(line), g_w.fp)) {
                g_w.file_read_exhausted.store(true);
                break;
            }

            LineParseOut parsed;
            parse_line_all(line, &parsed);
            for(int i = 0; i < parsed.count; i++) {
                if(g_w.stop_req.load()) {
                    break;
                }
                if(parsed.kinds[i] == LINE_EMIT_LAYER) {
                    if(!push_layer_mark(g_w.target_rb)) {
                        g_w.stop_req.store(true);
                        break;
                    }
                } else if(parsed.kinds[i] == LINE_EMIT_MOTION) {
                    if(!build_and_push_packet(g_w.target_rb)) {
                        g_w.stop_req.store(true);
                        break;
                    }
                    packets++;
                }
            }

            if(g_w.stop_req.load()) {
                break;
            }

            lines++;
            chunk_lines++;
            since_sync++;

            if(sending && since_sync >= GCODE_PARSE_SEND_SYNC_LINES) {
                sync_parse_progress(lines, packets);
                job_status_update_ring();
                since_sync = 0;
            }
        }

        sync_parse_progress(lines, packets);
        job_status_update_ring();
        parser_chunk_yield(g_w.target_rb);
    }

    sync_parse_progress(lines, packets);
    {
        JOB_LOCK();
        g_job_status.is_parsing = false;
    }
    job_status_update_ring();
    g_w.running.store(false);
}

/* ---------- 公共接口 ---------- */

int gcode_open_file(const char *filepath)
{
    gcode_close_file();
    g_w.fp = fopen(filepath, "r");
    if (!g_w.fp) return -1;

    /* 预读统计行数 */
    int cnt = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), g_w.fp)) cnt++;
    g_w.total_lines.store(cnt);
    g_w.parsed_lines.store(0);
    g_w.file_read_exhausted.store(false);
    rewind(g_w.fp);

    /* 重置解析状态（与 Qt resetState 一致） */
    g_w.x = g_w.y = g_w.z = g_w.f = 0.0f;
    g_w.e = 0;
    g_w.state_initialized = false;
    parser_load_gains_from_job();

    {
        char path_copy[512];
        snprintf(path_copy, sizeof(path_copy), "%s", filepath);
        const char *base = basename(path_copy);

        JOB_LOCK();
        snprintf(g_job_status.current_file, sizeof(g_job_status.current_file),
                 "%s", base);
        g_job_status.total_lines = cnt;
        g_job_status.parsed_lines = 0;
        g_job_status.job_print_completed = false;
    }
    return 0;
}

void gcode_start_parse(RingBuffer *rb)
{
    if(!g_w.fp || !rb) {
        JOB_LOCK();
        g_job_status.is_parsing = false;
        return;
    }
    /* Always join a finished/previous thread before assigning g_w.th again */
    gcode_stop_parse();

    g_w.target_rb = rb;
    g_w.stop_req.store(false);
    g_w.running.store(true);
    g_w.parsed_lines.store(0);
    g_w.file_read_exhausted.store(false);

    {
        JOB_LOCK();
        g_job_status.is_parsing = true;
        g_job_status.parsed_lines = 0;
        g_job_status.total_packets = 0;
        g_job_status.sent_packets = 0;
    }

    g_w.th = std::thread(parser_thread_fn);
}

void gcode_stop_parse(void)
{
    g_w.stop_req.store(true);
    /* Unblock parser if it is waiting on a full ring buffer */
    if(g_w.target_rb) {
        g_w.target_rb->clear();
    }
    if (g_w.th.joinable()) {
        g_w.th.join();
    }
    g_w.running.store(false);
    {
        JOB_LOCK();
        g_job_status.is_parsing = false;
    }
}

int gcode_get_total_lines(void)
{
    return g_w.total_lines.load();
}

int gcode_get_parsed_lines(void)
{
    return g_w.parsed_lines.load();
}

bool gcode_is_parsing(void)
{
    return g_w.running.load();
}

bool gcode_is_file_read_exhausted(void)
{
    return g_w.file_read_exhausted.load();
}

bool gcode_is_file_open(void)
{
    return g_w.fp != nullptr;
}

void gcode_close_file(void)
{
    gcode_stop_parse();
    if (g_w.fp) {
        fclose(g_w.fp);
        g_w.fp = nullptr;
    }
    g_w.total_lines.store(0);
    g_w.parsed_lines.store(0);
}
