#include "gcode_worker.hpp"
#include "parser_context.hpp"
#include "rs_log.hpp"
#include "galvo_coord.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libgen.h>
#include <regex>
#include <string>
#include <thread>
#include <unistd.h>

namespace upc::parser {

namespace {

/** 预编译正则集合，避免每行重复构造 std::regex */
struct GcodeLineRegex {
    const std::regex m3000_only; /**< ^M\s*0*3000\s*$ 换层命令 */
    const std::regex g_cmd;      /**< ^G0*(\d+) G 命令号 */
    const std::regex coord;      /**< [XYZ] 坐标参数 */
    const std::regex speed;      /**< F 进给速度 */
    const std::regex e_param;    /**< E 挤出参数 */
    const std::regex s_only;     /**< ^S\s*(\d+)\s*$ 独立功率行 */

    GcodeLineRegex()
        : m3000_only(R"(^M\s*0*3000\s*$)",
                     std::regex_constants::ECMAScript | std::regex_constants::icase)
        , g_cmd(R"(^G0*(\d+))",
                std::regex_constants::ECMAScript | std::regex_constants::icase)
        , coord(R"(([XYZ])([+-]?(?:\d+\.?\d*|\.\d+)))",
                std::regex_constants::ECMAScript | std::regex_constants::icase)
        , speed(R"([Ff]([+-]?(?:\d+\.?\d*|\.\d+)))",
                std::regex_constants::ECMAScript)
        , e_param(R"([Ee]([+-]?(?:\d+\.?\d*|\.\d+)))",
                  std::regex_constants::ECMAScript)
        , s_only(R"(^S\s*(\d+)\s*$)", std::regex_constants::ECMAScript)
    {}
};

/** @return 进程级单例正则表 */
const GcodeLineRegex& gcode_regex()
{
    static const GcodeLineRegex k_re;
    return k_re;
}

/** 原地去除字符串首尾空白字符 */
void trim_string(std::string& s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

/**
 * G-code 行规范化：去首尾空白，截断 ';' 注释与 '(' 括号注释。
 * @param line 原始行（可含 \r\n）
 * @return 可用于命令匹配的有效子串
 */
std::string normalize_command_line(const std::string& line)
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

/**
 * 解析独立功率行：整行须符合 ^S\s*(\d+)\s*$。
 * 不匹配 G-code 行内嵌的 S 参数。
 *
 * @param normalized 已去注释/空白的行
 * @param out_power  解析成功时写入功率值
 * @return 格式合法 true
 */
bool parse_standalone_s_line(const std::string& normalized, uint16_t* out_power)
{
    const GcodeLineRegex& re = gcode_regex();
    std::smatch match;
    if(!std::regex_match(normalized, match, re.s_only)) {
        return false;
    }

    const unsigned long val = std::strtoul(match[1].str().c_str(), nullptr, 10);
    if(val > 65535UL) {
        return false;
    }

    *out_power = static_cast<uint16_t>(val);
    return true;
}

/**
 * 单次顺序 read 统计文件行数（与 fgets 语义一致：末行无换行仍计一行）。
 * 用于 open_file 时填充 get_total_lines，不经过 stdio 缓冲。
 *
 * @param fd 已打开的只读文件描述符（调用后 seek 回 0）
 * @return 行数；read/lseek 失败返回 -1
 */
int count_file_newlines(int fd)
{
    if(lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    int count = 0;
    char buf[65536];
    ssize_t n = 0;
    bool seen_any = false;
    bool last_was_nl = false;

    while((n = read(fd, buf, sizeof(buf))) > 0) {
        seen_any = true;
        for(ssize_t i = 0; i < n; ++i) {
            if(buf[i] == '\n') {
                ++count;
                last_was_nl = true;
            } else {
                last_was_nl = false;
            }
        }
    }
    if(n < 0) {
        return -1;
    }
    if(seen_any && !last_was_nl) {
        ++count;
    }
    return count;
}

} // namespace

/** @return 进程内唯一 GcodeWorker 实例 */
GcodeWorker& GcodeWorker::instance()
{
    static GcodeWorker inst;
    return inst;
}

void GcodeWorker::set_total_lines_hint(int total_lines)
{
    /* >0 用 hint；0 跳过数换行；<0 打开时 count_file_newlines。 */
    total_lines_hint_ = (total_lines >= 0) ? total_lines : -1;
}

/** 析构时确保解析线程已停止且文件已关闭 */
GcodeWorker::~GcodeWorker()
{
    close_file();
}

void GcodeWorker::reset_parser_motion_state()
{
    // 打开新文件或 start_parse 前清零运动状态；Z 层厚由 last_layer_height_um_ 单独维护
    x_ = y_ = f_ = 0.0f;
    e_ = 0;
    state_initialized_ = false;
}

/** 从全局 ParserContext 同步校准表与三路增益，过小值钳位到 parser_config 默认 */
void GcodeWorker::apply_calibration_from_context()
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

/** 将线程内累计进度写入原子变量，供 get_parsed_lines / get_total_motion_packets 读取 */
void GcodeWorker::sync_parse_progress(int lines, int packets)
{
    parsed_lines_.store(lines);
    total_motion_packets_.store(packets);
}

/** 统一 RingBuffer 写入入口；stop_parse 时 stop_req_ 使 put_wait 尽快返回 */
bool GcodeWorker::put_packet(rb_packet_t pkt)
{
    return rb_.put_wait(pkt, stop_req_);
}

/**
 * 解析 G0/G1 行并更新 x_/y_/f_/e_；Z 仅更新 last_layer_height_um_，不参与运动包。
 * 首条有效 G 行必须包含至少一个坐标/速度/E 参数，否则忽略。
 */
bool GcodeWorker::try_parse_motion_line(const std::string& normalized)
{
    if(normalized.empty()) {
        return false;
    }

    const GcodeLineRegex& re = gcode_regex();
    std::smatch cmd_match;
    if(!std::regex_search(normalized, cmd_match, re.g_cmd) ||
       cmd_match.position() != 0)
    {
        return false;
    }

    const int gnum = std::atoi(cmd_match[1].str().c_str());
    if(gnum != 0 && gnum != 1) {
        return false;
    }

    bool has_any = false;

    for(std::sregex_iterator it(normalized.begin(), normalized.end(), re.coord),
            end;
        it != end;
        ++it)
    {
        const std::string axis = (*it)[1].str();
        const float value = std::strtof((*it)[2].str().c_str(), nullptr);
        has_any = true;
        switch(axis[0]) {
            case 'X': case 'x': x_ = value; break;
            case 'Y': case 'y': y_ = value; break;
            case 'Z': case 'z': {
                const uint16_t um = gcode_z_to_layer_um(value);
                if(um > 0) {
                    last_layer_height_um_ = um;
                }
                break;
            }
            default: break;
        }
    }

    std::smatch speed_match;
    if(std::regex_search(normalized, speed_match, re.speed)) {
        f_ = std::strtof(speed_match[1].str().c_str(), nullptr);
        has_any = true;
    }

    std::smatch e_match;
    if(std::regex_search(normalized, e_match, re.e_param)) {
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

    return true;
}

/**
 * 单行解析入口：规范化 → 功率(S) / 换层(M3000) / 运动(G0/G1)。
 * 每行最多产生一种 LineEvent。
 */
void GcodeWorker::parse_line_all(const char* raw_line, LineEvent* out)
{
    out->kind = LineEmitKind::None;

    const std::string normalized = normalize_command_line(raw_line);
    if(normalized.empty()) {
        return;
    }

    if(normalized[0] == 'S') {
        if(parse_standalone_s_line(normalized, &out->power)) {
            out->kind = LineEmitKind::Power;
            return;
        }
        UPC_LOG_GCODE_WORKER(Warn, "skip invalid standalone S line: %s",
                             normalized.c_str());
        return;
    }

    const GcodeLineRegex& re = gcode_regex();
    if(std::regex_match(normalized, re.m3000_only)) {
        out->kind = LineEmitKind::Layer;
        return;
    }

    if(try_parse_motion_line(normalized)) {
        out->kind = LineEmitKind::Motion;
    }
}

/**
 * 组装 11 字节运动子包：
 * data[0]=0x01, data[1..4]=X/Y 偏移, data[7..8]=速度, data[9..10]=E 标志。
 */
bool GcodeWorker::build_and_push_packet()
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    pkt.data[0] = 0x01;

    float out_x = 0.0f;
    float out_y = 0.0f;
    cal_apply_xy(&cal_data_, x_, y_, &out_x, &out_y);

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

    const int speed_int = static_cast<int>(f_ * speed_gain_);
    int_to_u16_bytes(speed_int, &lo, &hi);
    pkt.data[7] = lo;
    pkt.data[8] = hi;

    int_to_u16_bytes(e_, &lo, &hi);
    pkt.data[9]  = lo;
    pkt.data[10] = hi;

    return put_packet(pkt);
}

/** 功率包：data[0]=0x02，功率值小端写入 data[9..10] */
bool GcodeWorker::push_power_packet(uint16_t power)
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_MOTION;
    std::memset(pkt.data, 0, sizeof(pkt.data));
    pkt.data[0] = 0x02;
    pkt.data[9]  = static_cast<uint8_t>(power & 0xFF);
    pkt.data[10] = static_cast<uint8_t>(power >> 8);
    return put_packet(pkt);
}

/** 换层标记包：类型 RB_PKT_LAYER_MARK，层厚 μm 写入 data[0..1] */
bool GcodeWorker::push_layer_mark()
{
    rb_packet_t pkt;
    pkt.type = RB_PKT_LAYER_MARK;
    std::memset(pkt.data, 0, sizeof(pkt.data));
    rb_layer_mark_set_height_um(pkt, last_layer_height_um_);
    return put_packet(pkt);
}

/** 按 LineEvent 类型分发到 push_* / build_and_push_packet，失败时置 stop_req_ */
bool GcodeWorker::emit_parsed_line(const LineEvent& parsed, int* packets)
{
    if(stop_req_.load()) {
        return false;
    }

    switch(parsed.kind) {
    case LineEmitKind::None:
        return true;
    case LineEmitKind::Layer:
        if(!push_layer_mark()) {
            stop_req_.store(true);
            return false;
        }
        return true;
    case LineEmitKind::Motion:
        if(!build_and_push_packet()) {
            stop_req_.store(true);
            return false;
        }
        ++(*packets);
        return true;
    case LineEmitKind::Power:
        if(!push_power_packet(parsed.power)) {
            stop_req_.store(true);
            return false;
        }
        ++(*packets);
        return true;
    }
    return true;
}

/**
 * 解析线程：按 GCODE_PARSE_CHUNK_LINES 分块读行，每 GCODE_PARSE_SEND_SYNC_LINES 同步进度，
 * 块间 sleep GCODE_PARSE_CHUNK_INTERVAL_MS 让出 CPU。
 */
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

        while(chunk_lines < GCODE_PARSE_CHUNK_LINES &&
              !stop_req_.load(std::memory_order_relaxed))
        {
            if(!fgets(line, sizeof(line), fp_)) {
                if(feof(fp_)) {
                    file_read_exhausted_.store(true);
                }
                break;
            }

            LineEvent parsed;
            parse_line_all(line, &parsed);
            if(!emit_parsed_line(parsed, &packets)) {
                break;
            }

            lines++;
            chunk_lines++;
            if(++since_sync >= GCODE_PARSE_SEND_SYNC_LINES) {
                sync_parse_progress(lines, packets);
                since_sync = 0;
            }
        }

        if(since_sync > 0) {
            sync_parse_progress(lines, packets);
            since_sync = 0;
        }

        if(file_read_exhausted_.load(std::memory_order_relaxed) ||
           stop_req_.load(std::memory_order_relaxed)) {
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(GCODE_PARSE_CHUNK_INTERVAL_MS));
    }

    sync_parse_progress(lines, packets);
    running_.store(false);
}

/** open → 按 hint 决定是否数换行 → fdopen；成功时重置运动状态与进度计数 */
int GcodeWorker::open_file(const std::string& filepath)
{
    const int hinted_total_lines = total_lines_hint_;
    total_lines_hint_ = -1;
    close_file();
    apply_calibration_from_context();

    const int fd = open(filepath.c_str(), O_RDONLY);
    if(fd < 0) {
        return -1;
    }

    int cnt = 0;
    if(hinted_total_lines > 0) {
        cnt = hinted_total_lines;
    } else if(hinted_total_lines == 0) {
        /* 纯层进度：不整文件数换行，total_lines 保持 0。 */
        cnt = 0;
    } else {
        cnt = count_file_newlines(fd);
        if(cnt < 0) {
            close(fd);
            return -1;
        }
    }

    if(lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    fp_ = fdopen(fd, "r");
    if(!fp_) {
        close(fd);
        return -1;
    }

    reset_parser_motion_state();

    total_lines_.store(cnt);
    total_motion_packets_.store(0);
    parsed_lines_.store(0);
    file_read_exhausted_.store(false);
    last_layer_height_um_ = GCODE_DEFAULT_LAYER_UM;

    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", filepath.c_str());
    UPC_LOG_GCODE_WORKER(Info, "open file: %s (lines=%d)", basename(path_copy), cnt);
    return 0;
}

/** 先 stop_parse 再启动新线程；rewind 到文件头并重置进度 */
void GcodeWorker::start_parse()
{
    if(!fp_) {
        return;
    }
    stop_parse();

    stop_req_.store(false);
    running_.store(true);
    parsed_lines_.store(0);
    total_motion_packets_.store(0);
    file_read_exhausted_.store(false);
    rewind(fp_);

    th_ = std::thread(&GcodeWorker::parser_thread_fn, this);
}

/** 置 stop_req_、清空 RingBuffer、join 解析线程 */
void GcodeWorker::stop_parse()
{
    stop_req_.store(true);
    rb_.clear();
    if(th_.joinable()) {
        th_.join();
    }
    running_.store(false);
}

/** stop_parse + fclose，清零总行数/进度/EOF 标志 */
void GcodeWorker::close_file()
{
    stop_parse();
    if(fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
    total_lines_.store(0);
    total_motion_packets_.store(0);
    parsed_lines_.store(0);
    file_read_exhausted_.store(false);
    total_lines_hint_ = -1;
}

/** @see get_total_motion_packets */
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

uint16_t GcodeWorker::get_layer_height_um() const
{
    return last_layer_height_um_;
}

} // namespace upc::parser
