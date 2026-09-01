/**
 * @file glavo_service.cpp
 * @brief Glavo 侧 IPC 业务编排：订阅 UI 请求，驱动解析/打印/门控/WiFi，并向 UI/Media 推送 Rsp/Ntf。
 *
 * =============================================================================
 * 在系统中的位置
 * =============================================================================
 *   UI ──IPC(Broker)──► GlavoService（本文件）
 *                         ├─ worker 线程：串行处理 UI 消息 + McuLink 作业事件
 *                         ├─ ParserManager：G-code/OVF → RingBuffer
 *                         ├─ McuLink（独立 link 线程）：TCP 振镜 + 串口握手/传感器
 *                         ├─ PrintVideoNotify：换层 Snap / 录像会话 → Media
 *                         ├─ WifiManager（独立线程）：wpa_supplicant
 *                         └─ usb_hotplug / clean_worker：按需辅助线程
 *
 * 跨进程回复 UI **必须** publish_async()（worker 内禁止同步 publish，以免死锁/卡 IPC）。
 * 消息契约：common/COMMON.md + upc_contract.hpp；模块说明：app/glavo/GLAVO.md。
 *
 * =============================================================================
 * 线程模型（本类相关）
 * =============================================================================
 * - MsgPort 回调线程（IPC 收包）：只做 on_message() 入队，绝不跑业务。
 * - worker_thread_：唯一业务串行点。处理顺序固定为：
 *     1) McuLink 作业事件（完成/终止/缺粉…）
 *     2) 清粉测试完成 / 转运结果（来自 link 回调置位的 pending）
 *     3) 一条 UI IPC 消息 → handle_message()
 * - usb_hotplug_thread_：轮询 /proc/mounts，变化时发 GLAVO_USB_Hotplug_Ntf。
 * - clean_worker_：开门前清粉（风机 + 轮询 PM2.5）；完成后经 publish 回 UI。
 * - WifiManager / McuLink / Parser 线程不在本文件创建，但通过回调/队列与 worker 汇合。
 *
 * =============================================================================
 * 打印主状态机（读代码时按此顺序跟）
 * =============================================================================
 *   Prepare  UI_Print_Prepare_Req
 *            → USB 复制到本地（若需要）+ 轻量扫行数 + 写 config.json
 *            → prepare_ready_ / prepared_local_path_
 *   Start    UI_Print_Start_Req（必须先 Prepare 成功）
 *            → 若上一作业仍在 terminate：deferred_print_start_，等 TerminateDone 再 launch
 *            → launch_print_from_prepared：
 *                 open_file → start_parse → PrintVideoNotify::on_print_start
 *                 → McuLink::begin_sending
 *   Progress McuLink progress_callback → send_print_progress（0–95 下发 / 95–99 排空 / 100 完成）
 *   Pause    仅停 TCP 运动下发（解析可继续；RingBuffer 满则自然阻塞）
 *   Abort    UI_Print_Terminate_Req → stop_parse + request_terminate + 录像 abort
 *   Done     McuLinkEvent::PrintCompleted / TerminateDone / PrintError → 清会话、通知 UI
 *
 * Local 文件列表：UI 直读 /home/Gcode/config.json；本服务只处理 USB 扫盘与 Prepare/Delete 写清单。
 */

#include "glavo_service.hpp"
#include "msg_port.hpp"
#include "print_video_notify.hpp"
#include "parser_config.hpp"
#include "device_config.hpp"
#include "parser_context.hpp"
#include "parser_manager.hpp"
#include "parser_registry.hpp"
#include "parser_worker.hpp"
#include "mcu_link.hpp"
#include "print_job_state.hpp"
#include "msg_runtime.hpp"
#include "buffer_pool.hpp"
#include "config/cal_config.hpp"
#include "rs_log.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace upc::glavo {

namespace parser = upc::parser;

using ::upc::GcodeManifestEntry;
using ::upc::gcode_manifest_load;
using ::upc::gcode_manifest_save;
using ::upc::gcode_manifest_upsert;
using ::upc::gcode_manifest_remove_at;

/* ============================================================================
 * 文件内辅助常量与工具函数
 *
 * 进度映射约定（与 send_print_progress 配套）：
 *   - 下发段：sent/denom → 0–95（留给 MCU 排空 95–99，避免短文件瞬间跳满）
 *   - 排空段：用 MCU cmd/data cache rate 估空闲度 → 95–99
 *   - 100%：只允许 force_complete / job_completed
 * ============================================================================ */

namespace {

/** worker IPC 队列与 link 事件队列深度；满则丢弃并打 Warn（保护内存，不阻塞 IPC 回调） */
constexpr size_t kWorkerQueueDepth = 256;
/** 排空段进度节流：rate 变化时至少间隔这么久才再推一帧（ms） */
constexpr int kDrainProgressMinMs = 200;
/**
 * 层进度锚点上限（0~95 区间）：留出余量，避免层计数先于包发送到顶时
 * 直接把下发段顶到 95，导致末段长时间停在 95。
 */
constexpr int kLayerAnchorMax95 = 90;
/** 融合结果相对包进度的最大偏离（0~95 区间），层计数异常时限制其影响 */
constexpr int kLayerFuseDeviation95 = 10;
/** Visual 无响应时必须解除打印工作线程等待，按不通过保守处理。 */
constexpr auto kFirstLayerVisualReplyTimeout = std::chrono::seconds(3);

/** Fs_DirOpen_Rsp 最大条目数（受 BufferPool inline/缓冲上限约束） */
constexpr size_t kFsDirMaxEntries =
    (16384u - sizeof(middleware::FsDirHeader)) / sizeof(middleware::FileEntryPayload);
/** USB 挂载轮询周期 */
constexpr auto kUsbHotplugPollInterval = std::chrono::milliseconds(300);

/** @brief 本地 G-code 清单配置文件路径 */
std::string local_config_path()
{
    return UPC_LOCAL_GCODE_CONFIG;
}

bool path_has_dir_prefix(const std::string& path, const std::string& dir)
{
    if(path.size() < dir.size()) {
        return false;
    }
    if(path.compare(0, dir.size(), dir) != 0) {
        return false;
    }
    return path.size() == dir.size() || path[dir.size()] == '/';
}

std::string normalize_thumb_path_for_delete(const std::string& raw)
{
    if(raw.empty()) {
        return {};
    }

    std::string path = raw;
    if(path.compare(0, 3, "A:/") == 0) {
        path.erase(0, 2); /* A:/home/... -> /home/... */
    }
    if(path.empty() || path[0] != '/') {
        return {};
    }

    const std::string local_root =
        std::filesystem::path(GLAVO_LOCAL_GCODE_DIR).lexically_normal().string();
    const std::string normalized =
        std::filesystem::path(path).lexically_normal().string();
    if(!path_has_dir_prefix(normalized, local_root)) {
        return {};
    }
    return normalized;
}

bool thumb_path_used_by_other_entries(const std::vector<GcodeManifestEntry>& entries,
                                      size_t skip_idx,
                                      const std::string& normalized_thumb_path)
{
    if(normalized_thumb_path.empty()) {
        return false;
    }
    for(size_t i = 0; i < entries.size(); ++i) {
        if(i == skip_idx) {
            continue;
        }
        const auto& e = entries[i];
        if(normalize_thumb_path_for_delete(e.thumb_small) == normalized_thumb_path ||
           normalize_thumb_path_for_delete(e.thumb_large) == normalized_thumb_path ||
           normalize_thumb_path_for_delete(e.thumb_print) == normalized_thumb_path) {
            return true;
        }
    }
    return false;
}

void try_remove_thumb_file(const std::vector<GcodeManifestEntry>& entries,
                           size_t deleted_idx,
                           const std::string& thumb_path)
{
    const std::string normalized = normalize_thumb_path_for_delete(thumb_path);
    if(normalized.empty()) {
        return;
    }
    if(thumb_path_used_by_other_entries(entries, deleted_idx, normalized)) {
        return;
    }

    std::error_code ec;
    const std::filesystem::path p(normalized);
    if(std::filesystem::is_directory(p, ec)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: skip thumb dir %s", normalized.c_str());
        return;
    }
    ec.clear();
    if(!std::filesystem::exists(p, ec)) {
        return;
    }
    ec.clear();
    if(!std::filesystem::is_regular_file(p, ec)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: skip non-regular thumb %s",
                              normalized.c_str());
        return;
    }
    ec.clear();
    if(!std::filesystem::remove(p, ec) || ec) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: remove thumb failed %s",
                              normalized.c_str());
        return;
    }
    UPC_LOG_GLAVO_SERVICE(Info, "fs_delete_ntf: removed thumb %s", normalized.c_str());
}

/** @brief 将分子/分母换算为 0–100 的整数百分比（四舍五入向下取整） */
int percent_rounded(int num, int den)
{
    if(den <= 0) {
        return 0;
    }
    if(num >= den) {
        return 100;
    }
    return num * 100 / den;
}

/** 下发段映射到 0–95：raw=sent*100/denom → raw*95/100 */
int map_send_progress_to_95(int sent_packets, int denom)
{
    const int raw = percent_rounded(sent_packets, denom);
    if(raw <= 0) {
        return 0;
    }
    if(raw >= 100) {
        return 95;
    }
    return raw * 95 / 100;
}

/**
 * 排空段 95–99：按 MCU cmd/data 缓存占用估算空闲度。
 * rate 量程与背压一致（0～65535）；未观测到非空缓存时固定 95，避免初值 0,0 冲到 99。
 */
int map_drain_progress_95_to_99(uint16_t cmd_rate, uint16_t data_rate,
                                bool rates_seen_nonempty)
{
    if(!rates_seen_nonempty) {
        return 95;
    }
    if(cmd_rate == 0 && data_rate == 0) {
        return 99;
    }
    constexpr uint32_t kFull = 65535u;
    const uint32_t avg =
        (static_cast<uint32_t>(cmd_rate) + static_cast<uint32_t>(data_rate)) / 2u;
    const uint32_t fill = avg > kFull ? kFull : avg;
    const int bump = static_cast<int>((kFull - fill) * 4u / kFull);
    return 95 + bump;
}

/** @brief 获取当前活跃的 G-code 解析 worker，无则返回 nullptr */
parser::IParserWorker* active_parser()
{
    return parser::ParserManager::instance().active();
}

/**
 * @brief 估算打印进度分母（已发送包数 / 分母 → 百分比）
 *
 * 解析未完成时用行数比例外推总包数；解析完成后分母固定为已解析包总数。
 */
int progress_denominator(int sent_packets)
{
    parser::IParserWorker* parser = active_parser();
    if(!parser) {
        return 0;
    }

    const int parsed_packets = parser->get_total_motion_packets();
    if(parsed_packets <= 0) {
        return 0;
    }

    const bool parse_done = parser->is_file_read_exhausted() && !parser->is_parsing();
    if(parse_done) {
        return parsed_packets;
    }

    /* 解析进行中保守尾量：避免把分母放大太多导致进度明显偏低。 */
    const int tail_budget = std::max(1, sent_packets / 5);
    const int fallback = parsed_packets + tail_budget;

    const int parsed_lines = parser->get_parsed_lines();
    const int total_lines  = parser->get_total_lines();
    if(parsed_lines > 0 && total_lines > parsed_lines) {
        int64_t estimated =
            (static_cast<int64_t>(parsed_packets) * total_lines) / parsed_lines;
        if(estimated > parsed_packets) {
            const int64_t min_est = static_cast<int64_t>(parsed_packets) + 1;
            const int64_t max_est = static_cast<int64_t>(parsed_packets) +
                std::max<int64_t>(tail_budget, parsed_packets / 2);
            estimated = std::clamp(estimated, min_est, max_est);
            return static_cast<int>(estimated);
        }
    }

    return fallback;
}

/** @brief 统计 G-code 中的层标记数量（命令 token: M3000） */
uint32_t count_layer_marks_m3000(const std::string& path)
{
    std::ifstream in(path);
    if(!in) {
        return 0;
    }

    uint32_t count = 0;
    std::string line;
    while(std::getline(in, line)) {
        const size_t comment_pos = line.find(';');
        const std::string_view code =
            (comment_pos == std::string::npos)
            ? std::string_view(line)
            : std::string_view(line.data(), comment_pos);

        const size_t n = code.size();
        for(size_t i = 0; i + 5 <= n; ++i) {
            const char c0 = static_cast<char>(std::toupper(
                static_cast<unsigned char>(code[i])));
            if(c0 != 'M' ||
               code[i + 1] != '3' || code[i + 2] != '0' ||
               code[i + 3] != '0' || code[i + 4] != '0') {
                continue;
            }
            const bool left_ok = (i == 0) || !std::isalnum(
                static_cast<unsigned char>(code[i - 1]));
            const bool right_ok = (i + 5 == n) || !std::isalnum(
                static_cast<unsigned char>(code[i + 5]));
            if(left_ok && right_ok) {
                ++count;
                break;
            }
        }
    }
    return count;
}

/**
 * @brief 融合包进度与层进度（均映射到 0~95）
 *
 * - 包进度决定细粒度连续性，始终是主导量；
 * - 层进度只作为参考锚点：总层数（M3000 计数）与运行时 Snap 次数定义不同，
 *   两者可能互相超前/滞后，因此层进度不得单独把进度顶上去或压死，
 *   融合结果被限制在包进度附近的有限窗口内。
 */
int fuse_send_and_layer_progress_95(int packet_pct_95,
                                    uint32_t done_layers,
                                    uint32_t total_layers)
{
    if(packet_pct_95 < 0) {
        packet_pct_95 = 0;
    } else if(packet_pct_95 > 95) {
        packet_pct_95 = 95;
    }
    if(total_layers == 0) {
        return packet_pct_95;
    }

    if(done_layers > total_layers) {
        done_layers = total_layers;
    }
    const int layer_pct_raw = static_cast<int>(
        (static_cast<uint64_t>(done_layers) * 95u) / total_layers);
    const int layer_anchor = std::min(layer_pct_raw, kLayerAnchorMax95);

    /* 7:3 保持包进度平滑主导，同时跟随层进度锚点。 */
    const int fused = (packet_pct_95 * 7 + layer_anchor * 3) / 10;
    const int lo = std::max(0, packet_pct_95 - kLayerFuseDeviation95);
    const int hi = std::min(95, packet_pct_95 + kLayerFuseDeviation95);
    return std::clamp(fused, lo, hi);
}

/** @brief float → int16_t（四舍五入 + 限幅），用于轴速度/增量、风扇占空比换算 */
int16_t debug_round_i16(float f)
{
    if(f > 32767.0f)  f = 32767.0f;
    if(f < -32768.0f) f = -32768.0f;
    return static_cast<int16_t>(f >= 0.0f ? f + 0.5f : f - 0.5f);
}

/** @brief 读取挂载点是否已挂载（按 /proc/mounts 的第二列严格匹配） */
bool is_mountpoint_mounted(const std::string& mount_point)
{
    if(mount_point.empty()) {
        return false;
    }
    std::ifstream mounts("/proc/mounts");
    if(!mounts) {
        return false;
    }
    std::string line;
    while(std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string dev;
        std::string mnt;
        if(!(iss >> dev >> mnt)) {
            continue;
        }
        if(mnt == mount_point) {
            return true;
        }
    }
    return false;
}

/** @brief 解析 USB 入口路径真实挂载点（支持 /media/udisk0 -> /mnt/udisk 软链） */
std::string usb_mount_point()
{
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(
        std::filesystem::path(GLAVO_USB_MOUNT_DIR), ec);
    if(!ec && !resolved.empty()) {
        return resolved.string();
    }
    return GLAVO_USB_MOUNT_DIR;
}

std::string sanitize_job_name(const char* raw)
{
    if(!raw || !raw[0]) {
        return "print";
    }
    std::string out(raw);
    for(char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    if(out.empty()) {
        out = "print";
    }
    return out;
}

bool is_m3000_scan_candidate(const std::string& path)
{
    const size_t dot = path.rfind('.');
    if(dot == std::string::npos) {
        return false;
    }

    std::string ext = path.substr(dot);
    for(char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".gcode" || ext == ".nc" || ext == ".gco" || ext == ".gc";
}

void make_clean_test_session_id(char* out, size_t out_len)
{
    if(!out || out_len == 0) {
        return;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::snprintf(out, out_len, "cln%lld", static_cast<long long>(ms));
}

} // namespace

/* ============================================================================
 * 静态工具
 * ============================================================================ */

/** @brief 将 Inline 类型 payload 转为字符串；非 Inline 或空 payload 返回空串 */
std::string GlavoService::payload_to_string(const middleware::MsgPayload& payload)
{
    if (payload.type != middleware::MsgPayload::Type::Inline || payload.length == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data.bytes.data()), payload.length);
}

/** @brief 拼接目录与文件名，自动处理末尾 '/' */
std::string GlavoService::make_full_path(const std::string& dir, const std::string& name)
{
    if (dir.empty()) {
        return name;
    }
    if (dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

/* ============================================================================
 * 生命周期：Idle → init() → Idle → start() → Running → stop() → Idle
 *
 * main.cpp 典型顺序：GlavoService::init →（串口打开）→ start → 主循环 port.process
 * stop/析构会 join worker、停 USB/清粉、停解析、关 WifiManager。
 * 注意：McuLink::link 线程由 main/McuLink 自己管理，不在本类 stop 里 join。
 * ============================================================================ */

GlavoService::GlavoService() = default;

/** @brief 析构时停止打印、清空会话并 join worker 线程 */
GlavoService::~GlavoService()
{
    stop_usb_hotplug_monitor();
    stop_clean_worker();
    stop_print_job();
    clear_prepare_session();
    if (state() == ServiceState::Running) {
        stop();
    }
    if (worker_thread_.joinable()) {
        worker_running_.store(false);
        worker_cv_.notify_all();
        worker_thread_.join();
    }
}

/** @brief 返回当前服务状态 */
GlavoService::ServiceState GlavoService::state() const noexcept
{
    return state_.load();
}

/** @brief 返回 Glavo 模块在 IPC 中的 ModuleId */
uint32_t GlavoService::module_id() noexcept
{
    return static_cast<uint32_t>(middleware::ModuleId::APP_GLAVO);
}

/**
 * @brief 初始化：装配置、挂回调、起 WiFi 管理器；**不起** worker（需再调 start）
 *
 * 顺序要点：
 * 1) cal.csv 覆盖门控/ACK/PM2.5/延时帧率/H264 码率等运行时开关
 * 2) Parser → RingBuffer sink 绑定到 McuLink（解析产物才能下发）
 * 3) McuLink 各类回调只「入队/置 pending」，真正业务进 worker_loop
 * 4) WifiManager::init 会拉起其后台线程（生命周期到 stop）
 *
 * @return 仅 Idle 状态下可调用，成功返回 true
 */
bool GlavoService::init()
{
    if (state() != ServiceState::Idle) return false;

    state_ = ServiceState::Initializing;

    {
        upc::config::CalConfigOptions cal_cfg{};
        if(upc::config::load_cal_config_options(upc::config::kCalDefaultPath, &cal_cfg) &&
           cal_cfg.has_config_section) {
            if(cal_cfg.has_glavo_door_real_open_enable) {
                door_real_open_enabled_ = cal_cfg.glavo_door_real_open_enable;
            }
            if(cal_cfg.has_glavo_door_real_close_check_enable) {
                door_real_close_check_enabled_ =
                    cal_cfg.glavo_door_real_close_check_enable;
            }
            if(cal_cfg.has_glavo_serial_ack_enable) {
                McuLink::instance().set_serial_ack_enabled(
                    cal_cfg.glavo_serial_ack_enable);
            }
            if(cal_cfg.has_pm25_clean_threshold) {
                pm25_clean_threshold_ = cal_cfg.pm25_clean_threshold;
            }
            if(cal_cfg.has_timelapse_video_fps) {
                timelapse_video_fps_ = cal_cfg.timelapse_video_fps;
            }
            if(cal_cfg.has_print_h264_bitrate) {
                print_h264_bitrate_ = cal_cfg.print_h264_bitrate;
            }
        }
        UPC_LOG_GLAVO_SERVICE(Info,
            "runtime cfg: door_open=%d close_check=%d serial_ack=%d pm25_clean=%.1f timelapse_fps=%u h264_bps=%u",
            door_real_open_enabled_ ? 1 : 0,
            door_real_close_check_enabled_ ? 1 : 0,
            McuLink::instance().serial_ack_enabled() ? 1 : 0,
            static_cast<double>(pm25_clean_threshold_),
            timelapse_video_fps_,
            print_h264_bitrate_);
    }

    print_h264_continuous_on_ = false; /* 上电默认关：调试页 Other → Print H264 Rec */
    clean_video_record_on_ = true;
    clean_video_session_started_ = false;
    first_layer_visual_check_on_ = false;
    env_sim_on_ = false;
    env_sim_powder_low_on_ = false;
    McuLink::instance().set_env_sim_enabled(false);
    McuLink::instance().set_env_sim_powder_low(false);
    PrintVideoNotify::instance().set_print_h264_continuous(print_h264_continuous_on_);
    PrintVideoNotify::instance().set_timelapse_fps(timelapse_video_fps_);
    PrintVideoNotify::instance().set_print_h264_bitrate(print_h264_bitrate_);
    UPC_LOG_GLAVO_SERVICE(Info, "print h264 continuous=%d clean record(debug)=%d env_sim=%d",
                          print_h264_continuous_on_ ? 1 : 0,
                          clean_video_record_on_ ? 1 : 0,
                          env_sim_on_ ? 1 : 0);

    parser::ParserContext::instance().load_calibration(CAL_DEFAULT_FILE_PATH);
    parser::ParserManager::instance().set_ring_buffer_sink(
        [](parser::RingBuffer* rb) {
            McuLink::instance().bind_ring_buffer(rb);
        });
    McuLink::instance().set_job_event_callback(
        [this](McuLinkEvent ev) { enqueue_link_event(ev); });
    McuLink::instance().set_progress_callback([this]() {
        if(print_running_.load()) {
            send_print_progress(false);
        }
    });
    McuLink::instance().set_clean_test_done_callback([this](uint8_t result_code) {
        clean_test_result_code_.store(result_code);
        clean_test_done_pending_.store(true);
        worker_cv_.notify_one();
    });
    McuLink::instance().set_transport_result_callback([this](uint8_t result_code) {
        transport_result_code_.store(result_code);
        transport_result_pending_.store(true);
        worker_cv_.notify_one();
    });
    McuLink::instance().set_first_layer_check_callback([this](uint16_t layer_height_um) {
        return request_first_layer_visual_check(layer_height_um);
    });
    McuLink::instance().set_ota_status_callback(
        [this](const middleware::McuOtaStatusPayload& st) {
            publish_mcu_ota_status(st);
        });
    McuLink::instance().set_serial_ota_status_callback(
        [this](const middleware::McuOtaStatusPayload& st) {
            publish_serial_mcu_ota_status(st);
        });
    gcode_manifest_load(local_config_path(), local_manifest_);

    /* WiFi：wpa_supplicant 控制接口；回调在 WifiManager 后台线程触发，仅做 publish。 */
    WifiManager::instance().init(
        [this](const WifiSnapshot& s) { publish_wifi_state(s); },
        [this](WifiListKind kind, const std::vector<WifiEntry>& e) {
            publish_wifi_list(kind, e);
        },
        [this](bool ok, const std::string& ssid) {
            publish_wifi_connect_result(ok, ssid);
        });

    last_usb_mounted_ = is_mountpoint_mounted(usb_mount_point());

    state_ = ServiceState::Idle;
    return true;
}

/**
 * @brief 订阅全部 UI→Glavo MsgType，启动 worker + USB 热插拔监视
 *
 * IPC 订阅回调统一落到 on_message（入队），保证业务只在 worker 串行执行。
 * 启动末尾主动推一次调试开关状态，让 UI 调试页与后端一致。
 *
 * @return 仅 Idle 状态下可调用，成功返回 true
 */
bool GlavoService::start()
{
    if (state() != ServiceState::Idle) return false;

    state_ = ServiceState::Running;

    static constexpr middleware::MsgType k_subs[] = {
        middleware::MsgType::Fs_DirOpen_Req,
        middleware::MsgType::Fs_Delete_Ntf,
        middleware::MsgType::UI_Print_Prepare_Req,
        middleware::MsgType::UI_Parser_Exit_Ntf,
        middleware::MsgType::UI_Print_Start_Req,
        middleware::MsgType::UI_Print_Terminate_Req,
        middleware::MsgType::UI_Print_Pause_Req,
        middleware::MsgType::UI_Print_Resume_Req,
        middleware::MsgType::UI_Print_Powder_Low_Ignore_Req,
        middleware::MsgType::UI_Supply_Status_Req,
        middleware::MsgType::UI_PM25_Req,
        middleware::MsgType::UI_CLOSE_DOOR_Req,
        middleware::MsgType::UI_OpenDoor_Req,
        middleware::MsgType::UI_Purify_Req,
        middleware::MsgType::UI_Clean_Start_Req,
        middleware::MsgType::UI_Clean_Stop_Req,
        middleware::MsgType::UI_Clean_Test_Start_Req,
        middleware::MsgType::UI_Clean_Test_Close_Req,
        middleware::MsgType::UI_Transport_Req,
        middleware::MsgType::UI_Mcu_Ota_Start_Req,
        middleware::MsgType::UI_Mcu_Ota_Cancel_Ntf,
        middleware::MsgType::UI_Serial_Mcu_Ota_Start_Req,
        middleware::MsgType::UI_Serial_Mcu_Ota_Cancel_Ntf,

        /* 网络 / WiFi（ConnMan） */
        middleware::MsgType::UI_Wifi_Enable_Req,
        middleware::MsgType::UI_Wifi_Scan_Req,
        middleware::MsgType::UI_Wifi_Connect_Req,
        middleware::MsgType::UI_Wifi_Forget_Req,
        middleware::MsgType::UI_Wifi_Disconnect_Req,
        middleware::MsgType::UI_Wifi_Info_Req,
        middleware::MsgType::UI_Wifi_Saved_Req,

        /* 调试页手动控制（振镜/回粉电机预留，不订阅） */
        middleware::MsgType::UI_Debug_Axis_Jog_Ntf,
        middleware::MsgType::UI_Debug_Axis_Hold_Stop_Ntf,
        middleware::MsgType::UI_Debug_Axis_SetZero_Ntf,
        middleware::MsgType::UI_Debug_Axis_Home_Ntf,
        middleware::MsgType::UI_Debug_Device_Ntf,
        middleware::MsgType::UI_Debug_Powder_Bucket_Ntf,
        middleware::MsgType::UI_Debug_Pusher_Ntf,
        middleware::MsgType::UI_Debug_Recycle_Motor_Ntf,   /* 内门（原回粉电机块） */
        middleware::MsgType::UI_Debug_Fan_Ntf,
        middleware::MsgType::UI_Debug_Fan_Duty_Ntf,
        middleware::MsgType::UI_Debug_Switch_Ntf,
        middleware::MsgType::UI_Debug_Galvo_RedLight_Ntf,
        middleware::MsgType::UI_Debug_CleanTest_Start_Ntf,
        middleware::MsgType::UI_Debug_CleanTest_Close_Ntf,
        middleware::MsgType::UI_Debug_PrintParam_Set_Ntf,
        middleware::MsgType::UI_Debug_Laser_Power_Set_Ntf,
        middleware::MsgType::UI_Debug_Laser_Power_Reset_Ntf,
        middleware::MsgType::UI_Debug_Laser_AlarmReset_Ntf,
        middleware::MsgType::UI_Debug_Print_H264_Record_Set_Ntf,
        middleware::MsgType::UI_Debug_Print_H264_Record_Get_Ntf,
        middleware::MsgType::UI_Debug_Clean_Record_Set_Ntf,
        middleware::MsgType::UI_Debug_Clean_Record_Get_Ntf,
        middleware::MsgType::UI_Debug_Env_Sim_Set_Ntf,
        middleware::MsgType::UI_Debug_Env_Sim_Get_Ntf,
        middleware::MsgType::UI_Debug_Env_Sim_Powder_Set_Ntf,
        middleware::MsgType::UI_Debug_Env_Sim_Powder_Get_Ntf,
        middleware::MsgType::UI_Debug_FirstLayer_Visual_Check_Set_Ntf,
        middleware::MsgType::UI_Debug_FirstLayer_Visual_Check_Get_Ntf,
        middleware::MsgType::APP_Visual_FirstLayer_Check_Rsp,
        middleware::MsgType::APP_Visual_FirstLayer_Reference_Ready_Rsp,

        /* Settings/About：MCU / 振镜控制板固件版本查询 */
        middleware::MsgType::UI_Mcu_Version_Ntf,
        middleware::MsgType::UI_Galvo_Version_Ntf,

        /* 消息中心：MCU 报警复位 + 快照请求 */
        middleware::MsgType::UI_Alarm_Reset_Ntf,
        middleware::MsgType::UI_Alarm_Snapshot_Ntf,
    };
    for(middleware::MsgType t : k_subs) {
        subscribe(t, [this](const middleware::Message& msg) { on_message(msg); });
    }

    worker_running_.store(true);
    worker_thread_ = std::thread(&GlavoService::worker_loop, this);
    start_usb_hotplug_monitor();
    publish_debug_print_h264_record_state();
    publish_debug_clean_record_state();
    publish_debug_env_sim_state();
    publish_debug_env_sim_powder_state();
    publish_debug_first_layer_visual_check_state();
    return true;
}

/**
 * @brief 停止 worker、清空队列与订阅，并终止进行中的打印/准备会话
 *
 * 顺序：先停 USB 监视 → join worker（此后不再处理新消息）→ 停打印/清粉 → Wifi shutdown。
 * @return 仅 Running 状态下可调用，成功返回 true
 */
bool GlavoService::stop()
{
    if (state() != ServiceState::Running) return false;

    state_ = ServiceState::Stopping;
    {
        std::lock_guard<std::mutex> lock(first_layer_visual_mtx_);
        first_layer_visual_waiting_ = false;
    }
    first_layer_reference_waiting_ = false;
    first_layer_visual_cv_.notify_all();
    stop_usb_hotplug_monitor();

    if (worker_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_running_.store(false);
        }
        worker_cv_.notify_all();
        worker_thread_.join();

        std::lock_guard<std::mutex> lock(worker_mutex_);
        while (!worker_queue_.empty()) {
            worker_queue_.pop();
        }
        link_event_queue_.clear();
    }

    stop_print_job();
    stop_clean_worker();
    clear_prepare_session();
    WifiManager::instance().shutdown();
    subscriptions_.clear();
    state_ = ServiceState::Idle;
    return true;
}

/* ============================================================================
 * 消息队列与分发
 *
 * 路径：MsgPort 回调 → on_message / enqueue_link_event → worker_cv_ 唤醒
 *      → worker_loop 按优先级排空 → handle_message / on_mcu_link_event
 * ============================================================================ */

/**
 * @brief IPC 回调入口（**非** worker 线程）：消息入队，队列满则丢弃并打日志
 *
 * 禁止在此做磁盘/串口/解析等重活；只 push + notify。
 */
void GlavoService::on_message(const middleware::Message& msg)
{
    if (state() != ServiceState::Running) return;

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (worker_queue_.size() >= kWorkerQueueDepth) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "worker queue full (%zu), drop type=%u",
                kWorkerQueueDepth,
                static_cast<unsigned>(static_cast<uint32_t>(msg.type)));
            return;
        }
        worker_queue_.push(msg);
    }
    worker_cv_.notify_one();
}

/**
 * @brief McuLink 作业事件入队（通常从 link 线程回调进来）
 *
 * 与 on_message 共用 worker_mutex_/worker_cv_；满则丢事件（极少见，需查日志）。
 */
void GlavoService::enqueue_link_event(McuLinkEvent ev)
{
    if(state() != ServiceState::Running) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if(link_event_queue_.size() >= kWorkerQueueDepth) {
            UPC_LOG_GLAVO_SERVICE(Warn, "link event queue full, drop ev=%d",
                                  static_cast<int>(ev));
            return;
        }
        link_event_queue_.push_back(ev);
    }
    worker_cv_.notify_one();
}

/**
 * @brief worker 线程主循环：本类唯一允许跑完整业务逻辑的地方
 *
 * 每次被唤醒后的处理优先级（高→低）：
 *   1. link_event_queue_ —— 打印完成/终止/错误/缺粉（先于 UI 请求，避免状态机竞态）
 *   2. clean_test_done_pending_ —— MCU 清粉测试结束（串口回调只置位）
 *   3. transport_result_pending_ —— 转运结果
 *   4. worker_queue_ 一条 IPC —— handle_message()
 *
 * 注意：部分 handler（如真实开门轮询）会阻塞本线程数十～数百 ms；
 * 期间 UI 请求仍可入队，但要等本轮处理完才会执行。
 */
void GlavoService::worker_loop()
{
    while (worker_running_.load()) {
        middleware::Message msg;
        bool has_msg = false;

        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            const auto wake_predicate = [this]() {
                return !worker_running_.load() ||
                       clean_test_done_pending_.load() ||
                       transport_result_pending_.load() ||
                       !worker_queue_.empty() ||
                       !link_event_queue_.empty();
            };
            worker_cv_.wait(lock, wake_predicate);

            if (!worker_running_.load() &&
                worker_queue_.empty() &&
                link_event_queue_.empty()) {
                break;
            }

            while (!link_event_queue_.empty() &&
                   state() == ServiceState::Running) {
                const McuLinkEvent ev = link_event_queue_.front();
                link_event_queue_.pop_front();
                lock.unlock();
                on_mcu_link_event(ev);
                lock.lock();
            }

            if(clean_test_done_pending_.load() &&
               state() == ServiceState::Running) {
                const uint8_t result_code = clean_test_result_code_.load();
                clean_test_done_pending_.store(false);
                lock.unlock();
                on_clean_test_done(result_code);
                lock.lock();
            }

            if(transport_result_pending_.load() &&
               state() == ServiceState::Running) {
                const uint8_t result_code = transport_result_code_.load();
                transport_result_pending_.store(false);
                lock.unlock();
                on_transport_result(result_code);
                lock.lock();
            }

            if (!worker_queue_.empty()) {
                msg = std::move(worker_queue_.front());
                worker_queue_.pop();
                has_msg = true;
            }
        }

        if(!has_msg) {
            continue;
        }
        if (state() == ServiceState::Running) {
            handle_message(msg);
        }
    }
}

/** @brief 异步发布 IPC；worker 内发往 UI/Media 的统一出口（禁止改成同步 publish） */
bool GlavoService::publish_async(const middleware::Message& msg)
{
    return middleware::MsgPort::instance().publish_async(msg);
}

void GlavoService::publish_clean_rsp(middleware::MsgType type, uint32_t target_id)
{
    middleware::Message rsp(type,
        static_cast<middleware::ModuleId>(module_id()),
        static_cast<middleware::ModuleId>(target_id));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "clean_rsp publish_async failed type=%u",
            static_cast<unsigned>(static_cast<uint32_t>(type)));
    }
}

void GlavoService::stop_clean_worker()
{
    clean_cancel_requested_.store(true);
    if(clean_worker_.joinable()) {
        clean_worker_.join();
    }
    clean_running_.store(false);
    clean_stop_rsp_pending_.store(false);
    clean_stop_rsp_target_.store(0);
    clean_test_done_pending_.store(false);
    clean_test_active_ = false;
    clean_video_session_started_ = false;
    clean_test_session_id_[0] = '\0';
    clean_test_job_name_[0] = '\0';
}

/** @brief 订阅指定 MsgType 并记录 Subscription 句柄，stop 时统一释放 */
void GlavoService::subscribe(middleware::MsgType type, middleware::MsgHandler handler)
{
    auto sub = middleware::MsgPort::instance().subscribe(type,
        static_cast<middleware::ModuleId>(module_id()), std::move(handler));
    subscriptions_.push_back(std::move(sub));
}

/** @brief 按 MsgType 分派到具体 handler（仅应在 worker_loop 内调用） */
void GlavoService::handle_message(const middleware::Message& msg)
{
    switch (msg.type) {
        case middleware::MsgType::Fs_DirOpen_Req:
            handle_dir_open(msg);
            break;
        case middleware::MsgType::Fs_Delete_Ntf:
            handle_fs_delete_ntf(msg);
            break;
        case middleware::MsgType::UI_Print_Prepare_Req:
            handle_print_prepare(msg);
            break;
        case middleware::MsgType::UI_Parser_Exit_Ntf:
            handle_parser_exit(msg);
            break;
        case middleware::MsgType::UI_Print_Start_Req:
            handle_print_start(msg);
            break;
        case middleware::MsgType::UI_Print_Terminate_Req:
            handle_print_terminate(msg);
            break;
        case middleware::MsgType::UI_Print_Pause_Req:
            handle_print_pause(msg);
            break;
        case middleware::MsgType::UI_Print_Resume_Req:
            handle_print_resume(msg);
            break;
        case middleware::MsgType::UI_Print_Powder_Low_Ignore_Req:
            handle_print_powder_low_ignore(msg);
            break;
        case middleware::MsgType::UI_Supply_Status_Req:
            handle_supply_status_req(msg);
            break;
        case middleware::MsgType::UI_PM25_Req:
            handle_pm25_request(msg);
            break;
        case middleware::MsgType::UI_CLOSE_DOOR_Req:
            handle_close_door_request(msg);
            break;
        case middleware::MsgType::UI_OpenDoor_Req:
            handle_open_door_request(msg);
            break;
        case middleware::MsgType::UI_Purify_Req:
            handle_purify_request(msg);
            break;
        case middleware::MsgType::UI_Clean_Start_Req:
            handle_clean_start_request(msg);
            break;
        case middleware::MsgType::UI_Clean_Stop_Req:
            handle_clean_stop_request(msg);
            break;
        case middleware::MsgType::UI_Clean_Test_Start_Req:
            handle_clean_test_start_request(msg);
            break;
        case middleware::MsgType::UI_Clean_Test_Close_Req:
            handle_clean_test_close_request(msg);
            break;
        case middleware::MsgType::UI_Transport_Req:
            handle_transport_request(msg);
            break;
        case middleware::MsgType::UI_Mcu_Ota_Start_Req:
            handle_mcu_ota_start(msg);
            break;
        case middleware::MsgType::UI_Mcu_Ota_Cancel_Ntf:
            handle_mcu_ota_cancel(msg);
            break;
        case middleware::MsgType::UI_Serial_Mcu_Ota_Start_Req:
            handle_serial_mcu_ota_start(msg);
            break;
        case middleware::MsgType::UI_Serial_Mcu_Ota_Cancel_Ntf:
            handle_serial_mcu_ota_cancel(msg);
            break;

        case middleware::MsgType::UI_Wifi_Enable_Req:
            handle_wifi_enable(msg);
            break;
        case middleware::MsgType::UI_Wifi_Scan_Req:
            handle_wifi_scan(msg);
            break;
        case middleware::MsgType::UI_Wifi_Connect_Req:
            handle_wifi_connect(msg);
            break;
        case middleware::MsgType::UI_Wifi_Forget_Req:
            handle_wifi_forget(msg);
            break;
        case middleware::MsgType::UI_Wifi_Disconnect_Req:
            handle_wifi_disconnect(msg);
            break;
        case middleware::MsgType::UI_Wifi_Info_Req:
            handle_wifi_info(msg);
            break;
        case middleware::MsgType::UI_Wifi_Saved_Req:
            handle_wifi_saved(msg);
            break;

        case middleware::MsgType::UI_Debug_Axis_Jog_Ntf:
            handle_debug_axis_jog(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_Hold_Stop_Ntf:
            handle_debug_axis_hold_stop(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_SetZero_Ntf:
            handle_debug_axis_set_zero(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_Home_Ntf:
            handle_debug_axis_home(msg);
            break;
        case middleware::MsgType::UI_Debug_Device_Ntf:
            handle_debug_device(msg);
            break;
        case middleware::MsgType::UI_Debug_Powder_Bucket_Ntf:
            handle_debug_powder_bucket(msg);
            break;
        case middleware::MsgType::UI_Debug_Pusher_Ntf:
            handle_debug_pusher(msg);
            break;
        case middleware::MsgType::UI_Debug_Recycle_Motor_Ntf:
            handle_debug_inner_door(msg);
            break;
        case middleware::MsgType::UI_Debug_Fan_Ntf:
            handle_debug_fan(msg);
            break;
        case middleware::MsgType::UI_Debug_Fan_Duty_Ntf:
            handle_debug_fan_duty(msg);
            break;
        case middleware::MsgType::UI_Debug_Switch_Ntf:
            handle_debug_switch(msg);
            break;
        case middleware::MsgType::UI_Debug_Galvo_RedLight_Ntf:
            handle_debug_galvo_red_light(msg);
            break;
        case middleware::MsgType::UI_Debug_CleanTest_Start_Ntf:
            handle_debug_clean_test_start(msg);
            break;
        case middleware::MsgType::UI_Debug_CleanTest_Close_Ntf:
            handle_debug_clean_test_close(msg);
            break;
        case middleware::MsgType::UI_Debug_PrintParam_Set_Ntf:
            handle_debug_print_param(msg);
            break;
        case middleware::MsgType::UI_Debug_Laser_Power_Set_Ntf:
            handle_debug_laser_power_set(msg);
            break;
        case middleware::MsgType::UI_Debug_Laser_Power_Reset_Ntf:
            handle_debug_laser_power_reset(msg);
            break;
        case middleware::MsgType::UI_Debug_Laser_AlarmReset_Ntf:
            handle_debug_laser_alarm_reset(msg);
            break;
        case middleware::MsgType::UI_Debug_Print_H264_Record_Set_Ntf:
            handle_debug_print_h264_record_set(msg);
            break;
        case middleware::MsgType::UI_Debug_Print_H264_Record_Get_Ntf:
            handle_debug_print_h264_record_get(msg);
            break;
        case middleware::MsgType::UI_Debug_Clean_Record_Set_Ntf:
            handle_debug_clean_record_set(msg);
            break;
        case middleware::MsgType::UI_Debug_Clean_Record_Get_Ntf:
            handle_debug_clean_record_get(msg);
            break;
        case middleware::MsgType::UI_Debug_Env_Sim_Set_Ntf:
            handle_debug_env_sim_set(msg);
            break;
        case middleware::MsgType::UI_Debug_Env_Sim_Get_Ntf:
            handle_debug_env_sim_get(msg);
            break;
        case middleware::MsgType::UI_Debug_Env_Sim_Powder_Set_Ntf:
            handle_debug_env_sim_powder_set(msg);
            break;
        case middleware::MsgType::UI_Debug_Env_Sim_Powder_Get_Ntf:
            handle_debug_env_sim_powder_get(msg);
            break;
        case middleware::MsgType::UI_Debug_FirstLayer_Visual_Check_Set_Ntf:
            handle_debug_first_layer_visual_check_set(msg);
            break;
        case middleware::MsgType::UI_Debug_FirstLayer_Visual_Check_Get_Ntf:
            handle_debug_first_layer_visual_check_get(msg);
            break;
        case middleware::MsgType::APP_Visual_FirstLayer_Check_Rsp:
            handle_visual_first_layer_check_rsp(msg);
            break;
        case middleware::MsgType::APP_Visual_FirstLayer_Reference_Ready_Rsp:
            handle_visual_first_layer_reference_ready_rsp(msg);
            break;
        case middleware::MsgType::UI_Mcu_Version_Ntf:
            handle_mcu_version_req(msg);
            break;
        case middleware::MsgType::UI_Galvo_Version_Ntf:
            handle_galvo_version_req(msg);
            break;
        case middleware::MsgType::UI_Alarm_Reset_Ntf:
            handle_alarm_reset(msg);
            break;
        case middleware::MsgType::UI_Alarm_Snapshot_Ntf:
            handle_alarm_snapshot_req(msg);
            break;
        default:
            break;
    }
}

/* ============================================================================
 * 文件系统：目录浏览与删除
 *
 * Fs_DirOpen_Req：仅扫 USB（Local 列表 UI 直读 config.json，不经本路径）。
 * Fs_Delete_Ntf：只删 Local；打印中忽略；若删的是当前 prepare/打开文件则清会话。
 * ============================================================================ */

/** @brief 回复空的 Fs_DirOpen_Rsp（无条目或出错时） */
void GlavoService::send_dir_open_empty(const middleware::Message& req)
{
    middleware::Message rsp(middleware::MsgType::Fs_DirOpen_Rsp,
        static_cast<middleware::ModuleId>(module_id()), req.sender_id);
    if (!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: publish_async empty rsp failed");
    }
}

/** @brief 扫描 USB 挂载目录，过滤支持的 G-code 后缀后组 Fs_DirOpen_Rsp */
void GlavoService::send_dir_open_usb(const middleware::Message& req, const std::string& usb_path)
{
    DIR* dir = opendir(usb_path.c_str());
    if (!dir) {
        usb_entries_.clear();
        send_dir_open_empty(req);
        return;
    }

    usb_entries_.clear();
    usb_entries_.reserve(kFsDirMaxEntries);

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (usb_entries_.size() >= kFsDirMaxEntries) {
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if(!parser::ParserRegistry::instance().match_filename(entry->d_name)) {
            continue;
        }
        usb_entries_.push_back(entry->d_name);
    }
    closedir(dir);

    const uint32_t count = static_cast<uint32_t>(usb_entries_.size());
    if(count == 0) {
        send_dir_open_empty(req);
        return;
    }

    const size_t header_size = sizeof(middleware::FsDirHeader);
    const size_t entries_size = static_cast<size_t>(count) * sizeof(middleware::FileEntryPayload);
    const size_t total_size = header_size + entries_size;

    const uint32_t buf_id = middleware::BufferPool::instance().allocate(total_size);
    if (buf_id == middleware::INVALID_BUFFER_ID) {
        send_dir_open_empty(req);
        return;
    }

    uint8_t* base = middleware::BufferPool::instance().get_ptr(buf_id);
    if (!base) {
        middleware::BufferPool::instance().release(buf_id);
        send_dir_open_empty(req);
        return;
    }

    auto* hdr = reinterpret_cast<middleware::FsDirHeader*>(base);
    hdr->count = count;
    auto* files = reinterpret_cast<middleware::FileEntryPayload*>(base + header_size);
    for (uint32_t i = 0; i < count; i++) {
        const std::string full = make_full_path(usb_path, usb_entries_[i]);
        struct stat st{};
        files[i].index = i;
        snprintf(files[i].name, sizeof(files[i].name), "%s", usb_entries_[i].c_str());
        snprintf(files[i].path, sizeof(files[i].path), "%s", full.c_str());
        files[i].duration_min = 0;
        files[i].material_g   = 0.0f;
        files[i].file_size    = 0;
        files[i].line_count   = 0;
        if(stat(full.c_str(), &st) == 0) {
            files[i].file_size = static_cast<uint32_t>(st.st_size);
        }
    }

    middleware::Message rsp(middleware::MsgType::Fs_DirOpen_Rsp,
        static_cast<middleware::ModuleId>(module_id()), req.sender_id);
    rsp.payload = middleware::MsgPayload::from_buffer(buf_id, 0, static_cast<uint32_t>(total_size));
    if (!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open usb: publish_async failed");
    }
}

/** @brief 处理 Fs_DirOpen_Req：仅 USB 扫盘（Local 列表由 UI 直读 config.json） */
void GlavoService::handle_dir_open(const middleware::Message& msg)
{
    if(!parser::ParserRegistry::instance().has_any()) {
        send_dir_open_empty(msg);
        return;
    }

    middleware::FsDirOpenReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: invalid payload");
        send_dir_open_empty(msg);
        return;
    }

    if(req.list_source != middleware::PrintFileSource::Usb) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "dir_open: ignored non-Usb source=%u (Local is UI config.json)",
            static_cast<unsigned>(req.list_source));
        send_dir_open_empty(msg);
        return;
    }

    send_dir_open_usb(msg, GLAVO_USB_MOUNT_DIR);
}

/**
 * @brief 处理 Fs_Delete_Ntf：删除本地 G-code 文件并更新 manifest
 *
 * 打印进行中忽略；若删除的是当前准备/打开的文件则清理相关会话。
 */
void GlavoService::handle_fs_delete_ntf(const middleware::Message& msg)
{
    auto send_delete_rsp = [&](const middleware::FsDeleteNtfPayload& req,
                               middleware::FsDeleteResult reason,
                               bool ok) {
        middleware::FsDeleteRspPayload rsp{};
        rsp.file_index = req.file_index;
        rsp.file_source = static_cast<uint8_t>(req.file_source);
        rsp.ok = ok ? 1 : 0;
        rsp.reason = static_cast<uint8_t>(reason);
        middleware::Message out(middleware::MsgType::APP_Fs_Delete_Rsp,
                                middleware::ModuleId::APP_GLAVO,
                                msg.sender_id);
        out.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
        (void)publish_async(out);
    };

    middleware::FsDeleteNtfPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: invalid payload");
        send_delete_rsp(req, middleware::FsDeleteResult::InvalidPayload, false);
        return;
    }
    if(req.file_source != middleware::PrintFileSource::Local) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: only Local supported");
        send_delete_rsp(req, middleware::FsDeleteResult::OnlyLocalSupported, false);
        return;
    }
    if(print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: ignored while printing");
        send_delete_rsp(req, middleware::FsDeleteResult::BusyPrinting, false);
        return;
    }

    gcode_manifest_load(local_config_path(), local_manifest_);
    const size_t idx = static_cast<size_t>(req.file_index);
    if(idx >= local_manifest_.size()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: index out of range %u", req.file_index);
        send_delete_rsp(req, middleware::FsDeleteResult::IndexOutOfRange, false);
        return;
    }

    const GcodeManifestEntry deleted_entry = local_manifest_[idx];
    const std::string name = deleted_entry.name;
    const std::string full_path = make_full_path(GLAVO_LOCAL_GCODE_DIR, name);
    if(std::remove(full_path.c_str()) != 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: remove file failed %s", full_path.c_str());
    }

    if(!gcode_manifest_remove_at(local_manifest_, idx)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: manifest remove failed");
        send_delete_rsp(req, middleware::FsDeleteResult::ManifestUpdateFailed, false);
        return;
    }
    if(!gcode_manifest_save(local_config_path(), local_manifest_)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: config save failed");
        gcode_manifest_load(local_config_path(), local_manifest_);
        send_delete_rsp(req, middleware::FsDeleteResult::ManifestUpdateFailed, false);
        return;
    }

    try_remove_thumb_file(local_manifest_, idx, deleted_entry.thumb_small);
    try_remove_thumb_file(local_manifest_, idx, deleted_entry.thumb_large);
    try_remove_thumb_file(local_manifest_, idx, deleted_entry.thumb_print);

    if(prepared_local_path_ == full_path) {
        clear_prepare_session();
    }
    if(current_file_path_ == full_path) {
        current_file_path_.clear();
        if(parser::IParserWorker* worker = active_parser()) {
            if(worker->is_file_open()) {
                worker->close_file();
            }
        }
    }

    UPC_LOG_GLAVO_SERVICE(Info, "fs_delete_ntf: removed %s", name.c_str());
    send_delete_rsp(req, middleware::FsDeleteResult::Ok, true);
}

/** @brief 将 USB 源文件二进制复制到本地目标路径 */
bool GlavoService::copy_usb_to_local(const std::string& src, const std::string& dst)
{
    std::ifstream in(src, std::ios::binary);
    if(!in) {
        return false;
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if(!out) {
        return false;
    }
    out << in.rdbuf();
    return static_cast<bool>(out);
}

/**
 * @brief 轻量扫描 G-code 行数（打开→读 total_lines→关闭，不启动完整解析）
 * @param line_count 成功时写入行数
 */
bool GlavoService::light_scan_line_count(const std::string& path, uint32_t& line_count)
{
    line_count = 0;
    if(parser::ParserManager::instance().open_file(path) != 0) {
        return false;
    }
    parser::IParserWorker* worker = active_parser();
    if(!worker) {
        return false;
    }
    line_count = static_cast<uint32_t>(worker->get_total_lines());
    worker->close_file();
    return true;
}

/* ============================================================================
 * 打印流程：准备、启动、终止与进度
 *
 * Prepare / Start 拆开的原因：
 *   - Prepare 可能做 USB 拷贝 + 扫行数 + 写 manifest（偏慢、可失败）
 *   - Start 才真正开解析线程并让 McuLink 开始吃 RingBuffer
 *   - UI 可在 Prepare 成功后再走门控/清粉，再发 Start
 *
 * 关键成员：
 *   prepare_ready_ / prepared_local_path_ / prepared_line_count_
 *   print_running_          —— 业务层“正在印”（与 McuLink::is_sending 不完全等同）
 *   deferred_print_start_   —— 上一作业 terminate 未完又收到 Start，延后到 TerminateDone
 *   ignore_powder_low_...   —— 用户点“忽略缺粉”后，本作业不再自动暂停
 * ============================================================================ */

/** @brief 清除 print prepare 会话状态（prepared_local_path_ / prepare_ready_） */
void GlavoService::clear_prepare_session()
{
    if(prepared_parser_open_) {
        if(parser::IParserWorker* worker = active_parser()) {
            if(worker->is_file_open()) {
                worker->close_file();
            }
        }
    }
    prepared_parser_open_ = false;
    prepared_local_path_.clear();
    prepared_line_count_ = 0;
    prepared_layer_count_ = 0;
    print_total_layers_ = 0;
    prepare_ready_ = false;
}

/**
 * @brief 执行打印准备：USB 复制、轻量扫描、更新 manifest
 * @param local_path_out 成功时写入本地绝对路径
 */
bool GlavoService::run_print_prepare(middleware::PrintFileSource source, uint32_t index,
                                     std::string& local_path_out)
{
    local_path_out.clear();
    prepared_parser_open_ = false;
    stop_print_job();
    if(parser::IParserWorker* worker = active_parser()) {
        if(worker->is_file_open()) {
            worker->close_file();
        }
    }
    current_file_path_.clear();

    if(source == middleware::PrintFileSource::Local) {
        gcode_manifest_load(local_config_path(), local_manifest_);
    }

    std::string basename;
    if(source == middleware::PrintFileSource::Usb) {
        if(index >= usb_entries_.size()) {
            return false;
        }
        basename = usb_entries_[index];
        const std::string src = make_full_path(GLAVO_USB_MOUNT_DIR, basename);
        const std::string dst = make_full_path(GLAVO_LOCAL_GCODE_DIR, basename);
        if(!copy_usb_to_local(src, dst)) {
            UPC_LOG_GLAVO_SERVICE(Warn, "prepare: usb copy failed %s", src.c_str());
            return false;
        }
        local_path_out = dst;
    } else {
        if(index >= local_manifest_.size()) {
            return false;
        }
        basename = local_manifest_[index].name;
        local_path_out = make_full_path(GLAVO_LOCAL_GCODE_DIR, basename);
    }

    struct stat st{};
    if(stat(local_path_out.c_str(), &st) != 0) {
        return false;
    }

    uint32_t line_count = 0;
    uint32_t layer_count = 0;
    if(source == middleware::PrintFileSource::Local) {
        const auto& e = local_manifest_[index];
        const bool same_mtime = e.mtime == static_cast<int64_t>(st.st_mtime);
        const bool same_size =
            e.file_size == static_cast<uint32_t>(st.st_size);
        if(same_mtime && same_size) {
            if(e.line_count > 0) {
                line_count = e.line_count;
            }
            if(e.layer_count > 0) {
                layer_count = e.layer_count;
            }
        }
    }
    const int line_hint = line_count > 0 ? static_cast<int>(line_count) : -1;
    if(parser::ParserManager::instance().open_file(local_path_out, line_hint) != 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "prepare: open failed %s", local_path_out.c_str());
        return false;
    }
    if(parser::IParserWorker* worker = active_parser()) {
        if(!worker->is_file_open()) {
            UPC_LOG_GLAVO_SERVICE(Warn, "prepare: worker not open %s", local_path_out.c_str());
            return false;
        }
        if(line_count == 0) {
            line_count = static_cast<uint32_t>(std::max(worker->get_total_lines(), 0));
        }
    } else {
        UPC_LOG_GLAVO_SERVICE(Warn, "prepare: no active parser %s", local_path_out.c_str());
        return false;
    }
    prepared_parser_open_ = true;

    GcodeManifestEntry entry;
    entry.name        = basename;
    entry.file_size   = static_cast<uint32_t>(st.st_size);
    entry.line_count  = line_count;
    if(layer_count == 0 && is_m3000_scan_candidate(local_path_out)) {
        layer_count = count_layer_marks_m3000(local_path_out);
    }
    entry.layer_count = layer_count;
    entry.duration_sec = 0;
    entry.material_g  = 0.f;
    entry.mtime       = static_cast<int64_t>(st.st_mtime);

    if(source == middleware::PrintFileSource::Usb) {
        gcode_manifest_load(local_config_path(), local_manifest_);
    }
    gcode_manifest_upsert(local_manifest_, entry);
    if(!gcode_manifest_save(local_config_path(), local_manifest_)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "prepare: config save failed");
        return false;
    }

    prepared_local_path_ = local_path_out;
    prepared_line_count_ = line_count;
    prepared_layer_count_ = layer_count;
    prepare_ready_ = true;
    UPC_LOG_GLAVO_SERVICE(Info,
        "prepare stats: lines=%u layers=%u path=%s",
        static_cast<unsigned>(prepared_line_count_),
        static_cast<unsigned>(prepared_layer_count_),
        local_path_out.c_str());
    return true;
}

/** @brief 处理 UI_Print_Prepare_Req 并回复 APP_Print_Prepare_Rsp */
void GlavoService::handle_print_prepare(const middleware::Message& msg)
{
    middleware::PrintPrepareReqPayload req{};
    middleware::PrintPrepareRspPayload rsp{};
    rsp.ok = 0;

    if (!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_prepare: invalid payload");
    } else {
        std::string local_path;
        if(run_print_prepare(req.file_source, req.file_index, local_path)) {
            rsp.ok = 1;
            UPC_LOG_GLAVO_SERVICE(Info, "print_prepare ok src=%u idx=%u path=%s",
                static_cast<unsigned>(req.file_source), req.file_index, local_path.c_str());
        } else {
            clear_prepare_session();
            UPC_LOG_GLAVO_SERVICE(Warn, "print_prepare failed src=%u idx=%u",
                static_cast<unsigned>(req.file_source), req.file_index);
        }
    }

    middleware::Message out(middleware::MsgType::APP_Print_Prepare_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    if (!publish_async(out)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_prepare_rsp publish_async failed");
    }
}

/** @brief 处理 UI_Parser_Exit_Ntf：非打印状态下停止解析并清空会话 */
void GlavoService::handle_parser_exit(const middleware::Message& msg)
{
    (void)msg;
    if(print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "parser_exit ignored while printing");
        return;
    }
    UPC_LOG_GLAVO_SERVICE(Info, "parser_exit: stop parse and clear session");
    stop_print_job();
    if(parser::IParserWorker* worker = active_parser()) {
        if(worker->is_file_open()) {
            worker->close_file();
        }
    }
    current_file_path_.clear();
    clear_prepare_session();
}

/**
 * @brief 在已 Prepare 的路径上真正启动一次打印作业
 *
 * 调用前须保证：prepare_ready_、路径有效，且 McuLink 不在 terminate 清理中。
 * 步骤：open_file(带行数 hint) → 重置计数/进度 → start_parse → 通知 Media 录像会话
 *      → begin_sending（link 线程开始从 RingBuffer 抽运动包）。
 */
void GlavoService::launch_print_from_prepared(const std::string& full_path)
{
    /* 起印前强制切回激光模式（串口开激光 + TCP 关红光），确保后续打印控制在预期态。 */
    (void)apply_red_laser_mode(false, false, "before_print_start");

    bool parser_ready = false;
    if(prepared_parser_open_) {
        if(parser::IParserWorker* worker = active_parser()) {
            parser_ready = worker->is_file_open();
        }
    }

    if(!parser_ready) {
        const int line_hint = prepared_line_count_ > 0
            ? static_cast<int>(prepared_line_count_)
            : -1;
        if(parser::ParserManager::instance().open_file(full_path, line_hint) != 0) {
            UPC_LOG_GLAVO_SERVICE(Error, "print_start: open failed path=%s",
                                  full_path.c_str());
            print_running_.store(false);
            prepared_parser_open_ = false;
            return;
        }
    }
    prepared_parser_open_ = false;
    current_file_path_ = full_path;
    PrintJobState::instance().reset_job_counters();
#if GLAVO_ENABLE_LAYER_SNAP
    print_total_layers_ = prepared_layer_count_;
    if(print_total_layers_ == 0 && is_m3000_scan_candidate(full_path)) {
        print_total_layers_ = count_layer_marks_m3000(full_path);
    }
#else
    /* 未开启换层 Snap 时没有层完成事件，层进度恒为 0，只能退化为纯包进度。 */
    print_total_layers_ = 0;
#endif
    reset_print_progress_state();
    parser::ParserManager::instance().start_parse();

    ignore_powder_low_for_current_print_ = false;
    print_running_.store(true);
    McuLink::instance().set_print_task_flow_active(true);
    publish_print_state_ntf(static_cast<uint32_t>(middleware::UserHomeStatus::Printing));
    if(first_layer_visual_check_on_) {
        first_layer_reference_waiting_ = true;
        if(!publish_first_layer_visual_session(true, "print_start")) {
            first_layer_reference_waiting_ = false;
            UPC_LOG_GLAVO_SERVICE(Warn,
                "reference session start publish failed; continue print without reference");
            begin_print_motion_after_reference_ready();
            UPC_LOG_GLAVO_SERVICE(Info, "print_start: path=%s", full_path.c_str());
            return;
        }
        UPC_LOG_GLAVO_SERVICE(Info,
            "print_start waiting for same-session bare reference path=%s",
            full_path.c_str());
        worker_cv_.notify_all();
        return;
    }

    begin_print_motion_after_reference_ready();

    UPC_LOG_GLAVO_SERVICE(Info, "print_start: path=%s total_layers=%u",
        full_path.c_str(), static_cast<unsigned>(print_total_layers_));
}

void GlavoService::begin_print_motion_after_reference_ready()
{
    if(!print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "print motion start ignored: print is no longer active");
        return;
    }
    first_layer_reference_waiting_ = false;
    PrintVideoNotify::instance().on_print_start(current_file_path_);
    McuLink::instance().begin_sending();
    UPC_LOG_GLAVO_SERVICE(Info,
        "print motion started reference_ready=%d path=%s",
        first_layer_visual_check_on_ ? 1 : 0, current_file_path_.c_str());
}

/**
 * @brief 处理 UI_Print_Start_Req：在已 prepare 的文件上启动解析与 McuLink 发送
 *
 * 若 link 仍在发送或 terminate 清理中：只 request_terminate + 置 deferred_print_start_，
 * 真正 launch 推迟到 on_mcu_link_event(TerminateDone/PrintError)。
 */
void GlavoService::handle_print_start(const middleware::Message& msg)
{
    middleware::PrintStartReqPayload req{};
    if (!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_start: invalid payload");
        return;
    }

    if(!prepare_ready_ || prepared_local_path_.empty()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_start: no prepared session");
        return;
    }

    (void)req.file_index;
    (void)req.file_source;

    const std::string& full_path = prepared_local_path_;

    stop_print_job();
    auto& link = McuLink::instance();
    if(link.is_sending() || link.is_terminate_pending()) {
        /* 异步 terminate 完成前不得 open/begin，否则清理会清掉新作业 */
        link.request_terminate();
        deferred_print_start_ = true;
        UPC_LOG_GLAVO_SERVICE(Info,
            "print_start deferred until terminate done path=%s",
            full_path.c_str());
        return;
    }

    deferred_print_start_ = false;
    launch_print_from_prepared(full_path);
}

/** @brief 停止解析线程并将 print_running_ 置 false（不请求 Mcu 终止） */
void GlavoService::stop_print_job()
{
    first_layer_reference_waiting_ = false;
    print_running_.store(false);
    publish_first_layer_visual_session(false, "print_stop");
    parser::ParserManager::instance().stop_parse();
}

/** @brief 处理 UI_Print_Terminate_Req：停止解析并请求 McuLink 终止作业 */
void GlavoService::handle_print_terminate(const middleware::Message& msg)
{
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Info, "print_terminate requested");
    if(!print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Debug, "print_terminate ignored: not running");
        publish_terminate_done();
        return;
    }
    deferred_print_start_ = false;
    first_layer_reference_waiting_ = false;
    print_running_.store(false);
    {
        std::lock_guard<std::mutex> lock(first_layer_visual_mtx_);
        first_layer_visual_waiting_ = false;
        first_layer_visual_result_ = 0;
    }
    first_layer_visual_cv_.notify_all();
    publish_first_layer_visual_session(false, "user_terminate");
    McuLink::instance().set_user_dispatch_paused(false);
    PrintVideoNotify::instance().on_print_abort();
    parser::ParserManager::instance().stop_parse();
    McuLink::instance().request_terminate();
    clear_prepare_session();
}

/** @brief 处理 UI_Print_Pause_Req：暂停 TCP 运动下发（解析继续，RingBuffer 满则自然阻塞） */
void GlavoService::handle_print_pause(const middleware::Message& msg)
{
    (void)msg;
    if(!print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Debug, "print_pause ignored: not running");
        publish_print_pause_done(false);
        return;
    }
    McuLink& link = McuLink::instance();
    if(link.is_user_dispatch_paused()) {
        publish_print_pause_done(true);
        return;
    }
    link.set_user_dispatch_paused(true);
    if(!link.send_tcp_emergency_ctrl(false, true)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_pause: tcp 0x0A pause send failed");
    }
    UPC_LOG_GLAVO_SERVICE(Info, "print_pause accepted");
    publish_print_pause_done(true);
}

/** @brief 处理 UI_Print_Resume_Req：恢复 TCP 运动下发 */
void GlavoService::handle_print_resume(const middleware::Message& msg)
{
    (void)msg;
    if(!print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Debug, "print_resume ignored: not running");
        publish_print_pause_done(false);
        return;
    }
    McuLink& link = McuLink::instance();
    if(!link.is_user_dispatch_paused()) {
        publish_print_pause_done(false);
        return;
    }
    if(!link.send_tcp_emergency_ctrl(false, false)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_resume: tcp 0x0A resume send failed");
    }
    link.set_user_dispatch_paused(false);
    UPC_LOG_GLAVO_SERVICE(Info, "print_resume accepted");
    publish_print_pause_done(false);
}

/**
 * @brief 用户忽略缺粉告警：本作业内不再因 PowderLow 自动暂停
 *
 * 仅影响当前 print_running_ 会话；新作业在 launch 时会清零该标志。
 */
void GlavoService::handle_print_powder_low_ignore(const middleware::Message& msg)
{
    (void)msg;
    if(!print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Debug, "powder_low_ignore ignored: not running");
        return;
    }
    ignore_powder_low_for_current_print_ = true;
    UPC_LOG_GLAVO_SERVICE(Warn, "powder_low_ignore enabled for current print");
}

/** @brief 重置进度推送的去重状态（last_print_pct_ / last_parse_done_） */
void GlavoService::reset_print_progress_state()
{
    last_print_pct_        = -1;
    last_parse_done_       = false;
    last_sent_cmd_rate_    = 0xffff;
    last_sent_data_rate_   = 0xffff;
    last_drain_progress_at_ = {};
}

/**
 * @brief 计算并推送 APP_Print_Progress_Ntf
 * @param force_complete true 时强制 100%（打印完成事件）
 *
 * 两段式进度（防短文件瞬间 99%、区分发送与排空）：
 * - 下发段 0～95：sent_packets / denom 线性映射；
 * - 排空段 95～99：natural_complete_pending 时按 MCU 双缓冲占用；
 * - 完成 100：仅 force_complete / job_completed。
 * 百分比单调不回退；暂停 / 换层等待时冻结百分比。
 */
void GlavoService::send_print_progress(bool force_complete)
{
    auto& link = McuLink::instance();
    const auto job = PrintJobState::instance().snapshot();

    parser::IParserWorker* parser = active_parser();
    const bool parse_done = parser &&
                            parser->is_file_read_exhausted() &&
                            !parser->is_parsing();
    last_parse_done_ = parse_done;

    int denom = 0;
    if(!force_complete && !job.job_completed) {
        denom = progress_denominator(job.sent_packets);
        if(denom <= 0) {
            return;
        }
    }

    int pct = 0;
    if(force_complete || job.job_completed) {
        pct = 100;
    } else {
        const bool paused = link.is_user_dispatch_paused();
        const bool waiting_layer = link.is_waiting_layer();
        const bool draining = link.is_natural_complete_pending();

        if((paused || waiting_layer) && last_print_pct_ >= 0) {
            /* 暂停/换层：冻结显示，仍可因 rate 变化带环境字段推送 */
            pct = last_print_pct_;
            if(pct > 99) {
                pct = 99;
            }
        } else if(draining) {
            pct = map_drain_progress_95_to_99(job.cmd_cache_rate,
                                              job.data_cache_rate,
                                              link.mcu_rates_have_been_nonempty());
            if(last_print_pct_ >= 0 && pct < last_print_pct_) {
                pct = last_print_pct_;
            }
            if(pct > 99) {
                pct = 99;
            }
        } else {
            int packet_pct_95 = map_send_progress_to_95(job.sent_packets, denom);
            if(!parse_done && packet_pct_95 > 95) {
                packet_pct_95 = 95;
            }
            const uint32_t done_layers =
                PrintVideoNotify::instance().current_layer_index();
            pct = fuse_send_and_layer_progress_95(packet_pct_95,
                                                  done_layers,
                                                  print_total_layers_);
            if(last_print_pct_ >= 0 && pct < last_print_pct_) {
                pct = last_print_pct_;
            }
            if(pct > 95) {
                pct = 95;
            }
        }
    }

    if(!force_complete && !job.job_completed) {
        const bool rates_changed =
            job.cmd_cache_rate != last_sent_cmd_rate_ ||
            job.data_cache_rate != last_sent_data_rate_;
        const bool drained =
            job.cmd_cache_rate == 0 && job.data_cache_rate == 0;
        if(pct >= 95 && rates_changed && !drained) {
            const auto now = std::chrono::steady_clock::now();
            if(last_drain_progress_at_.time_since_epoch().count() != 0 &&
               now - last_drain_progress_at_ <
                   std::chrono::milliseconds(kDrainProgressMinMs)) {
                return;
            }
            last_drain_progress_at_ = now;
        }
        /* 百分比未前进时不推送，除非缓存 rate 变化需要带环境字段刷新 */
        if(last_print_pct_ >= 0 && pct <= last_print_pct_ && !rates_changed) {
            return;
        }
    } else if(last_print_pct_ >= 100) {
        return;
    }

    last_print_pct_       = pct;
    last_sent_cmd_rate_   = job.cmd_cache_rate;
    last_sent_data_rate_  = job.data_cache_rate;

    const auto env = link.env_snapshot();

    middleware::PrintProgressPayload p{};
    p.progress_percent  = pct;
    p.remaining_minutes = 0;
    p.pm25_indoor       = env.valid ? env.pm25_indoor : 0.f;
    p.pm25_cabin        = env.valid ? env.pm25_cabin : 0.f;
    p.temperature_c     = env.valid ? env.temperature_c : 0.f;
    p.oxygen_percent    = env.valid ? env.oxygen_percent : 0.f;
    p.cmd_cache_rate    = job.cmd_cache_rate;
    p.data_cache_rate   = job.data_cache_rate;

    middleware::Message ntf(middleware::MsgType::APP_Print_Progress_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_progress publish failed pct=%d", pct);
    }
}

/** @brief 向 UI 推送 APP_Print_Terminate_Rsp */
void GlavoService::publish_terminate_done()
{
    middleware::Message ntf(middleware::MsgType::APP_Print_Terminate_Rsp,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    if (!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "terminate_done publish_async failed");
    }
}

/** @brief 向 UI 回复 APP_Print_Pause_Rsp（paused=1 暂停 / 0 恢复） */
void GlavoService::publish_print_pause_done(bool paused)
{
    middleware::PrintPauseStatePayload p{};
    p.paused = paused ? 1 : 0;

    middleware::Message ntf(middleware::MsgType::APP_Print_Pause_Rsp,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_pause_done publish failed paused=%d",
                              paused ? 1 : 0);
    }
    publish_print_h264_record_pause(paused);
}

/** @brief 通知 Media 暂停/恢复打印 H264 连续录像（与 UI 暂停态同步） */
void GlavoService::publish_print_h264_record_pause(bool paused)
{
    middleware::PrintPauseStatePayload p{};
    p.paused = paused ? 1 : 0;
    middleware::Message ntf(
        middleware::MsgType::APP_Print_H264_Record_Pause_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::APP_MEDIA);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_h264_pause publish failed paused=%d",
                              paused ? 1 : 0);
    }
}

/** @brief 向 UI 推送缺粉告警（通常已先自动 pause） */
void GlavoService::publish_print_powder_low_ntf()
{
    middleware::Message ntf(middleware::MsgType::APP_Print_Powder_Low_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "powder_low_ntf publish failed");
    }
}

/** @brief 向 MQTT 推送 GLAVO_Print_State_Ntf（开始打印/打印结束状态边沿） */
void GlavoService::publish_print_state_ntf(uint32_t mode)
{
    middleware::PrintStateNtfPayload p{};
    p.mode = mode;
    p.reserved = 0;
    p.since_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    middleware::Message ntf(middleware::MsgType::GLAVO_Print_State_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::APP_MQTT);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_state_ntf publish failed mode=%u", mode);
    }
}

void GlavoService::publish_device_event(middleware::DeviceEventCode code,
                                        middleware::DeviceEventSeverity severity,
                                        bool active)
{
    middleware::DeviceEventPayload p{};
    p.code = static_cast<uint16_t>(code);
    p.severity = static_cast<uint8_t>(severity);
    p.active = active ? 1u : 0u;
    p.event_ts_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    middleware::Message ntf(middleware::MsgType::GLAVO_Device_Event_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "device_event publish failed code=%u active=%u",
            static_cast<unsigned>(p.code),
            static_cast<unsigned>(p.active));
    }
}

bool GlavoService::request_first_layer_visual_check(uint16_t layer_height_um)
{
    if(!first_layer_visual_check_on_) {
        return true;
    }
    if(!print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "first layer visual check skipped: print already stopped");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(first_layer_visual_mtx_);
        first_layer_visual_waiting_ = true;
        first_layer_visual_result_ = 0;
    }

    middleware::VisualFirstLayerCheckReqPayload req{};
    req.layer_height_um = layer_height_um;
    middleware::Message msg(
        middleware::MsgType::GLAVO_Visual_FirstLayer_Check_Req,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::APP_VISUAL);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    if(!publish_async(msg)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "first layer visual check req publish failed");
        std::lock_guard<std::mutex> lock(first_layer_visual_mtx_);
        first_layer_visual_waiting_ = false;
        return false;
    }

    UPC_LOG_GLAVO_SERVICE(Info, "first layer visual check requested layer=%u",
                          static_cast<unsigned>(layer_height_um));

    std::unique_lock<std::mutex> lk(first_layer_visual_mtx_);
    if(!first_layer_visual_cv_.wait_for(
           lk, kFirstLayerVisualReplyTimeout,
           [this]() { return !first_layer_visual_waiting_; })) {
        first_layer_visual_waiting_ = false;
        first_layer_visual_result_ = 0;
        UPC_LOG_GLAVO_SERVICE(Warn,
            "first layer visual check timeout_ms=%lld result=0",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kFirstLayerVisualReplyTimeout).count()));
    }
    const bool pass = (first_layer_visual_result_ != 0);
    UPC_LOG_GLAVO_SERVICE(Info, "first layer visual check result=%u",
                          static_cast<unsigned>(first_layer_visual_result_));
    if(pass) {
        publish_device_event(middleware::DeviceEventCode::FirstLayerVisualPassed,
            middleware::DeviceEventSeverity::Info, true);
    } else {
        publish_device_event(middleware::DeviceEventCode::FirstLayerVisualRecoat,
            middleware::DeviceEventSeverity::Error, true);
    }
    return pass;
}

void GlavoService::handle_visual_first_layer_reference_ready_rsp(
    const middleware::Message& msg)
{
    middleware::VisualFirstLayerReferenceReadyPayload rsp{};
    if(!read_inline_pod(msg.payload, rsp)) {
        if(first_layer_reference_waiting_) {
            publish_device_event(
                middleware::DeviceEventCode::FirstLayerReferenceRetrying,
                middleware::DeviceEventSeverity::Error, true);
            UPC_LOG_GLAVO_SERVICE(Warn,
                "invalid bare reference response; keep waiting for retry");
        }
        return;
    }
    if(!first_layer_reference_waiting_) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "bare reference response without pending print result=%u frame=%u",
            static_cast<unsigned>(rsp.result),
            static_cast<unsigned>(rsp.frame_seq));
        return;
    }
    if(rsp.result == 0) {
        publish_device_event(
            middleware::DeviceEventCode::FirstLayerReferenceRetrying,
            middleware::DeviceEventSeverity::Error, true);
        UPC_LOG_GLAVO_SERVICE(Warn,
            "same-session bare reference capture failed; keep waiting for retry");
        return;
    }
    first_layer_reference_waiting_ = false;
    publish_device_event(
        middleware::DeviceEventCode::FirstLayerReferenceReady,
        middleware::DeviceEventSeverity::Info, true);
    UPC_LOG_GLAVO_SERVICE(Info,
        "same-session bare reference ready frame=%u; allowing print motion",
        static_cast<unsigned>(rsp.frame_seq));
    begin_print_motion_after_reference_ready();
}

void GlavoService::handle_visual_first_layer_check_rsp(const middleware::Message& msg)
{
    middleware::VisualFirstLayerCheckRspPayload rsp{};
    if(!read_inline_pod(msg.payload, rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "first layer visual check rsp invalid payload");
        std::lock_guard<std::mutex> lock(first_layer_visual_mtx_);
        if(first_layer_visual_waiting_) {
            first_layer_visual_result_ = 0;
            first_layer_visual_waiting_ = false;
            first_layer_visual_cv_.notify_all();
        }
        return;
    }

    std::lock_guard<std::mutex> lock(first_layer_visual_mtx_);
    if(!first_layer_visual_waiting_) {
        UPC_LOG_GLAVO_SERVICE(Warn, "first layer visual check rsp without pending request");
        return;
    }
    first_layer_visual_result_ = rsp.result ? 1 : 0;
    first_layer_visual_waiting_ = false;
    first_layer_visual_cv_.notify_all();
    if(first_layer_visual_result_ != 0) {
        publish_first_layer_visual_session(false, "first_layer_passed");
    }
}

/**
 * @brief McuLink 作业事件（已在 worker 线程）：完成/终止/错误/缺粉
 *
 * PrintCompleted → 录像 finish、进度 100%、Terminate_Rsp（UI 用其退出打印态）、清会话。
 * TerminateDone / PrintError：
 *   - 若 deferred_print_start_：立刻 launch 新作业（不再发 abort/清 prepare）
 *   - 否则：录像 abort、Terminate_Rsp、清会话。
 * PowderLow：自动暂停 TCP 下发 + 通知 UI（除非用户已 ignore）。
 */
void GlavoService::on_mcu_link_event(McuLinkEvent ev)
{
    switch(ev) {
        case McuLinkEvent::PrintCompleted:
            UPC_LOG_GLAVO_SERVICE(Info, "tcp_job_event: PrintCompleted");
            deferred_print_start_ = false;
            PrintVideoNotify::instance().on_print_finish();
            send_print_progress(true);
            (void)apply_red_laser_mode(true, false, "print_completed");
            publish_terminate_done();
            print_running_.store(false);
            publish_first_layer_visual_session(false, "print_completed");
            ignore_powder_low_for_current_print_ = false;
            clear_prepare_session();
            publish_print_state_ntf(static_cast<uint32_t>(middleware::UserHomeStatus::Idle));
            break;
        case McuLinkEvent::TerminateDone:
        case McuLinkEvent::PrintError:
            if(ev == McuLinkEvent::TerminateDone) {
                UPC_LOG_GLAVO_SERVICE(Info, "tcp_job_event: TerminateDone");
            } else {
                UPC_LOG_GLAVO_SERVICE(Warn, "tcp_job_event: PrintError");
            }
            if(deferred_print_start_) {
                deferred_print_start_ = false;
                if(prepare_ready_ && !prepared_local_path_.empty()) {
                    UPC_LOG_GLAVO_SERVICE(Info,
                        "print_start resume after terminate path=%s",
                        prepared_local_path_.c_str());
                    launch_print_from_prepared(prepared_local_path_);
                    break;
                }
            }
            PrintVideoNotify::instance().on_print_abort();
            print_running_.store(false);
            (void)apply_red_laser_mode(true, false,
                ev == McuLinkEvent::TerminateDone ? "terminate_done" : "print_error");
            publish_first_layer_visual_session(false,
                ev == McuLinkEvent::TerminateDone ? "terminate_done" : "print_error");
            ignore_powder_low_for_current_print_ = false;
            publish_terminate_done();
            clear_prepare_session();
            publish_print_state_ntf(static_cast<uint32_t>(middleware::UserHomeStatus::Idle));
            break;
        case McuLinkEvent::PowderLow: {
            if(!print_running_.load()) {
                break;
            }
            if(ignore_powder_low_for_current_print_) {
                UPC_LOG_GLAVO_SERVICE(Warn, "powder low ignored for current print");
                break;
            }
            McuLink& link = McuLink::instance();
            if(!link.is_user_dispatch_paused()) {
                link.set_user_dispatch_paused(true);
                publish_print_pause_done(true);
                UPC_LOG_GLAVO_SERVICE(Warn, "auto pause due to powder low");
            }
            publish_print_powder_low_ntf();
            break;
        }
    }
}

/* ============================================================================
 * UI 设备状态与控制（耗材 / 环境 / 门 / 净化 / 清粉 / 转运）
 *
 * 门控：cal.csv 可开「真实传感器」模式；关闭时走占位立即成功（便于无 MCU 联调）。
 * 开门前清粉（UI_Clean_Start）：独立 clean_worker_，避免堵死 worker 数秒～数分钟。
 * 清粉测试（Clean_Test）：串口指令 + 同步通知 Media 延时录像；完成由 MCU 回调回灌。
 * ============================================================================ */

/** @brief 回复耗材余量（当前为占位数据） */
void GlavoService::handle_supply_status_req(const middleware::Message& msg)
{
    middleware::SupplyStatusPayload p{};
    p.material_pct = 48.0f;
    p.filter_pct   = 48.0f;
    p.material_ok  = 1;
    p.filter_ok    = 1;

    middleware::Message rsp(middleware::MsgType::APP_Supply_Status_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "supply_status_rsp publish_async failed");
    }
}

/** @brief 回复舱内 PM2.5（来自 McuLink 环境快照） */
void GlavoService::handle_pm25_request(const middleware::Message& msg)
{
    /* 阈值判定归后端：舱内 PM2.5 ≥ 阈值 判不合格（is_high=1）→ 触发开门前清粉。
     * 阈值默认 50，可由 cal.csv Config 段 PM2.5Clean 覆盖。 */
    const auto env = McuLink::instance().env_snapshot();
    middleware::Pm25ResultPayload p{};
    p.is_high   = (env.valid && env.pm25_cabin >= pm25_clean_threshold_) ? 1 : 0;
    p.pm25_cabin = env.valid ? env.pm25_cabin : 0.f;

    middleware::Message rsp(middleware::MsgType::APP_PM25_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "pm25_rsp publish_async failed");
    }
}

/** @brief 发送门控操作结果 Rsp */
void GlavoService::send_door_rsp(const middleware::Message& msg,
                                 middleware::MsgType rsp_type,
                                 bool ok)
{
    middleware::DoorResultPayload p{};
    p.ok = ok ? 1 : 0;

    middleware::Message rsp(rsp_type,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "door_rsp publish_async failed type=%u",
                              static_cast<unsigned>(rsp_type));
    }
}

/** @brief 处理 UI_CLOSE_DOOR_Req */
void GlavoService::handle_close_door_request(const middleware::Message& msg)
{
    if(door_real_close_check_enabled_) {
        const auto env = McuLink::instance().env_snapshot();
        const bool closed = env.valid && env.door_state_valid && !env.outer_door_open;
        send_door_rsp(msg, middleware::MsgType::APP_CLOSE_DOOR_Rsp, closed);
        UPC_LOG_GLAVO_SERVICE(Info,
            "close_door_check real mode: valid=%d door_valid=%d door_open=%d -> closed=%d",
            env.valid ? 1 : 0, env.door_state_valid ? 1 : 0,
            env.outer_door_open ? 1 : 0, closed ? 1 : 0);
        return;
    }
    /* 兼容旧逻辑：占位立即确认已关 */
    send_door_rsp(msg, middleware::MsgType::APP_CLOSE_DOOR_Rsp, true);
}

/**
 * @brief 处理 UI_OpenDoor_Req
 *
 * 真实模式会在 **worker 线程内** sleep 轮询门传感器（最长 GLAVO_DOOR_OPEN_TIMEOUT_MS），
 * 期间其它 UI 请求只入队不处理——属有意简化，超时宜保持较短。
 */
void GlavoService::handle_open_door_request(const middleware::Message& msg)
{
    if(door_real_open_enabled_) {
        /* 按传感器协议判定开门成功：
         * 先发送一次开门指令，再在超时窗内轮询 0x9B value[10] bit7（outer_door_open）。 */
        const bool sent = McuLink::instance().request_open_door_once();
        bool opened = false;
        bool valid_seen = false;
        if(sent) {
            const uint32_t wait_ms = GLAVO_DOOR_OPEN_TIMEOUT_MS > 0
                                     ? GLAVO_DOOR_OPEN_TIMEOUT_MS : 1u;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(wait_ms);
            do {
                const auto env = McuLink::instance().env_snapshot();
                if(env.valid && env.door_state_valid) {
                    valid_seen = true;
                    if(env.outer_door_open) {
                        opened = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } while(std::chrono::steady_clock::now() < deadline);
        }
        send_door_rsp(msg, middleware::MsgType::APP_OpenDoor_Rsp, sent && opened);
        if(sent && opened) {
            McuLink::instance().set_print_task_flow_active(false);
        }
        UPC_LOG_GLAVO_SERVICE(Info,
            "open_door real mode(sensor): sent=%d opened=%d valid_seen=%d timeout_ms=%u",
            sent ? 1 : 0, opened ? 1 : 0, valid_seen ? 1 : 0,
            static_cast<unsigned>(GLAVO_DOOR_OPEN_TIMEOUT_MS));
        return;
    }
    /* 兼容旧逻辑：占位立即确认已开 */
    send_door_rsp(msg, middleware::MsgType::APP_OpenDoor_Rsp, true);
    McuLink::instance().set_print_task_flow_active(false);
}

/** @brief 处理 UI_Purify_Req（尚未实现业务逻辑） */
void GlavoService::handle_purify_request(const middleware::Message& msg)
{
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Debug, "purify_req: not implemented");
}


/**
 * @brief 处理 UI_Clean_Start_Req（开门前清粉）
 *
 * 业务顺序（在 clean_worker_ 中执行，不阻塞 worker 主循环）：
 *   风机占空比 60% → 开风机 → 每 1s 读舱内 PM2.5
 *   → 低于阈值（或用户 Stop）→ 关风机 → APP_Clean_Start_Rsp / Stop_Rsp
 *
 * Stop：只置 cancel 标志；真正 Rsp 仍由 clean_worker_ 退出路径发出。
 */
void GlavoService::handle_clean_start_request(const middleware::Message& msg)
{
    if(clean_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "clean_start_req: already running");
        return;
    }
    if(clean_worker_.joinable()) {
        clean_worker_.join();
    }

    constexpr uint8_t kCleanFanDutyPercent = 60u;
    constexpr auto kCleanPollInterval = std::chrono::seconds(1);
    const uint32_t reply_target = static_cast<uint32_t>(msg.sender_id);
    clean_cancel_requested_.store(false);
    clean_stop_rsp_pending_.store(false);
    clean_stop_rsp_target_.store(0);
    clean_running_.store(true);

    clean_worker_ = std::thread([this, reply_target, kCleanFanDutyPercent, kCleanPollInterval]() {
        bool cancelled = false;
        uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
        int n = serial_debug_build_fan_duty(kCleanFanDutyPercent, buf, sizeof(buf));
        if(n <= 0) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "clean_start_req: build fan duty failed duty=%u",
                static_cast<unsigned>(kCleanFanDutyPercent));
        } else {
            debug_tx(buf, n);
        }

        n = serial_debug_build_fan_switch(true, buf, sizeof(buf));
        if(n <= 0) {
            UPC_LOG_GLAVO_SERVICE(Warn, "clean_start_req: build fan on failed");
        } else {
            debug_tx(buf, n);
        }

        UPC_LOG_GLAVO_SERVICE(Info,
            "clean_start_req: fan on, waiting pm25 < %.1f",
            static_cast<double>(pm25_clean_threshold_));
        for(;;) {
            if(clean_cancel_requested_.load()) {
                cancelled = true;
                UPC_LOG_GLAVO_SERVICE(Info, "clean_start_req: cancelled by stop request");
                break;
            }
            const auto env = McuLink::instance().env_snapshot();
            if(env.valid && env.pm25_cabin < pm25_clean_threshold_) {
                UPC_LOG_GLAVO_SERVICE(Info,
                    "clean_start_req: pm25 reached %.1f < %.1f",
                    static_cast<double>(env.pm25_cabin),
                    static_cast<double>(pm25_clean_threshold_));
                break;
            }
            std::this_thread::sleep_for(kCleanPollInterval);
        }

        n = serial_debug_build_fan_switch(false, buf, sizeof(buf));
        if(n <= 0) {
            UPC_LOG_GLAVO_SERVICE(Warn, "clean_start_req: build fan off failed");
        } else {
            debug_tx(buf, n);
        }

        if(cancelled) {
            const uint32_t stop_target = clean_stop_rsp_target_.load();
            if(clean_stop_rsp_pending_.load() && stop_target != 0) {
                publish_clean_rsp(middleware::MsgType::APP_Clean_Stop_Rsp, stop_target);
            }
        } else {
            publish_clean_rsp(middleware::MsgType::APP_Clean_Start_Rsp, reply_target);
        }

        clean_running_.store(false);
        clean_stop_rsp_pending_.store(false);
        clean_stop_rsp_target_.store(0);
        clean_cancel_requested_.store(false);
    });
}

/** @brief 取消进行中的开门前清粉；未在跑则立即回 Stop_Rsp */
void GlavoService::handle_clean_stop_request(const middleware::Message& msg)
{
    if(!clean_running_.load()) {
        publish_clean_rsp(middleware::MsgType::APP_Clean_Stop_Rsp,
                          static_cast<uint32_t>(msg.sender_id));
        return;
    }
    clean_stop_rsp_pending_.store(true);
    clean_stop_rsp_target_.store(static_cast<uint32_t>(msg.sender_id));
    clean_cancel_requested_.store(true);
    UPC_LOG_GLAVO_SERVICE(Info, "clean_stop_req: requested");
}

void GlavoService::publish_clean_test_start_rsp(uint32_t target_id, bool accepted)
{
    middleware::CleanTestStartRspPayload rsp{};
    rsp.accepted = accepted ? 1 : 0;
    middleware::Message out(middleware::MsgType::APP_Clean_Test_Start_Rsp,
        middleware::ModuleId::APP_GLAVO,
        static_cast<middleware::ModuleId>(target_id));
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    (void)publish_async(out);
}

void GlavoService::publish_clean_test_done(uint8_t result_code, bool raw_retained,
                                           bool has_mp4,
                                           const std::string& mp4_rel_path)
{
    middleware::CleanTestDonePayload ntf{};
    ntf.result_code = result_code;
    ntf.raw_retained = raw_retained ? 1 : 0;
    ntf.has_mp4 = has_mp4 ? 1 : 0;
    if(has_mp4) {
        std::snprintf(ntf.mp4_rel_path, sizeof(ntf.mp4_rel_path), "%s",
                      mp4_rel_path.c_str());
    }
    middleware::Message out(middleware::MsgType::APP_Clean_Test_Result_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    (void)publish_async(out);
}

void GlavoService::publish_clean_test_close_rsp(uint32_t target_id)
{
    middleware::Message rsp(middleware::MsgType::APP_Clean_Test_Close_Rsp,
        middleware::ModuleId::APP_GLAVO,
        static_cast<middleware::ModuleId>(target_id));
    (void)publish_async(rsp);
}

void GlavoService::notify_clean_video_start(const middleware::CleanTestStartReqPayload& req)
{
    middleware::CleanVideoSessionStartPayload payload{};
    std::snprintf(payload.session_id, sizeof(payload.session_id), "%s",
                  clean_test_session_id_);
    std::snprintf(payload.job_name, sizeof(payload.job_name), "%s",
                  clean_test_job_name_);
    payload.width = req.width;
    payload.height = req.height;
    /* 优先 UI 请求；否则用 cal.csv TIMELAPSE_VIDEO_FPS；0 则 Media 默认 */
    payload.fps = req.fps > 0 ? req.fps : timelapse_video_fps_;

    middleware::Message msg(middleware::MsgType::APP_Clean_Video_Session_Start_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::APP_MEDIA);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    (void)publish_async(msg);
}

void GlavoService::notify_clean_video_stop(uint8_t result_code, bool keep_raw)
{
    middleware::CleanVideoSessionStopPayload payload{};
    std::snprintf(payload.session_id, sizeof(payload.session_id), "%s",
                  clean_test_session_id_);
    payload.result_code = result_code;
    payload.keep_raw = keep_raw ? 1 : 0;

    middleware::Message msg(middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::APP_MEDIA);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    (void)publish_async(msg);
}

/**
 * @brief MCU 清粉测试完成（worker 内）：停 Media 录像会话，必要时由 Glavo 直接发 Done
 *
 * 已开录像时：Done 改由 Media 合成后通知 UI；此处只发 Session_Stop。
 */
void GlavoService::on_clean_test_done(uint8_t result_code)
{
    if(!clean_test_active_) {
        return;
    }
    const bool video_started = clean_video_session_started_;
    if(clean_video_session_started_) {
        /* Media 合成完成后发 APP_Clean_Test_Result_Ntf；此处只停录 */
        notify_clean_video_stop(result_code, false);
        clean_video_session_started_ = false;
    }
    clean_test_active_ = false;

    if(!video_started) {
        /* 未开录像时仍由 Glavo 直接回 Done */
        publish_clean_test_done(result_code, false, false, std::string{});
    }
}

/**
 * @brief 启动清粉测试：串口下发 + 通知 Media 开始延时抽帧
 *
 * clean_video_record_on_ 仅供调试页状态回读，不再门控是否录像。
 */
void GlavoService::handle_clean_test_start_request(const middleware::Message& msg)
{
    middleware::CleanTestStartReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        publish_clean_test_start_rsp(static_cast<uint32_t>(msg.sender_id), false);
        return;
    }
    if(clean_test_active_) {
        publish_clean_test_start_rsp(static_cast<uint32_t>(msg.sender_id), false);
        return;
    }

    const std::string job_name = sanitize_job_name(req.job_name);
    std::snprintf(clean_test_job_name_, sizeof(clean_test_job_name_), "%s",
                  job_name.c_str());
    make_clean_test_session_id(clean_test_session_id_, sizeof(clean_test_session_id_));

    uint8_t frame[SERIAL_DEBUG_MAX_FRAME];
    const int n = serial_debug_build_clean_test_start(frame, sizeof(frame));
    if(n <= 0) {
        publish_clean_test_start_rsp(static_cast<uint32_t>(msg.sender_id), false);
        return;
    }

    clean_test_active_ = true;
    debug_tx(frame, n);
    /* Purify 路径始终录像；调试开关仅保留状态回读，不再门控 */
    notify_clean_video_start(req);
    clean_video_session_started_ = true;
    publish_clean_test_start_rsp(static_cast<uint32_t>(msg.sender_id), true);
}

void GlavoService::handle_clean_test_close_request(const middleware::Message& msg)
{
    uint8_t frame[SERIAL_DEBUG_MAX_FRAME];
    const int n = serial_debug_build_clean_test_close(frame, sizeof(frame));
    if(n > 0) {
        debug_tx(frame, n);
    }

    if(clean_test_active_) {
        if(clean_video_session_started_) {
            notify_clean_video_stop(0xfe, true);
            clean_video_session_started_ = false;
        }
        clean_test_active_ = false;
    }
    publish_clean_test_close_rsp(static_cast<uint32_t>(msg.sender_id));
}

/** @brief 处理 UI_Transport_Req：经 McuLink 发一次转运指令，结果异步经 on_transport_result 回 UI */
void GlavoService::handle_transport_request(const middleware::Message& /*msg*/)
{
    if(!McuLink::instance().request_transport_once()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "transport request send failed");
    }
}

void GlavoService::publish_transport_result(uint8_t result)
{
    middleware::TransportResultPayload ntf{};
    ntf.result = result;
    middleware::Message out(middleware::MsgType::APP_Transport_Result_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    (void)publish_async(out);
}

void GlavoService::on_transport_result(uint8_t result)
{
    UPC_LOG_GLAVO_SERVICE(Info, "transport_result ntf result=%u",
                          static_cast<unsigned>(result));
    publish_transport_result(result);
}

/* ============================================================================
 * MCU OTA
 *
 * 两路互斥：TCP 振镜 OTA（GALVO_OTA_IMAGE_DIR）与串口 MCU OTA（MCU_OTA_IMAGE_DIR）。
 * 状态进度由 McuLink 回调直接 publish_*_ota_status → UI，不经 worker 队列。
 * ============================================================================ */

/** @brief 处理 UI_Mcu_Ota_Start_Req，启动 McuLink OTA 会话 */
void GlavoService::handle_mcu_ota_start(const middleware::Message& msg)
{
    (void)msg;
    {
        std::lock_guard<std::mutex> lock(mcu_ota_status_pub_mtx_);
        mcu_ota_status_pub_has_last_ = false;
    }
    if(McuLink::instance().ota_active()) {
        UPC_LOG_MCU_OTA(Warn, "mcu_ota_start: already active");
        middleware::McuOtaStatusPayload st{};
        st.state = middleware::McuOtaState::Failed;
        st.progress = 0;
        publish_mcu_ota_status(st);
        return;
    }
    if(McuLink::instance().serial_ota_active()) {
        UPC_LOG_MCU_OTA(Warn, "mcu_ota_start: serial ota active");
        middleware::McuOtaStatusPayload st{};
        st.state = middleware::McuOtaState::Failed;
        st.progress = 0;
        publish_mcu_ota_status(st);
        return;
    }
    if(!McuLink::instance().start_mcu_ota(GALVO_OTA_IMAGE_DIR)) {
        middleware::McuOtaStatusPayload st{};
        st.state    = middleware::McuOtaState::Failed;
        st.progress = 0;
        publish_mcu_ota_status(st);
    }
}

/** @brief 处理 UI_Mcu_Ota_Cancel_Ntf，取消进行中的 OTA */
void GlavoService::handle_mcu_ota_cancel(const middleware::Message& msg)
{
    (void)msg;
    McuLink::instance().cancel_mcu_ota();
}

/** @brief 向 UI 推送 APP_Mcu_Ota_Status_Ntf */
void GlavoService::publish_mcu_ota_status(
    const middleware::McuOtaStatusPayload& st)
{
    const auto state = static_cast<middleware::McuOtaState>(st.state);
    const bool terminal =
        (state == middleware::McuOtaState::Success) ||
        (state == middleware::McuOtaState::Failed) ||
        (state == middleware::McuOtaState::Cancelled);
    {
        std::lock_guard<std::mutex> lock(mcu_ota_status_pub_mtx_);
        if(mcu_ota_status_pub_has_last_) {
            const auto& last = mcu_ota_status_pub_last_;
            if(last.state == st.state && last.progress == st.progress) {
                return;
            }
            if(!terminal && last.state == st.state) {
                const int delta = static_cast<int>(st.progress) - static_cast<int>(last.progress);
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - mcu_ota_status_pub_last_ts_)
                        .count();
                // 非终态场景下做降频：进度变化很小且时间很短时，跳过本次推送。
                if(delta >= 0 && delta < 6 && elapsed_ms < 80) {
                    return;
                }
            }
        }
        mcu_ota_status_pub_last_ = st;
        mcu_ota_status_pub_last_ts_ = std::chrono::steady_clock::now();
        mcu_ota_status_pub_has_last_ = true;
    }

    middleware::Message out_ui(middleware::MsgType::APP_Mcu_Ota_Status_Ntf,
                               middleware::ModuleId::APP_GLAVO,
                               middleware::ModuleId::UI);
    out_ui.priority = terminal ? middleware::MsgPriority::Realtime
                               : middleware::MsgPriority::High;
    out_ui.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&st), sizeof(st));
    (void)publish_async(out_ui);

    middleware::Message out_mqtt(middleware::MsgType::APP_Mcu_Ota_Status_Ntf,
                                 middleware::ModuleId::APP_GLAVO,
                                 middleware::ModuleId::APP_MQTT);
    out_mqtt.priority = out_ui.priority;
    out_mqtt.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&st), sizeof(st));
    (void)publish_async(out_mqtt);
}

/** @brief 处理 UI_Serial_Mcu_Ota_Start_Req，启动 McuLink 串口 OTA 会话 */
void GlavoService::handle_serial_mcu_ota_start(const middleware::Message& msg)
{
    (void)msg;
    if(McuLink::instance().serial_ota_active()) {
        UPC_LOG_MCU_OTA(Warn, "serial_mcu_ota_start: already active");
        return;
    }
    if(!McuLink::instance().start_serial_mcu_ota(MCU_OTA_IMAGE_DIR)) {
        middleware::McuOtaStatusPayload st{};
        st.state = middleware::McuOtaState::Failed;
        st.progress = 0;
        publish_serial_mcu_ota_status(st);
    }
}

/** @brief 处理 UI_Serial_Mcu_Ota_Cancel_Ntf，取消 MCU 串口 OTA */
void GlavoService::handle_serial_mcu_ota_cancel(const middleware::Message& msg)
{
    (void)msg;
    McuLink::instance().cancel_serial_mcu_ota();
}

/** @brief 向 UI 推送 APP_Serial_Mcu_Ota_Status_Ntf */
void GlavoService::publish_serial_mcu_ota_status(
    const middleware::McuOtaStatusPayload& st)
{
    middleware::Message out(middleware::MsgType::APP_Serial_Mcu_Ota_Status_Ntf,
                            middleware::ModuleId::APP_GLAVO,
                            middleware::ModuleId::UI);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&st), sizeof(st));
    (void)publish_async(out);
}

/* ============================================================================
 * 网络 / WiFi / USB 热插拔
 *
 * UI_Wifi_*：worker 内只把命令丢给 WifiManager（其后台线程跑 wpa_ctrl）。
 * 状态/列表/连接结果回调在 Wifi 线程触发 → 此处 publish_* 回 UI（异步）。
 * USB：独立线程轮询挂载点，变化发 GLAVO_USB_Hotplug_Ntf。
 * ============================================================================ */

/** @brief 开关 WiFi（enable=0/1） */
 void GlavoService::handle_wifi_enable(const middleware::Message& msg)
 {
     middleware::WifiEnableReqPayload p{};
     if(!read_inline_pod(msg.payload, p)) {
         return;
     }
     WifiManager::instance().set_enabled(p.enable != 0);
 }
 
 void GlavoService::handle_wifi_scan(const middleware::Message& /*msg*/)
 {
     WifiManager::instance().scan();
 }
 
 void GlavoService::handle_wifi_connect(const middleware::Message& msg)
 {
     middleware::WifiConnectReqPayload p{};
     if(!read_inline_pod(msg.payload, p)) {
         return;
     }
     p.ssid[sizeof(p.ssid) - 1]         = '\0';
     p.password[sizeof(p.password) - 1] = '\0';
     WifiManager::instance().connect(p.ssid, p.password);
 }
 
 void GlavoService::handle_wifi_forget(const middleware::Message& msg)
 {
     middleware::WifiForgetReqPayload p{};
     if(!read_inline_pod(msg.payload, p)) {
         return;
     }
     p.ssid[sizeof(p.ssid) - 1] = '\0';
     WifiManager::instance().forget(p.ssid);
 }
 
 void GlavoService::handle_wifi_disconnect(const middleware::Message& msg)
 {
     middleware::WifiForgetReqPayload p{};   /* 复用 ssid 载荷 */
     if(!read_inline_pod(msg.payload, p)) {
         return;
     }
     p.ssid[sizeof(p.ssid) - 1] = '\0';
     WifiManager::instance().disconnect(p.ssid);
 }
 
 void GlavoService::handle_wifi_info(const middleware::Message& /*msg*/)
 {
     WifiManager::instance().request_info();
 }
 
 void GlavoService::handle_wifi_saved(const middleware::Message& /*msg*/)
 {
     WifiManager::instance().request_saved();
 }
 
 /** @brief 向 UI 推送 GLAVO_Wifi_State_Ntf（首页图标 + 设置详情共用）。 */
 void GlavoService::publish_wifi_state(const WifiSnapshot& snap)
 {
     middleware::WifiStatePayload p{};
     p.enabled     = snap.enabled ? 1 : 0;
     p.connected   = snap.connected ? 1 : 0;
     p.signal_bars = snap.signal_bars;
     std::snprintf(p.ssid, sizeof(p.ssid), "%s", snap.ssid.c_str());
     std::snprintf(p.ip_addr, sizeof(p.ip_addr), "%s", snap.ip.c_str());
 
     middleware::Message out(middleware::MsgType::GLAVO_Wifi_State_Ntf,
         middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
     out.payload = middleware::MsgPayload::from_bytes(
         reinterpret_cast<const uint8_t*>(&p), sizeof(p));
     (void)publish_async(out);
 }
 
 /** @brief 组 [WifiListHeader][WifiEntryPayload×N] 并经 BufferPool 推送 Scan/Saved 结果。 */
 void GlavoService::publish_wifi_list(WifiListKind kind,
                                      const std::vector<WifiEntry>& entries)
 {
     const middleware::MsgType type = (kind == WifiListKind::Scan)
         ? middleware::MsgType::GLAVO_Wifi_Scan_Rsp
         : middleware::MsgType::GLAVO_Wifi_Saved_Rsp;
 
     const uint32_t count = static_cast<uint32_t>(entries.size());
     const size_t header_size  = sizeof(middleware::WifiListHeader);
     const size_t entries_size = static_cast<size_t>(count) *
                                 sizeof(middleware::WifiEntryPayload);
     const size_t total_size   = header_size + entries_size;
 
     const uint32_t buf_id = middleware::BufferPool::instance().allocate(total_size);
     if(buf_id == middleware::INVALID_BUFFER_ID) {
         UPC_LOG_GLAVO_SERVICE(Warn, "wifi: list buffer alloc failed (count=%u)", count);
         return;
     }
     uint8_t* base = middleware::BufferPool::instance().get_ptr(buf_id);
     if(!base) {
         middleware::BufferPool::instance().release(buf_id);
         return;
     }
 
     auto* hdr = reinterpret_cast<middleware::WifiListHeader*>(base);
     hdr->count = count;
     auto* items = reinterpret_cast<middleware::WifiEntryPayload*>(base + header_size);
     for(uint32_t i = 0; i < count; i++) {
         const auto& e = entries[i];
         std::snprintf(items[i].ssid, sizeof(items[i].ssid), "%s", e.ssid.c_str());
         items[i].signal_bars = e.signal_bars;
         items[i].security    = static_cast<uint32_t>(e.security);
         items[i].saved       = e.saved ? 1 : 0;
         items[i].connected   = e.connected ? 1 : 0;
     }
 
     middleware::Message out(type,
         middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
     out.payload = middleware::MsgPayload::from_buffer(
         buf_id, 0, static_cast<uint32_t>(total_size));
     if(!publish_async(out)) {
         UPC_LOG_GLAVO_SERVICE(Warn, "wifi: list publish_async failed (count=%u)", count);
     }
 }
 
 /** @brief 向 UI 推送 GLAVO_Wifi_Connect_Rsp（连接成功/失败；失败用于弹提示）。 */
 void GlavoService::publish_wifi_connect_result(bool ok, const std::string& ssid)
 {
     middleware::WifiConnectRspPayload p{};
     p.ok = ok ? 1 : 0;
     std::snprintf(p.ssid, sizeof(p.ssid), "%s", ssid.c_str());
 
     middleware::Message out(middleware::MsgType::GLAVO_Wifi_Connect_Rsp,
         middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
     out.payload = middleware::MsgPayload::from_bytes(
         reinterpret_cast<const uint8_t*>(&p), sizeof(p));
     (void)publish_async(out);
 }

void GlavoService::publish_usb_hotplug(bool mounted)
{
    middleware::UsbHotplugPayload p{};
    p.present = mounted ? 1 : 0;
    p.mounted = mounted ? 1 : 0;
    std::snprintf(p.mount_path, sizeof(p.mount_path), "%s", GLAVO_USB_MOUNT_DIR);

    middleware::Message out(middleware::MsgType::GLAVO_USB_Hotplug_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(out);
}

void GlavoService::start_usb_hotplug_monitor()
{
    if(usb_hotplug_running_.exchange(true)) {
        return;
    }
    usb_hotplug_thread_ = std::thread(&GlavoService::usb_hotplug_loop, this);
}

void GlavoService::stop_usb_hotplug_monitor()
{
    if(!usb_hotplug_running_.exchange(false)) {
        return;
    }
    if(usb_hotplug_thread_.joinable()) {
        usb_hotplug_thread_.join();
    }
}

/** @brief 轮询 USB 挂载点；状态变化则 publish_usb_hotplug（本线程只读 mounts，不跑业务） */
void GlavoService::usb_hotplug_loop()
{
    publish_usb_hotplug(last_usb_mounted_);
    while(usb_hotplug_running_.load()) {
        std::this_thread::sleep_for(kUsbHotplugPollInterval);
        const bool mounted = is_mountpoint_mounted(usb_mount_point());
        if(mounted == last_usb_mounted_) {
            continue;
        }
        last_usb_mounted_ = mounted;
        publish_usb_hotplug(mounted);
    }
}

/* ============================================================================
 * 调试页手动控制：UI_Debug_* → serial_debug 组帧 → McuLink 串口下发
 *
 * 模式统一：读 Inline POD → serial_debug_build_* → debug_tx（可靠串口队列）。
 * 另有一组“软件开关”不经串口：Print H264 连续录、清粉录像状态回读、环境模拟；
 * 它们改成员变量 / McuLink / PrintVideoNotify，并 publish 状态给 UI。
 * 振镜（以太网 TCP）、回粉电机后端逻辑预留，故不订阅、不下发。
 * ============================================================================ */

/** @brief 将 serial_debug 帧经 McuLink 可靠串口通道下发 */
void GlavoService::debug_tx(const uint8_t* frame, int n)
{
    if(n > 0) {
        /* 调试命令走带 ACK 的可靠通道（GLAVO_SERIAL_ACK_ENABLE=1 时重传） */
        McuLink::instance().enqueue_serial_tx(frame, static_cast<size_t>(n), true);
    } else {
        UPC_LOG_GLAVO_SERVICE(Warn, "debug serial build failed");
    }
}

/** @brief 轴点动：Hold 为速度模式，Click 为相对增量 */
void GlavoService::handle_debug_axis_jog(const middleware::Message& msg)
{
    middleware::DebugAxisJogPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    const int sign = (p.dir < 0) ? -1 : 1;
    const int16_t mag = debug_round_i16(p.value);
    const int16_t val = static_cast<int16_t>(sign * mag);
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    int n;
    if(p.mode == static_cast<uint8_t>(middleware::DebugJogMode::Hold)) {
        /* 长按：速度模式点动 */
        n = serial_debug_build_axis_jog(p.axis, val, buf, sizeof(buf));
    } else {
        /* 点击：相对增量移动 */
        n = serial_debug_build_axis_inc(p.axis, val, buf, sizeof(buf));
    }
    debug_tx(buf, n);
}

/** @brief 长按点动松开：发送轴 jog stop */
void GlavoService::handle_debug_axis_hold_stop(const middleware::Message& msg)
{
    middleware::DebugAxisPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_axis_jog_stop(p.axis, buf, sizeof(buf)));
}

/** @brief 设置 Z 轴工作零点（仅 Z 轴有效） */
void GlavoService::handle_debug_axis_set_zero(const middleware::Message& msg)
{
    middleware::DebugAxisPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    if(p.axis != static_cast<uint8_t>(middleware::DebugAxis::Z)) {
        return;  /* 仅 Z 轴有工作零点 */
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_z_set_zero(buf, sizeof(buf)));
}

/** @brief 轴回 Home（Z→0x11，S→0x10；D 无 Home） */
void GlavoService::handle_debug_axis_home(const middleware::Message& msg)
{
    middleware::DebugAxisPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_axis_home(p.axis, buf, sizeof(buf)));
}

/** @brief 整机/门/电机/照明等设备开关 */
void GlavoService::handle_debug_device(const middleware::Message& msg)
{
    middleware::DebugDevicePayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    const bool on = (p.on != 0);
    int n = -1;
    switch(static_cast<middleware::DebugDeviceTarget>(p.target)) {
        case middleware::DebugDeviceTarget::Device:
            n = serial_debug_build_machine_wake(on, buf, sizeof(buf));
            break;
        case middleware::DebugDeviceTarget::Door:
            if(!on) {
                return;  /* 关门不下发指令，仅开门触发 */
            }
            n = serial_debug_build_door_open(buf, sizeof(buf));
            break;
        case middleware::DebugDeviceTarget::Motor:
            n = serial_debug_build_motor_enable(on, buf, sizeof(buf));
            break;
        case middleware::DebugDeviceTarget::Light:
            n = serial_debug_build_light(on, buf, sizeof(buf));
            break;
        default:
            return;
    }
    debug_tx(buf, n);
}

/** @brief 粉桶开关 */
void GlavoService::handle_debug_powder_bucket(const middleware::Message& msg)
{
    middleware::DebugBucketPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_powder_bucket(p.bucket_id, p.on != 0,
                                                   buf, sizeof(buf)));
}

/** @brief 推粉器开关 */
void GlavoService::handle_debug_pusher(const middleware::Message& msg)
{
    /* 筛粉电机：action 0=停止 1=筛粉运行 2=反转倒渣 */
    middleware::DebugMotorPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_sieve_motor(p.action, buf, sizeof(buf)));
}

/** @brief 内门：action 0=停止 1=开 2=关（原回粉电机块改为内门） */
void GlavoService::handle_debug_inner_door(const middleware::Message& msg)
{
    middleware::DebugMotorPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_inner_door(p.action, buf, sizeof(buf)));
}

/** @brief 风扇开/关（占空比由 handle_debug_fan_duty 单独下发） */
void GlavoService::handle_debug_fan(const middleware::Message& msg)
{
    middleware::DebugFanPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_fan_switch(p.on != 0, buf, sizeof(buf)));
}

/** @brief 风扇占空比（协议合法范围 0–100） */
void GlavoService::handle_debug_fan_duty(const middleware::Message& msg)
{
    middleware::DebugFanPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    int16_t duty = debug_round_i16(p.duty);
    if(duty < 0)   duty = 0;
    if(duty > 100) duty = 100;
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_fan_duty(static_cast<uint8_t>(duty),
                                              buf, sizeof(buf)));
}

/** @brief 通用开关类调试目标（target + on/off） */
void GlavoService::handle_debug_switch(const middleware::Message& msg)
{
    middleware::DebugSwitchPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_switch(p.target, p.on != 0,
                                            buf, sizeof(buf)));
}

bool GlavoService::apply_red_laser_mode(bool red_on, bool dedup, const char* reason)
{
    if(dedup && red_laser_cmd_cached_valid_ && red_laser_cmd_cached_ == red_on) {
        UPC_LOG_GLAVO_SERVICE(Debug, "red/laser mode dedup ignored red_on=%d reason=%s",
                              red_on ? 1 : 0,
                              reason ? reason : "n/a");
        return true;
    }
    /* 红光与激光使能联动：
     * red_on=1（红光） -> 激光使能关（TLV 0x12）
     * red_on=0（激光） -> 激光使能开（TLV 0x11） */
    {
        uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
        const bool laser_enable = !red_on;
        debug_tx(buf, serial_debug_build_switch(0u, laser_enable, buf, sizeof(buf)));
    }
    const uint8_t red_code = red_on ? 1u : 2u;
    if(!McuLink::instance().send_tcp_emergency_ctrl_ex(0u, 0u, red_code)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "red/laser tcp switch failed red_on=%d reason=%s",
                              red_on ? 1 : 0,
                              reason ? reason : "n/a");
        return false;
    }
    red_laser_cmd_cached_ = red_on;
    red_laser_cmd_cached_valid_ = true;
    UPC_LOG_GLAVO_SERVICE(Info, "red/laser mode switched red_on=%d reason=%s",
                          red_on ? 1 : 0,
                          reason ? reason : "n/a");
    return true;
}

/** @brief 调试红光开关：下发 TCP CMD=0x0A（Stop=0, Pause=0, RedLaser=on/off） */
void GlavoService::handle_debug_galvo_red_light(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    (void)apply_red_laser_mode(p.on != 0, true, "debug_toggle");
}

/** @brief 启动清洁测试流程 */
void GlavoService::handle_debug_clean_test_start(const middleware::Message& msg)
{
    (void)msg;
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_clean_test_start(buf, sizeof(buf)));
}

/** @brief 关闭/中止清洁测试流程 */
void GlavoService::handle_debug_clean_test_close(const middleware::Message& msg)
{
    (void)msg;
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_clean_test_close(buf, sizeof(buf)));
}

/** @brief 下发打印层参数（层厚、平衡系数等） */
void GlavoService::handle_debug_print_param(const middleware::Message& msg)
{
    middleware::DebugPrintParamPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_print_layer_param(p.layer_um, p.balance_x1000,
                                                       buf, sizeof(buf)));
}

/** @brief 设置激光功率百分比 */
void GlavoService::handle_debug_laser_power_set(const middleware::Message& msg)
{
    middleware::DebugLaserPowerPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_laser_power_set(p.percent, buf, sizeof(buf)));
}

/** @brief 激光功率恢复默认 */
void GlavoService::handle_debug_laser_power_reset(const middleware::Message& msg)
{
    (void)msg;
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_laser_power_reset(buf, sizeof(buf)));
}

/** @brief 激光报警复位 */
void GlavoService::handle_debug_laser_alarm_reset(const middleware::Message& msg)
{
    (void)msg;
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_laser_alarm_reset(buf, sizeof(buf)));
}

void GlavoService::publish_debug_print_h264_record_state()
{
    middleware::DebugOnOffPayload p{};
    p.on = print_h264_continuous_on_ ? 1 : 0;
    middleware::Message ntf(
        middleware::MsgType::GLAVO_Debug_Print_H264_Record_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(ntf);
}

void GlavoService::handle_debug_print_h264_record_set(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    print_h264_continuous_on_ = (p.on != 0);
    PrintVideoNotify::instance().set_print_h264_continuous(print_h264_continuous_on_);
    UPC_LOG_GLAVO_SERVICE(Info, "debug print_h264_continuous=%d",
                          print_h264_continuous_on_ ? 1 : 0);
    publish_debug_print_h264_record_state();
}

void GlavoService::handle_debug_print_h264_record_get(const middleware::Message& msg)
{
    (void)msg;
    publish_debug_print_h264_record_state();
}

void GlavoService::publish_debug_clean_record_state()
{
    middleware::DebugOnOffPayload p{};
    p.on = clean_video_record_on_ ? 1 : 0;
    middleware::Message ntf(
        middleware::MsgType::GLAVO_Debug_Clean_Record_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(ntf);
}

void GlavoService::handle_debug_clean_record_set(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    clean_video_record_on_ = (p.on != 0);
    UPC_LOG_GLAVO_SERVICE(Info, "debug clean_video_record=%d",
                          clean_video_record_on_ ? 1 : 0);
    publish_debug_clean_record_state();
}

void GlavoService::handle_debug_clean_record_get(const middleware::Message& msg)
{
    (void)msg;
    publish_debug_clean_record_state();
}

void GlavoService::publish_debug_env_sim_state()
{
    middleware::DebugOnOffPayload p{};
    p.on = env_sim_on_ ? 1 : 0;
    auto send_to = [&](middleware::ModuleId target) {
        middleware::Message ntf(
            middleware::MsgType::GLAVO_Debug_Env_Sim_Ntf,
            middleware::ModuleId::APP_GLAVO,
            target);
        ntf.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        (void)publish_async(ntf);
    };
    send_to(middleware::ModuleId::UI);
    send_to(middleware::ModuleId::APP_MQTT);
}

void GlavoService::publish_debug_env_sim_powder_state()
{
    middleware::DebugOnOffPayload p{};
    p.on = env_sim_powder_low_on_ ? 1 : 0;
    middleware::Message ntf(
        middleware::MsgType::GLAVO_Debug_Env_Sim_Powder_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(ntf);
}

void GlavoService::handle_debug_env_sim_set(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    env_sim_on_ = (p.on != 0);
    McuLink::instance().set_env_sim_enabled(env_sim_on_);
    if(env_sim_on_) {
        /* 仅在 unknown 时补模拟版本，便于 About/MQTT 联调。 */
        McuLink::instance().publish_simulated_versions_if_unknown();
    }
    if(!env_sim_on_) {
        env_sim_powder_low_on_ = false;
        McuLink::instance().set_env_sim_powder_low(false);
    }
    UPC_LOG_GLAVO_SERVICE(Info, "debug env_sim=%d", env_sim_on_ ? 1 : 0);
    publish_debug_env_sim_state();
    publish_debug_env_sim_powder_state();
}

void GlavoService::handle_debug_env_sim_get(const middleware::Message& msg)
{
    (void)msg;
    publish_debug_env_sim_state();
    publish_debug_env_sim_powder_state();
}

void GlavoService::handle_debug_env_sim_powder_set(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    if(!env_sim_on_) {
        env_sim_powder_low_on_ = false;
        McuLink::instance().set_env_sim_powder_low(false);
        UPC_LOG_GLAVO_SERVICE(Info, "debug env_sim_powder ignored (env_sim off)");
        publish_debug_env_sim_powder_state();
        return;
    }
    env_sim_powder_low_on_ = (p.on != 0);
    McuLink::instance().set_env_sim_powder_low(env_sim_powder_low_on_);
    UPC_LOG_GLAVO_SERVICE(Info, "debug env_sim_powder_low=%d", env_sim_powder_low_on_ ? 1 : 0);
    publish_debug_env_sim_powder_state();
}

void GlavoService::handle_debug_env_sim_powder_get(const middleware::Message& msg)
{
    (void)msg;
    publish_debug_env_sim_powder_state();
}

void GlavoService::publish_debug_first_layer_visual_check_state()
{
    middleware::DebugOnOffPayload p{};
    p.on = first_layer_visual_check_on_ ? 1 : 0;
    middleware::Message ntf(
        middleware::MsgType::GLAVO_Debug_FirstLayer_Visual_Check_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(ntf);
}

bool GlavoService::publish_first_layer_visual_session(bool active,
                                                       const char* reason)
{
    if(active && !first_layer_visual_check_on_) {
        return false;
    }
    if(first_layer_visual_session_active_ == active) {
        return true;
    }

    const middleware::MsgType type = active
        ? middleware::MsgType::GLAVO_Visual_FirstLayer_Session_Start_Ntf
        : middleware::MsgType::GLAVO_Visual_FirstLayer_Session_Stop_Ntf;
    middleware::Message ntf(type,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::APP_VISUAL);
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "first layer visual session publish failed active=%d reason=%s",
            active ? 1 : 0, reason);
        return false;
    }
    first_layer_visual_session_active_ = active;
    UPC_LOG_GLAVO_SERVICE(Info,
        "first layer visual session active=%d reason=%s",
        active ? 1 : 0, reason);
    return true;
}

void GlavoService::handle_debug_first_layer_visual_check_set(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    first_layer_visual_check_on_ = (p.on != 0);
    UPC_LOG_GLAVO_SERVICE(Info, "debug first_layer_visual_check=%d",
                          first_layer_visual_check_on_ ? 1 : 0);
    if(!first_layer_visual_check_on_) {
        publish_first_layer_visual_session(false, "feature_disabled");
    } else if(print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "first-layer visual enabled during an active print; "
            "same-session B0 cannot be guaranteed, so it takes effect next print");
    }
    publish_debug_first_layer_visual_check_state();
}

void GlavoService::handle_debug_first_layer_visual_check_get(const middleware::Message& msg)
{
    (void)msg;
    publish_debug_first_layer_visual_check_state();
}
/* ============================================================================
 * Settings/About：MCU 固件版本查询
 * ============================================================================ */

/**
 * @brief 下发 TLV_GET_VERSION 查询帧
 * @note MCU 的 TLV_REPORT_VERSION 应答由 McuLink::handle_serial_rx_frame 直接推
 *       GLAVO_Mcu_Version_Ntf 给 UI，故此处只负责发请求，不等待回包。
 */
void GlavoService::handle_mcu_version_req(const middleware::Message& msg)
{
    (void)msg;
    uint8_t frame[16];
    const int n = serial_build_project_info_request(frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "mcu_version request build failed");
        return;
    }
    UPC_LOG_GLAVO_SERVICE(Info, "mcu_version query TX");
    debug_tx(frame, n);
}

/**
 * @brief 回读 TCP 0x0F 首包缓存并推 GLAVO_Galvo_Version_Ntf
 * @note 振镜板版本只在 TCP 连上后主动上报，此处不能再向下位机查询。
 */
void GlavoService::handle_galvo_version_req(const middleware::Message& msg)
{
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Info, "galvo_version query (cached)");
    McuLink::instance().publish_galvo_version_ntf();
}

void GlavoService::handle_alarm_reset(const middleware::Message& msg)
{
    middleware::AlarmResetReqPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "alarm_reset: bad payload");
        return;
    }
    uint8_t frame[16];
    const int n = serial_build_alarm_reset(p.alarm_code, frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "alarm_reset build failed code=%u",
                              static_cast<unsigned>(p.alarm_code));
        return;
    }
    UPC_LOG_GLAVO_SERVICE(Info, "alarm_reset TX code=%u",
                          static_cast<unsigned>(p.alarm_code));
    /* 通信 ACK 只表示 MCU 收到帧；复位是否成功以后续单条上报为准，
     * UI 侧在收到该 code 的下一帧或 Reset_Result 失败回执前保持"复位处理中"。 */
    debug_tx(frame, n);
}

/**
 * @brief 转发 UI 的报警快照请求（0xA3/0x81 TLV 0x05 len=0）
 * @note MCU 不会主动推快照，不发这条 UI 就永远补不齐重启/重连前就亮着的报警。
 *       攒帧、轮次判定与原子提交都在 UI 侧，这里只负责把请求发出去。
 */
void GlavoService::handle_alarm_snapshot_req(const middleware::Message& msg)
{
    (void)msg;
    uint8_t frame[16];
    const int n = serial_build_alarm_snapshot_request(frame, sizeof(frame));
    if(n <= 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "alarm_snapshot request build failed");
        return;
    }
    UPC_LOG_GLAVO_SERVICE(Info, "alarm_snapshot request TX");
    debug_tx(frame, n);
}

} // namespace upc::glavo
