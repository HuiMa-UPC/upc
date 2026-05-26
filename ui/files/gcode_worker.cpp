#include "gcode_worker.hpp"
#include "job_status.hpp"
#include "protocol_mcu.hpp"
#include "pipeline_tune.hpp"
#include "gcode_parse.hpp"

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

    bool  cal_apply = false;
    galvo_cal_data_t cal_data;
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

static void emit_parse_kind(LineParseOut *out, uint8_t kind)
{
    if(out->count < 2) {
        out->kinds[out->count++] = kind;
    }
}

static void reset_parser_motion_state(void)
{
    g_w.x = g_w.y = g_w.z = g_w.f = 0.0f;
    g_w.e = 0;
    g_w.state_initialized = false;
}

static void apply_gain_defaults(void)
{
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

/** 去除字符串首尾空白字符 */
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

/** 将 int 截断/饱和到 16bit 后拆分为低字节和高字节 */
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
    const int offset_value = static_cast<int>(normalized * 32767.0f + 32768.0f);
    int_to_u16_bytes(offset_value, low_byte, high_byte);
}

/** 每次 Open/重新解析前从 job_status 读取 Control 页增益或 CAL 文件增益 */
static void parser_load_gains_from_job(void)
{
    bool use_cal_gains = false;

    {
        JOB_LOCK();
        use_cal_gains = g_job_status.enable_cal_apply && g_job_status.cal_file_loaded &&
                        g_job_status.cal_has_gains;
        g_w.speed_gain = use_cal_gains ? g_job_status.cal_speed_gain
                                       : g_job_status.parse_speed_gain;
    }
    gcode_get_active_image_gains(&g_w.image_scale_gain, &g_w.image_rang_gain);
    apply_gain_defaults();

    g_w.cal_apply = use_cal_gains && (galvo_cal_is_valid() != 0);
    if(g_w.cal_apply) {
        galvo_cal_copy(&g_w.cal_data);
        if(!g_w.cal_data.valid) {
            g_w.cal_apply = false;
        }
    } else {
        galvo_cal_clear(&g_w.cal_data);
    }
}

/**
 * @brief 将当前模态坐标组装成 11B 运动子包，写入 RingBuffer
 * @return true 成功（或已被 stop），false 异常
 */
static bool build_and_push_packet(RingBuffer *rb)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    pkt.data[0] = 0x01;

    float out_x = g_w.x;
    float out_y = g_w.y;
    if(g_w.cal_apply && g_w.cal_data.valid) {
        double ddx = 0.0;
        double ddy = 0.0;
        if(galvo_cal_interpolate_delta(&g_w.cal_data, g_w.x, g_w.y, &ddx, &ddy)) {
            out_x = g_w.x - static_cast<float>(ddx);
            out_y = g_w.y - static_cast<float>(ddy);
        }
    }

    {
        JOB_LOCK();
        out_x += g_job_status.center_offset_x;
        out_y += g_job_status.center_offset_y;
    }

    const float ox = out_x * g_w.image_scale_gain;
    const float oy = out_y * g_w.image_scale_gain;
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

/**
 * @brief 向 RingBuffer 写入换层标记包（RB_PKT_LAYER_MARK）
 * @return true 成功（或已被 stop），false 异常
 */
static bool push_layer_mark(RingBuffer *rb)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_LAYER_MARK;
    memset(pkt.data, 0, sizeof(pkt.data));
    return rb->put_wait(pkt, &g_w.stop_req);
}

/** @brief 同步解析进度到全局 job_status（含线程安全锁） */
static void sync_parse_progress(int lines, int packets)
{
    g_w.parsed_lines.store(lines);
    JOB_LOCK();
    g_job_status.parsed_lines = lines;
    g_job_status.total_packets = packets;
    job_status_ui_bump();
}

/**
 * @brief 解析线程 chunk 结束后的 yield
 *
 * 解析线程不受发送状态影响；仅在 RingBuffer 接近满时让出 CPU，
 * 避免在预填阶段空转占用过多 CPU。Ring 满时 put_wait() 会自动阻塞。
 */
static void parser_chunk_yield(RingBuffer *rb)
{
    int delay_ms = GCODE_PARSE_CHUNK_INTERVAL_MS;
    if(rb && rb->count() + 64 >= rb->capacity()) {
        delay_ms = GCODE_PRELOAD_IDLE_MS;
    }
    if(delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

/** 将 parse_line_all 结果写入 Ring；@return false 表示应停止解析 */
static bool emit_line_parse_result(RingBuffer *rb, const LineParseOut *parsed, int *packets)
{
    for(int i = 0; i < parsed->count; i++) {
        if(g_w.stop_req.load()) {
            return false;
        }
        if(parsed->kinds[i] == LINE_EMIT_LAYER) {
            if(!push_layer_mark(rb)) {
                g_w.stop_req.store(true);
                return false;
            }
        } else if(parsed->kinds[i] == LINE_EMIT_MOTION) {
            if(!build_and_push_packet(rb)) {
                g_w.stop_req.store(true);
                return false;
            }
            (*packets)++;
        }
    }
    return !g_w.stop_req.load();
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

static void parser_set_not_parsing(void)
{
    JOB_LOCK();
    g_job_status.is_parsing = false;
}

/**
 * @brief G-code 解析线程主函数
 *
 * 该线程由 gcode_start_parse() 创建，职责是：
 *   1. 逐行读取已打开的 G-code 文件
 *   2. 解析每行：提取 G0/G1 运动指令 和 M3000 换层标记
 *   3. 将解析结果组装成 rb_packet_t，写入 RingBuffer（g_w.target_rb）
 *   4. RingBuffer 满时 put_wait() 自动阻塞，有空位后自动续解析
 *   5. 同步解析进度（parsed_lines / total_packets）到全局 job_status
 *
 * 两级循环结构：
 *   - 外层 while：控制整体解析生命周期（running / stop_req / EOF）
 *   - 内层 while：按 chunk 批量解析（固定 GCODE_PARSE_CHUNK_LINES 批量入队）
 *
 * 与 TCP 发送线程的关系：
 *   - 解析线程是生产者 → RingBuffer → 发送线程是消费者
 *   - 两者通过 RingBuffer 解耦，解析线程不关心发送线程状态
 *   - RingBuffer 满时 put_wait() 自动阻塞，有空位后自动续解析
 *   - 解析线程按固定 chunk 批量工作，chunk 间 yield 让出 CPU
 */
static void parser_thread_fn(void)
{
    char line[256];          /* 单行 G-code 缓冲 */
    int lines = 0;           /* 累计已解析行数 */
    int packets = 0;         /* 累计已生成运动包数（不含换层标记） */

    /* ---------- 启动检查 ---------- */
    /* 文件未打开或 RingBuffer 未绑定：无法开始解析，直接退出 */
    if(!g_w.fp || !g_w.target_rb) {
        g_w.running.store(false);
        parser_set_not_parsing();
        return;
    }

    /* 从 job_status 读取 Control 页设置的解析增益（scale/range/speed） */
    parser_load_gains_from_job();

    /* 重置 EOF 标志，文件指针回到开头（支持重新解析） */
    g_w.file_read_exhausted.store(false);
    rewind(g_w.fp);

    /* ---------- 外层循环：整体解析生命周期 ---------- */
    while(g_w.running && !g_w.stop_req.load() && !g_w.file_read_exhausted.load()) {
        /* 更新 RingBuffer 水位到 job_status（供 UI 进度条显示） */
        job_status_update_ring();

        /* ---------- 内层循环：按 chunk 批量解析 ---------- */
        int chunk_lines = 0;   /* 当前 chunk 已解析行数 */
        int since_sync  = 0;   /* 距离上次同步进度的行数 */
        /* 固定 chunk 批量入队，不受发送状态影响 */
        const int chunk_limit = GCODE_PARSE_CHUNK_LINES;

        while(chunk_lines < chunk_limit &&
              g_w.running && !g_w.stop_req.load() &&
              !g_w.file_read_exhausted.load()) {

            /* 1. 从文件读取一行 */
            if(!fgets(line, sizeof(line), g_w.fp)) {
                g_w.file_read_exhausted.store(true);  /* EOF 或读取出错 */
                break;
            }

            /* 2. 解析单行：可能产生 0~2 个输出（MOTION + LAYER） */
            LineParseOut parsed;
            parse_line_all(line, &parsed);
            if(!emit_line_parse_result(g_w.target_rb, &parsed, &packets)) {
                break;
            }

            /* 4. 更新计数器 */
            lines++;
            chunk_lines++;
            since_sync++;

            /* 5. 固定间隔同步进度到 UI */
            if(since_sync >= GCODE_PARSE_SEND_SYNC_LINES) {
                sync_parse_progress(lines, packets);
                job_status_update_ring();
                since_sync = 0;
            }
        }

        /* chunk 结束：同步一次进度 */
        sync_parse_progress(lines, packets);
        job_status_update_ring();

        /* chunk 结束后让出 CPU；Ring 满时 put_wait 已阻塞，无需额外 sleep */
        parser_chunk_yield(g_w.target_rb);
    }

    /* ---------- 解析结束清理 ---------- */
    sync_parse_progress(lines, packets);
    parser_set_not_parsing();
    job_status_update_ring();
    g_w.running.store(false);
}

/* ---------- 公共接口 ---------- */

/**
 * @brief 打开并预处理 G-code 文件
 * @param filepath 完整文件路径
 * @return 0 成功，-1 失败
 *
 * 内部会先关闭旧文件，然后 fopen 并 fgets 预统计总行数，
 * 最后重置解析坐标状态机。
 */

int gcode_open_file(const char *filepath)
{
    gcode_close_file();
    g_w.fp = fopen(filepath, "r");
    if (!g_w.fp) return -1;

    const int cnt = count_file_lines(g_w.fp);
    g_w.total_lines.store(cnt);
    g_w.parsed_lines.store(0);
    g_w.file_read_exhausted.store(false);
    rewind(g_w.fp);

    reset_parser_motion_state();
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

/**
 * @brief 启动后台解析线程
 * @param rb 目标 RingBuffer，解析结果将写入此处
 *
 * 若已有线程在运行，会先调用 gcode_stop_parse() 安全结束。
 * 线程入口为 parser_thread_fn()。
 */
void gcode_start_parse(RingBuffer *rb)
{
    if(!g_w.fp || !rb) {
        parser_set_not_parsing();
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
        job_status_ui_bump();
    }

    g_w.th = std::thread(parser_thread_fn);
}

/**
 * @brief 请求停止解析线程并等待其结束
 *
 * 置位 stop_req，清空 RingBuffer 以解除 put_wait 阻塞，然后 join 线程。
 */
void gcode_stop_parse(void)
{
    g_w.stop_req.store(true);
    if(g_w.target_rb) {
        g_w.target_rb->clear();
    }
    if(g_w.th.joinable()) {
        g_w.th.join();
    }
    g_w.running.store(false);
    JOB_LOCK();
    g_job_status.is_parsing = false;
    g_job_status.parsed_lines = g_w.parsed_lines.load();
    job_status_ui_bump();
}


/** @return 文件总行数（gcode_open_file 时预统计） */
int gcode_get_total_lines(void)
{
    return g_w.total_lines.load();
}

/** @return 已解析行数（解析线程实时更新） */
int gcode_get_parsed_lines(void)
{
    return g_w.parsed_lines.load();
}

/** @return 解析线程是否仍在运行 */
bool gcode_is_parsing(void)
{
    return g_w.running.load();
}

/** @return 文件是否已读到 EOF */
bool gcode_is_file_read_exhausted(void)
{
    return g_w.file_read_exhausted.load();
}

/** @return 当前是否有已打开的文件句柄 */
bool gcode_is_file_open(void)
{
    return g_w.fp != nullptr;
}

/**
 * @brief 关闭文件并停止解析线程
 *
 * 内部调用 gcode_stop_parse() + fclose，并重置行数计数器。
 */
void gcode_close_file(void)
{
    gcode_stop_parse();
    if (g_w.fp) {
        fclose(g_w.fp);
        g_w.fp = nullptr;
    }
    g_w.total_lines.store(0);
    g_w.parsed_lines.store(0);
    g_w.file_read_exhausted.store(false);
}
