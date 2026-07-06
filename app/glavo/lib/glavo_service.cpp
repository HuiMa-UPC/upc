/**
 * @file glavo_service.cpp
 * @brief Glavo 侧 IPC 服务实现：订阅 UI 请求、驱动解析/打印/McuLink，并推送 Rsp/Ntf。
 *
 * 消息在 worker 线程中串行处理，避免与解析器/McuLink 产生竞态。
 * 契约见 common/contract/README.md。
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
#include "msg_types.hpp"
#include "buffer_pool.hpp"
#include "rs_log.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <string>

namespace upc::glavo {

namespace parser = upc::parser;

/* ============================================================================
 * 文件内辅助常量与工具函数
 * ============================================================================ */

namespace {

constexpr size_t kWorkerQueueDepth = 64;

constexpr size_t kFsDirMaxEntries =
    (16384u - sizeof(middleware::FsDirHeader)) / sizeof(middleware::FileEntryPayload);

/** @brief 本地 G-code 清单配置文件路径 */
std::string local_config_path()
{
    return std::string(GLAVO_LOCAL_GCODE_DIR) + "/config.json";
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

    const int parsed_lines = parser->get_parsed_lines();
    const int total_lines  = parser->get_total_lines();
    if(parsed_lines > 0 && total_lines > parsed_lines) {
        const int64_t estimated =
            (static_cast<int64_t>(parsed_packets) * total_lines) / parsed_lines;
        if(estimated > parsed_packets) {
            return static_cast<int>(estimated);
        }
    }

    return parsed_packets + sent_packets + 1;
}

/** @brief float → int16_t（四舍五入 + 限幅），用于轴速度/增量、风扇占空比换算 */
int16_t debug_round_i16(float f)
{
    if(f > 32767.0f)  f = 32767.0f;
    if(f < -32768.0f) f = -32768.0f;
    return static_cast<int16_t>(f >= 0.0f ? f + 0.5f : f - 0.5f);
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
 * 生命周期
 * ============================================================================ */

GlavoService::GlavoService() = default;

/** @brief 析构时停止打印、清空会话并 join worker 线程 */
GlavoService::~GlavoService()
{
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
 * @brief 初始化解析器、McuLink 回调与本地 G-code 清单
 * @return 仅 Idle 状态下可调用，成功返回 true
 */
bool GlavoService::init()
{
    if (state() != ServiceState::Idle) return false;

    state_ = ServiceState::Initializing;
    parser::ParserContext::instance().load_calibration(CAL_DEFAULT_FILE_PATH);
    parser::ParserManager::instance().set_ring_buffer_sink(
        [](parser::RingBuffer* rb) {
            McuLink::instance().bind_ring_buffer(rb);
        });
    McuLink::instance().set_job_event_callback(
        [this](McuLinkEvent ev) { on_mcu_link_event(ev); });
    McuLink::instance().set_progress_callback(
        [this]() { on_motion_batch_sent(); });
    McuLink::instance().set_ota_status_callback(
        [this](const middleware::McuOtaStatusPayload& st) {
            publish_mcu_ota_status(st);
        });
    gcode_manifest_load(local_config_path(), local_manifest_);
    state_ = ServiceState::Idle;
    return true;
}

/**
 * @brief 订阅 IPC 消息并启动 worker 线程
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
        middleware::MsgType::UI_Supply_Status_Req,
        middleware::MsgType::UI_PM25_Req,
        middleware::MsgType::UI_CLOSE_DOOR_Req,
        middleware::MsgType::UI_OpenDoor_Req,
        middleware::MsgType::UI_Purify_Req,
        middleware::MsgType::UI_Mcu_Ota_Start_Req,
        middleware::MsgType::UI_Mcu_Ota_Cancel_Ntf,
        /* 调试页手动控制（振镜/回粉电机预留，不订阅） */
        middleware::MsgType::UI_Debug_Axis_Jog_Req,
        middleware::MsgType::UI_Debug_Axis_Hold_Stop_Req,
        middleware::MsgType::UI_Debug_Axis_SetZero_Req,
        middleware::MsgType::UI_Debug_Axis_Home_Req,
        middleware::MsgType::UI_Debug_Device_Req,
        middleware::MsgType::UI_Debug_Powder_Bucket_Req,
        middleware::MsgType::UI_Debug_Pusher_Req,
        middleware::MsgType::UI_Debug_Recycle_Motor_Req,   /* 内门（原回粉电机块） */
        middleware::MsgType::UI_Debug_Fan_Req,
        middleware::MsgType::UI_Debug_Fan_Duty_Req,
        middleware::MsgType::UI_Debug_Switch_Req,
        middleware::MsgType::UI_Debug_CleanTest_Start_Req,
        middleware::MsgType::UI_Debug_PrintParam_Set_Req,
        middleware::MsgType::UI_Debug_Laser_Power_Set_Req,
        middleware::MsgType::UI_Debug_Laser_Power_Reset_Req,
        middleware::MsgType::UI_Debug_Laser_AlarmReset_Req,
    };
    for(middleware::MsgType t : k_subs) {
        subscribe(t, [this](const middleware::Message& msg) { on_message(msg); });
    }

    worker_running_.store(true);
    worker_thread_ = std::thread(&GlavoService::worker_loop, this);
    return true;
}

/**
 * @brief 停止 worker、清空队列与订阅，并终止进行中的打印/准备会话
 * @return 仅 Running 状态下可调用，成功返回 true
 */
bool GlavoService::stop()
{
    if (state() != ServiceState::Running) return false;

    state_ = ServiceState::Stopping;

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
    }

    stop_print_job();
    clear_prepare_session();
    subscriptions_.clear();
    state_ = ServiceState::Idle;
    return true;
}

/* ============================================================================
 * 消息队列与分发
 * ============================================================================ */

/** @brief IPC 回调入口：将消息入队，队列满则丢弃并打日志 */
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

/** @brief worker 线程主循环：出队并在 Running 状态下分派 handle_message */
void GlavoService::worker_loop()
{
    while (worker_running_.load()) {
        middleware::Message msg;

        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this]() {
                return !worker_running_.load() || !worker_queue_.empty();
            });

            if (!worker_running_.load() && worker_queue_.empty()) {
                break;
            }

            if (worker_queue_.empty()) {
                continue;
            }

            msg = std::move(worker_queue_.front());
            worker_queue_.pop();
        }

        if (state() == ServiceState::Running) {
            handle_message(msg);
        }
    }
}

/** @brief 异步发布 IPC 消息 */
bool GlavoService::publish_async(const middleware::Message& msg)
{
    return middleware::MsgPort::instance().publish_async(msg);
}

/** @brief 订阅指定 MsgType 并记录 Subscription 句柄，stop 时统一释放 */
void GlavoService::subscribe(middleware::MsgType type, middleware::MsgHandler handler)
{
    auto sub = middleware::MsgPort::instance().subscribe(type,
        static_cast<middleware::ModuleId>(module_id()), std::move(handler));
    subscriptions_.push_back(std::move(sub));
}

/** @brief 按消息类型分派到具体 handler */
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
        case middleware::MsgType::UI_Mcu_Ota_Start_Req:
            handle_mcu_ota_start(msg);
            break;
        case middleware::MsgType::UI_Mcu_Ota_Cancel_Ntf:
            handle_mcu_ota_cancel(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_Jog_Req:
            handle_debug_axis_jog(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_Hold_Stop_Req:
            handle_debug_axis_hold_stop(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_SetZero_Req:
            handle_debug_axis_set_zero(msg);
            break;
        case middleware::MsgType::UI_Debug_Axis_Home_Req:
            handle_debug_axis_home(msg);
            break;
        case middleware::MsgType::UI_Debug_Device_Req:
            handle_debug_device(msg);
            break;
        case middleware::MsgType::UI_Debug_Powder_Bucket_Req:
            handle_debug_powder_bucket(msg);
            break;
        case middleware::MsgType::UI_Debug_Pusher_Req:
            handle_debug_pusher(msg);
            break;
        case middleware::MsgType::UI_Debug_Recycle_Motor_Req:
            handle_debug_inner_door(msg);
            break;
        case middleware::MsgType::UI_Debug_Fan_Req:
            handle_debug_fan(msg);
            break;
        case middleware::MsgType::UI_Debug_Fan_Duty_Req:
            handle_debug_fan_duty(msg);
            break;
        case middleware::MsgType::UI_Debug_Switch_Req:
            handle_debug_switch(msg);
            break;
        case middleware::MsgType::UI_Debug_CleanTest_Start_Req:
            handle_debug_clean_test_start(msg);
            break;
        case middleware::MsgType::UI_Debug_PrintParam_Set_Req:
            handle_debug_print_param(msg);
            break;
        case middleware::MsgType::UI_Debug_Laser_Power_Set_Req:
            handle_debug_laser_power_set(msg);
            break;
        case middleware::MsgType::UI_Debug_Laser_Power_Reset_Req:
            handle_debug_laser_power_reset(msg);
            break;
        case middleware::MsgType::UI_Debug_Laser_AlarmReset_Req:
            handle_debug_laser_alarm_reset(msg);
            break;
        default:
            break;
    }
}

/* ============================================================================
 * 文件系统：目录浏览与删除
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

/**
 * @brief 用本地 manifest 条目组 Fs_DirOpen_Rsp 并通过 BufferPool 发送
 * @param base_path 条目 path 字段的前缀目录
 */
void GlavoService::send_dir_open_entries(const middleware::Message& req,
                                         const std::vector<GcodeManifestEntry>& entries,
                                         const std::string& base_path)
{
    const uint32_t count = static_cast<uint32_t>(entries.size());
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
        const auto& e = entries[i];
        files[i].index = i;
        snprintf(files[i].name, sizeof(files[i].name), "%s", e.name.c_str());
        const std::string full = make_full_path(base_path, e.name);
        snprintf(files[i].path, sizeof(files[i].path), "%s", full.c_str());
        files[i].duration_min = e.duration_sec / 60u;
        files[i].material_g   = e.material_g;
        files[i].file_size    = e.file_size;
        files[i].line_count   = e.line_count;
    }

    middleware::Message rsp(middleware::MsgType::Fs_DirOpen_Rsp,
        static_cast<middleware::ModuleId>(module_id()), req.sender_id);
    rsp.payload = middleware::MsgPayload::from_buffer(buf_id, 0, static_cast<uint32_t>(total_size));
    if (!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: publish_async failed (count=%u)", count);
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

/** @brief 处理 Fs_DirOpen_Req：本地清单或 USB 目录列表 */
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

    if(req.list_source == middleware::PrintFileSource::Local) {
        gcode_manifest_load(local_config_path(), local_manifest_);
        send_dir_open_entries(msg, local_manifest_, GLAVO_LOCAL_GCODE_DIR);
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
    middleware::FsDeleteNtfPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: invalid payload");
        return;
    }
    if(req.file_source != middleware::PrintFileSource::Local) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: only Local supported");
        return;
    }
    if(print_running_.load()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: ignored while printing");
        return;
    }

    gcode_manifest_load(local_config_path(), local_manifest_);
    const size_t idx = static_cast<size_t>(req.file_index);
    if(idx >= local_manifest_.size()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: index out of range %u", req.file_index);
        return;
    }

    const std::string name = local_manifest_[idx].name;
    const std::string full_path = make_full_path(GLAVO_LOCAL_GCODE_DIR, name);
    if(std::remove(full_path.c_str()) != 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: remove file failed %s", full_path.c_str());
    }

    if(!gcode_manifest_remove_at(local_manifest_, idx)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: manifest remove failed");
        return;
    }
    if(!gcode_manifest_save(local_config_path(), local_manifest_)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: config save failed");
        gcode_manifest_load(local_config_path(), local_manifest_);
        return;
    }

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
 * ============================================================================ */

/** @brief 清除 print prepare 会话状态（prepared_local_path_ / prepare_ready_） */
void GlavoService::clear_prepare_session()
{
    prepared_local_path_.clear();
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

    uint32_t line_count = 0;
    if(!light_scan_line_count(local_path_out, line_count)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "prepare: light scan failed %s", local_path_out.c_str());
        return false;
    }

    struct stat st{};
    if(stat(local_path_out.c_str(), &st) != 0) {
        return false;
    }

    GcodeManifestEntry entry;
    entry.name        = basename;
    entry.file_size   = static_cast<uint32_t>(st.st_size);
    entry.line_count  = line_count;
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
    prepare_ready_ = true;
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

/** @brief 处理 UI_Print_Start_Req：在已 prepare 的文件上启动解析与 McuLink 发送 */
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
    if(McuLink::instance().is_sending()) {
        McuLink::instance().request_terminate();
    }
    if(parser::ParserManager::instance().open_file(full_path) != 0) {
        UPC_LOG_GLAVO_SERVICE(Error, "print_start: open failed");
        return;
    }
    current_file_path_ = full_path;
    PrintJobState::instance().reset_job_counters();
    reset_print_progress_state();
    parser::ParserManager::instance().start_parse();

    print_running_.store(true);
    PrintVideoNotify::instance().on_print_start(current_file_path_);
    McuLink::instance().begin_sending();

    UPC_LOG_GLAVO_SERVICE(Info, "print_start: path=%s", full_path.c_str());
}

/** @brief 停止解析线程并将 print_running_ 置 false（不请求 Mcu 终止） */
void GlavoService::stop_print_job()
{
    print_running_.store(false);
    parser::ParserManager::instance().stop_parse();
}

/** @brief 处理 UI_Print_Terminate_Req：停止解析并请求 McuLink 终止作业 */
void GlavoService::handle_print_terminate(const middleware::Message& msg)
{
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Info, "print_terminate requested");
    print_running_.store(false);
    PrintVideoNotify::instance().on_print_abort();
    parser::ParserManager::instance().stop_parse();
    McuLink::instance().request_terminate();
    clear_prepare_session();
}

/** @brief 重置进度推送的去重状态（last_print_pct_ / last_parse_done_） */
void GlavoService::reset_print_progress_state()
{
    last_print_pct_   = -1;
    last_parse_done_  = false;
}

/** @brief McuLink 每发送一批 motion 数据后的回调，驱动进度 Ntf */
void GlavoService::on_motion_batch_sent()
{
    if(!print_running_.load()) {
        return;
    }
    send_print_progress(false);
}

/**
 * @brief 计算并推送 APP_Print_Progress_Ntf
 * @param force_complete true 时强制 100%（打印完成事件）
 *
 * 解析未完成时进度上限 95%；解析完成但未 job_completed 时最高 99%。
 */
void GlavoService::send_print_progress(bool force_complete)
{
    const auto job = PrintJobState::instance().snapshot();

    parser::IParserWorker* parser = active_parser();
    const bool parse_done = parser &&
                            parser->is_file_read_exhausted() &&
                            !parser->is_parsing();

    if(parse_done && !last_parse_done_) {
        last_print_pct_ = -1;
    }
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
        pct = percent_rounded(job.sent_packets, denom);
        if(!parse_done) {
            if(pct > 95) {
                pct = 95;
            }
        } else if(pct >= 100) {
            pct = 99;
        }
    }

    if(!force_complete && !job.job_completed) {
        if(last_print_pct_ >= 0) {
            if(pct <= last_print_pct_) {
                return;
            }
            if(pct - last_print_pct_ < 1) {
                return;
            }
        }
    } else if(last_print_pct_ >= 100) {
        return;
    }

    last_print_pct_ = pct;

    const auto env = McuLink::instance().env_snapshot();

    middleware::PrintProgressPayload p{};
    p.progress_percent  = pct;
    p.remaining_minutes = 0;
    p.pm25_indoor       = env.valid ? env.pm25_indoor : 0.f;
    p.pm25_cabin        = env.valid ? env.pm25_cabin : 0.f;
    p.temperature_c     = env.valid ? env.temperature_c : 0.f;
    p.oxygen_percent    = env.valid ? env.oxygen_percent : 0.f;

    middleware::Message ntf(middleware::MsgType::APP_Print_Progress_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_progress publish failed pct=%d", pct);
    }
}

/** @brief 向 UI 推送 APP_Print_Terminate_Done */
void GlavoService::publish_terminate_done()
{
    middleware::Message ntf(middleware::MsgType::APP_Print_Terminate_Done,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    if (!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "terminate_done publish_async failed");
    }
}

/**
 * @brief McuLink 作业事件回调：完成/终止/错误时更新 UI 与本地状态
 *
 * PrintCompleted → 100% 进度 + 清会话；
 * TerminateDone/PrintError → 终止完成 Ntf + 清会话。
 */
void GlavoService::on_mcu_link_event(McuLinkEvent ev)
{
    switch(ev) {
        case McuLinkEvent::PrintCompleted:
            UPC_LOG_GLAVO_SERVICE(Info, "tcp_job_event: PrintCompleted");
            PrintVideoNotify::instance().on_print_finish();
            send_print_progress(true);
            print_running_.store(false);
            clear_prepare_session();
            break;
        case McuLinkEvent::TerminateDone:
        case McuLinkEvent::PrintError:
            if(ev == McuLinkEvent::TerminateDone) {
                UPC_LOG_GLAVO_SERVICE(Info, "tcp_job_event: TerminateDone");
            } else {
                UPC_LOG_GLAVO_SERVICE(Warn, "tcp_job_event: PrintError");
            }
            PrintVideoNotify::instance().on_print_abort();
            print_running_.store(false);
            publish_terminate_done();
            clear_prepare_session();
            break;
    }
}

/* ============================================================================
 * UI 设备状态与控制（耗材 / 环境 / 门 / 净化）
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
    const auto env = McuLink::instance().env_snapshot();
    middleware::Pm25ResultPayload p{};
    p.is_high   = (env.valid && env.pm25_cabin > 35.0f) ? 1 : 0;
    p.pm25_cabin = env.valid ? env.pm25_cabin : 0.f;

    middleware::Message rsp(middleware::MsgType::APP_PM25_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "pm25_rsp publish_async failed");
    }
}

/** @brief 发送门控操作结果 Rsp（ok=1 占位） */
void GlavoService::send_door_rsp(const middleware::Message& msg,
                                 middleware::MsgType rsp_type)
{
    middleware::DoorResultPayload p{};
    p.ok = 1;

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
    send_door_rsp(msg, middleware::MsgType::APP_CLOSE_DOOR_Rsp);
}

/** @brief 处理 UI_OpenDoor_Req */
void GlavoService::handle_open_door_request(const middleware::Message& msg)
{
    send_door_rsp(msg, middleware::MsgType::APP_OpenDoor_Rsp);
}

/** @brief 处理 UI_Purify_Req（尚未实现业务逻辑） */
void GlavoService::handle_purify_request(const middleware::Message& msg)
{
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Debug, "purify_req: not implemented");
}

/* ============================================================================
 * MCU OTA
 * ============================================================================ */

/** @brief 处理 UI_Mcu_Ota_Start_Req，启动 McuLink OTA 会话 */
void GlavoService::handle_mcu_ota_start(const middleware::Message& msg)
{
    (void)msg;
    if(McuLink::instance().ota_active()) {
        UPC_LOG_MCU_OTA(Warn, "mcu_ota_start: already active");
        return;
    }
    if(!McuLink::instance().start_mcu_ota(GLAVO_OTA_IMAGE_DIR)) {
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
    middleware::Message out(middleware::MsgType::APP_Mcu_Ota_Status_Ntf,
                            middleware::ModuleId::APP_GLAVO,
                            middleware::ModuleId::UI);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&st), sizeof(st));
    (void)publish_async(out);
}

/* ============================================================================
 * 调试页手动控制：UI_Debug_* → serial_debug 组帧 → McuLink 串口下发
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

/** @brief 风扇占空比（协议合法范围 35–100） */
void GlavoService::handle_debug_fan_duty(const middleware::Message& msg)
{
    middleware::DebugFanPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        return;
    }
    int16_t duty = debug_round_i16(p.duty);
    if(duty < 35)  duty = 35;
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

/** @brief 启动清洁测试流程 */
void GlavoService::handle_debug_clean_test_start(const middleware::Message& msg)
{
    (void)msg;
    uint8_t buf[SERIAL_DEBUG_MAX_FRAME];
    debug_tx(buf, serial_debug_build_clean_test_start(buf, sizeof(buf)));
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

} // namespace upc::glavo
