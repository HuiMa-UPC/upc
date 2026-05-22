#include "gcode_worker.hpp"
#include "job_status.hpp"
#include "protocol_mcu.hpp"
#include "pipeline_tune.hpp"
#include "gcode_parse_gains.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <libgen.h>
#include <thread>
#include <atomic>
#include <chrono>

/* ---------- G-code 解析状态 ---------- */
static struct {
    FILE *fp = nullptr;
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_req{false};
    std::atomic<int> total_lines{0};
    std::atomic<int> parsed_lines{0};
    std::atomic<bool> file_read_exhausted{false};
    std::atomic<bool> buffer_full_stopped{false};
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
 * 解析规则对齐 docs/Qt/gcodeparserworker.cpp (GCodeLineConverter)
 * - 空行 / 仅空白：跳过
 * - 注释：; 后整行丢弃；( 后整行丢弃（与 Qt normalizedCommandLine 一致）
 * - M3000：整行 ^M\s*0*3000\s*$ 或行尾 …M3000（可与 G1 同行，先运动包再换层标记）
 * - G0/G1：^G0*(\d+)，仅 G0/G1；X/Y/Z/F/E 更新模态坐标；首条 G 行须含至少一个参数
 * - E>0 记为 1，否则 0（与 Qt 一致）
 */

enum LineEmitKind : uint8_t {
    LINE_EMIT_MOTION = 1,
    LINE_EMIT_LAYER  = 2,
};

struct LineParseOut {
    uint8_t kinds[2];
    int count;
};

static void trim_inplace(char *s)
{
    if(!s) {
        return;
    }
    char *start = s;
    while(*start && std::isspace(static_cast<unsigned char>(*start))) {
        start++;
    }
    if(start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while(len > 0 && std::isspace(static_cast<unsigned char>(s[len - 1]))) {
        s[--len] = '\0';
    }
}

/** Qt: trim + ';' + '(' */
static void normalize_command_line(char *line)
{
    trim_inplace(line);
    char *semi = strchr(line, ';');
    if(semi) {
        *semi = '\0';
        trim_inplace(line);
    }
    char *lparen = strchr(line, '(');
    if(lparen) {
        *lparen = '\0';
        trim_inplace(line);
    }
}

static bool is_line_empty(const char *s)
{
    return !s || s[0] == '\0';
}

/** ^M\s*0*3000\s*$ 大小写不敏感 */
static bool matches_m3000_line_only(const char *normalized)
{
    const char *p = normalized;
    while(*p && std::isspace(static_cast<unsigned char>(*p))) {
        p++;
    }
    if(*p != 'M' && *p != 'm') {
        return false;
    }
    p++;
    while(*p == ' ' || *p == '\t') {
        p++;
    }
    while(*p == '0') {
        p++;
    }
    if(strncmp(p, "3000", 4) != 0) {
        return false;
    }
    p += 4;
    while(*p && std::isspace(static_cast<unsigned char>(*p))) {
        p++;
    }
    return *p == '\0';
}

/**
 * 若行尾为 M3000，截断得到前缀并返回 true（s 被改写）
 * 例：G1 X1 Y1 M3000 -> 前缀 G1 X1 Y1
 */
static bool split_trailing_m3000(char *normalized, char **prefix_out)
{
    size_t len = strlen(normalized);
    while(len > 0 && std::isspace(static_cast<unsigned char>(normalized[len - 1]))) {
        normalized[--len] = '\0';
    }
    if(len < 5) {
        return false;
    }

    size_t pos = len;
    if(pos < 4 || strcasecmp(normalized + pos - 4, "3000") != 0) {
        return false;
    }
    pos -= 4;
    while(pos > 0 && normalized[pos - 1] == '0') {
        pos--;
    }
    while(pos > 0 && std::isspace(static_cast<unsigned char>(normalized[pos - 1]))) {
        pos--;
    }
    if(pos == 0) {
        return false;
    }
    const char m = normalized[pos - 1];
    if(m != 'M' && m != 'm') {
        return false;
    }
    pos--;

    normalized[pos] = '\0';
    trim_inplace(normalized);
    *prefix_out = normalized;
    return true;
}

static bool parse_gcode_number_prefix(const char *s, int *out_num)
{
    if(!s || (*s != 'G' && *s != 'g')) {
        return false;
    }
    const char *p = s + 1;
    while(*p == '0') {
        p++;
    }
    if(!std::isdigit(static_cast<unsigned char>(*p))) {
        return false;
    }
    int num = 0;
    while(std::isdigit(static_cast<unsigned char>(*p))) {
        num = num * 10 + (*p - '0');
        p++;
    }
    *out_num = num;
    return true;
}

static bool parse_float_param(const char *s, size_t *io_i, char axis, float *out_val)
{
    const size_t n = strlen(s);
    while(*io_i < n) {
        if(s[*io_i] == axis || s[*io_i] == static_cast<char>(toupper(axis))) {
            size_t j = *io_i + 1;
            char *endptr = nullptr;
            const float v = strtof(s + j, &endptr);
            if(endptr != s + j) {
                *out_val = v;
                *io_i = static_cast<size_t>(endptr - s);
                return true;
            }
        }
        (*io_i)++;
    }
    return false;
}

/** 解析一段 normalized 文本是否为 G0/G1 并更新模态；返回是否应下发运动包 */
static bool parse_g1_segment(char *segment, bool *out_motion)
{
    *out_motion = false;
    trim_inplace(segment);
    if(is_line_empty(segment)) {
        return false;
    }

    int gnum = -1;
    if(!parse_gcode_number_prefix(segment, &gnum)) {
        return false;
    }
    if(gnum != 0 && gnum != 1) {
        return false;
    }

    bool has_any = false;
    size_t i = 0;

    float vx = 0, vy = 0, vz = 0, vf = 0;
    if(parse_float_param(segment, &i, 'X', &vx)) {
        g_w.x = vx;
        has_any = true;
    }
    i = 0;
    if(parse_float_param(segment, &i, 'Y', &vy)) {
        g_w.y = vy;
        has_any = true;
    }
    i = 0;
    if(parse_float_param(segment, &i, 'Z', &vz)) {
        g_w.z = vz;
        has_any = true;
    }
    i = 0;
    if(parse_float_param(segment, &i, 'F', &vf)) {
        g_w.f = vf;
        has_any = true;
    }
    i = 0;
    float ve = 0;
    if(parse_float_param(segment, &i, 'E', &ve)) {
        g_w.e = (ve > 0.0f) ? 1 : 0;
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

static void line_parse_push(LineParseOut *out, LineEmitKind kind)
{
    if(out->count < 2) {
        out->kinds[out->count++] = static_cast<uint8_t>(kind);
    }
}

/** 单行可产生 0~2 条：G1 包 + M3000 标记（顺序与 Qt 一致） */
static void parse_line_all(const char *raw_line, LineParseOut *out)
{
    out->count = 0;

    char buf[512];
    strncpy(buf, raw_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    normalize_command_line(buf);

    if(is_line_empty(buf)) {
        return;
    }

    char work[512];
    strncpy(work, buf, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *prefix = nullptr;
    if(split_trailing_m3000(work, &prefix)) {
        bool motion = false;
        if(prefix && prefix[0] != '\0') {
            if(parse_g1_segment(prefix, &motion) && motion) {
                line_parse_push(out, LINE_EMIT_MOTION);
            }
        }
        line_parse_push(out, LINE_EMIT_LAYER);
        return;
    }

    if(matches_m3000_line_only(buf)) {
        line_parse_push(out, LINE_EMIT_LAYER);
        return;
    }

    char gseg[512];
    strncpy(gseg, buf, sizeof(gseg) - 1);
    gseg[sizeof(gseg) - 1] = '\0';
    bool motion = false;
    if(parse_g1_segment(gseg, &motion) && motion) {
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

static size_t ring_packets_for_pct(int pct)
{
    return (size_t)RB_MAX_PACKETS * (size_t)pct / 100U;
}

/** 发送期 55% 暂停入队；空闲预填允许至 100% */
static size_t ring_producer_pause_packets(RingBuffer *rb)
{
    if(pipeline_is_sending()) {
        return gcode_ring_high_water_mark();
    }
    if(rb) {
        return rb->capacity();
    }
    return ring_packets_for_pct(RB_PRELOAD_PAUSE_PCT);
}

static void clear_buffer_full_if_has_space(RingBuffer *rb)
{
    if(!rb) {
        return;
    }
    if(rb->count() < ring_producer_pause_packets(rb)) {
        g_w.buffer_full_stopped.store(false);
    }
}

/** 解析侧：已达暂停入队线，本行不再 push */
static bool ring_should_pause_producer(RingBuffer *rb)
{
    return rb && rb->count() >= ring_producer_pause_packets(rb);
}

/** 每块解析结束后的节流（Qt 为定时器 tick，此处为 sleep） */
static void parser_chunk_yield(RingBuffer *rb)
{
    int delay_ms = GCODE_PARSE_CHUNK_INTERVAL_MS;

    if(pipeline_is_sending()) {
        delay_ms = GCODE_PARSE_SEND_YIELD_MS;
        if(job_status_parser_should_throttle()) {
            delay_ms = GCODE_PARSE_MCU_PRESSURE_MS;
        } else if(rb && ring_should_pause_producer(rb)) {
            delay_ms = GCODE_PARSE_RING_HIGH_YIELD_MS;
        }
    } else if(rb && rb->count() + 64 >= rb->capacity()) {
        /* 空闲预填接近满时略作让步 */
        delay_ms = GCODE_PRELOAD_IDLE_MS;
    }

    if(delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

/* ---------- 解析线程（对齐 Qt onParseTimer + parseAndEnqueue） ---------- */
static void parser_thread_fn(void)
{
    char line[256];
    int lines = 0;
    int packets = 0;

    if(!g_w.fp || !g_w.target_rb) {
        return;
    }

    parser_load_gains_from_job();

    g_w.file_read_exhausted.store(false);
    g_w.buffer_full_stopped.store(false);
    rewind(g_w.fp);

    while(g_w.running && !g_w.stop_req.load() && !g_w.file_read_exhausted.load()) {
        job_status_update_ring();
        clear_buffer_full_if_has_space(g_w.target_rb);

        const int chunk_limit = pipeline_is_sending()
                                    ? GCODE_PARSE_CHUNK_LINES_SEND
                                    : GCODE_PARSE_CHUNK_LINES;

        int chunk_lines = 0;
        while(chunk_lines < chunk_limit &&
              g_w.running && !g_w.stop_req.load() &&
              !g_w.file_read_exhausted.load()) {

            if(ring_should_pause_producer(g_w.target_rb)) {
                g_w.buffer_full_stopped.store(true);
                break;
            }

            if(pipeline_is_sending() && job_status_parser_should_throttle()) {
                parser_chunk_yield(g_w.target_rb);
                continue;
            }

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
                if(ring_should_pause_producer(g_w.target_rb)) {
                    g_w.buffer_full_stopped.store(true);
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
                clear_buffer_full_if_has_space(g_w.target_rb);
            }

            if(g_w.stop_req.load() || g_w.buffer_full_stopped.load()) {
                break;
            }

            lines++;
            chunk_lines++;
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
    g_w.buffer_full_stopped.store(false);
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
    }
    return 0;
}

void gcode_start_parse(RingBuffer *rb)
{
    if (!g_w.fp || !rb) return;
    /* Always join a finished/previous thread before assigning g_w.th again */
    gcode_stop_parse();

    g_w.target_rb = rb;
    g_w.stop_req.store(false);
    g_w.running.store(true);
    g_w.parsed_lines.store(0);
    g_w.file_read_exhausted.store(false);
    g_w.buffer_full_stopped.store(false);

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

size_t gcode_ring_target_fill(void)
{
    return ring_packets_for_pct(RB_TARGET_FILL_PCT);
}

size_t gcode_ring_high_water_mark(void)
{
    return ring_packets_for_pct(RB_PRODUCER_PAUSE_PCT);
}

size_t gcode_ring_consumer_low_water(void)
{
    return ring_packets_for_pct(RB_CONSUMER_RESUME_PCT);
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
