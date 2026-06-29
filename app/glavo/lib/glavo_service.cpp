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

namespace {

constexpr size_t kWorkerQueueDepth = 64;

constexpr size_t kFsDirMaxEntries =
    (16384u - sizeof(middleware::FsDirHeader)) / sizeof(middleware::FileEntryPayload);

constexpr const char* kLocalGcodeDir  = "/home/Gcode";
constexpr const char* kUsbMountDir      = "/media/udisk0";
constexpr const char* kLocalConfigPath  = "/home/Gcode/config.json";

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

parser::IParserWorker* active_parser()
{
    return parser::ParserManager::instance().active();
}

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

bool is_local_dir_path(const std::string& path)
{
    return path.empty() || path == kLocalGcodeDir;
}

} // namespace

std::string GlavoService::payload_to_string(const middleware::MsgPayload& payload) {
    if (payload.type != middleware::MsgPayload::Type::Inline || payload.length == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data.bytes.data()), payload.length);
}

std::string GlavoService::make_full_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    if (dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

GlavoService::GlavoService() = default;

GlavoService::~GlavoService() {
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

GlavoService::ServiceState GlavoService::state() const noexcept {
    return state_.load();
}

uint32_t GlavoService::module_id() noexcept {
    return static_cast<uint32_t>(middleware::ModuleId::APP_GLAVO);
}

bool GlavoService::init() {
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
    gcode_manifest_load(kLocalConfigPath, local_manifest_);
    state_ = ServiceState::Idle;
    return true;
}

bool GlavoService::start() {
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
    };
    for(middleware::MsgType t : k_subs) {
        subscribe(t, [this](const middleware::Message& msg) { on_message(msg); });
    }

    worker_running_.store(true);
    worker_thread_ = std::thread(&GlavoService::worker_loop, this);
    return true;
}

bool GlavoService::stop() {
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

void GlavoService::on_message(const middleware::Message& msg) {
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

void GlavoService::worker_loop() {
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

bool GlavoService::publish_async(const middleware::Message& msg) {
    return middleware::MsgPort::instance().publish_async(msg);
}

void GlavoService::subscribe(middleware::MsgType type, middleware::MsgHandler handler) {
    auto sub = middleware::MsgPort::instance().subscribe(type,
        static_cast<middleware::ModuleId>(module_id()), std::move(handler));
    subscriptions_.push_back(std::move(sub));
}

void GlavoService::handle_message(const middleware::Message& msg) {
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
        default:
            break;
    }
}

void GlavoService::handle_supply_status_req(const middleware::Message& msg) {
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

void GlavoService::handle_pm25_request(const middleware::Message& msg) {
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

void GlavoService::handle_close_door_request(const middleware::Message& msg) {
    send_door_rsp(msg, middleware::MsgType::APP_CLOSE_DOOR_Rsp);
}

void GlavoService::handle_open_door_request(const middleware::Message& msg) {
    send_door_rsp(msg, middleware::MsgType::APP_OpenDoor_Rsp);
}

void GlavoService::handle_purify_request(const middleware::Message& msg) {
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Debug, "purify_req: not implemented");
}

void GlavoService::on_mcu_link_event(McuLinkEvent ev) {
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

void GlavoService::reset_print_progress_state() {
    last_print_pct_   = -1;
    last_parse_done_  = false;
}

void GlavoService::on_motion_batch_sent() {
    if(!print_running_.load()) {
        return;
    }
    send_print_progress(false);
}

void GlavoService::send_print_progress(bool force_complete) {
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

void GlavoService::publish_terminate_done() {
    middleware::Message ntf(middleware::MsgType::APP_Print_Terminate_Done,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    if (!publish_async(ntf)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "terminate_done publish_async failed");
    }
}

void GlavoService::send_dir_open_empty(const middleware::Message& req) {
    middleware::Message rsp(middleware::MsgType::Fs_DirOpen_Rsp,
        static_cast<middleware::ModuleId>(module_id()), req.sender_id);
    if (!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: publish_async empty rsp failed");
    }
}

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

void GlavoService::handle_dir_open(const middleware::Message& msg) {
    if(!parser::ParserRegistry::instance().has_any()) {
        send_dir_open_empty(msg);
        return;
    }

    const std::string path = payload_to_string(msg.payload);
    if(is_local_dir_path(path)) {
        gcode_manifest_load(kLocalConfigPath, local_manifest_);
        send_dir_open_entries(msg, local_manifest_, kLocalGcodeDir);
        return;
    }

    send_dir_open_usb(msg, path.empty() ? kUsbMountDir : path);
}

void GlavoService::handle_fs_delete_ntf(const middleware::Message& msg) {
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

    gcode_manifest_load(kLocalConfigPath, local_manifest_);
    const size_t idx = static_cast<size_t>(req.file_index);
    if(idx >= local_manifest_.size()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: index out of range %u", req.file_index);
        return;
    }

    const std::string name = local_manifest_[idx].name;
    const std::string full_path = make_full_path(kLocalGcodeDir, name);
    if(std::remove(full_path.c_str()) != 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: remove file failed %s", full_path.c_str());
    }

    if(!gcode_manifest_remove_at(local_manifest_, idx)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: manifest remove failed");
        return;
    }
    if(!gcode_manifest_save(kLocalConfigPath, local_manifest_)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "fs_delete_ntf: config save failed");
        gcode_manifest_load(kLocalConfigPath, local_manifest_);
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

void GlavoService::clear_prepare_session()
{
    prepared_local_path_.clear();
    prepare_ready_ = false;
}

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
        gcode_manifest_load(kLocalConfigPath, local_manifest_);
    }

    std::string basename;
    if(source == middleware::PrintFileSource::Usb) {
        if(index >= usb_entries_.size()) {
            return false;
        }
        basename = usb_entries_[index];
        const std::string src = make_full_path(kUsbMountDir, basename);
        const std::string dst = make_full_path(kLocalGcodeDir, basename);
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
        local_path_out = make_full_path(kLocalGcodeDir, basename);
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
        gcode_manifest_load(kLocalConfigPath, local_manifest_);
    }
    gcode_manifest_upsert(local_manifest_, entry);
    if(!gcode_manifest_save(kLocalConfigPath, local_manifest_)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "prepare: config save failed");
        return false;
    }

    prepared_local_path_ = local_path_out;
    prepare_ready_ = true;
    return true;
}

void GlavoService::handle_print_prepare(const middleware::Message& msg) {
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

void GlavoService::handle_parser_exit(const middleware::Message& msg) {
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

void GlavoService::handle_print_start(const middleware::Message& msg) {
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

void GlavoService::stop_print_job() {
    print_running_.store(false);
    parser::ParserManager::instance().stop_parse();
}

void GlavoService::handle_print_terminate(const middleware::Message& msg) {
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Info, "print_terminate requested");
    print_running_.store(false);
    PrintVideoNotify::instance().on_print_abort();
    parser::ParserManager::instance().stop_parse();
    McuLink::instance().request_terminate();
    clear_prepare_session();
}

void GlavoService::handle_mcu_ota_start(const middleware::Message& msg) {
    (void)msg;
    if(McuLink::instance().ota_active()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "mcu_ota_start: already active");
        return;
    }
    if(!McuLink::instance().start_mcu_ota(GLAVO_OTA_IMAGE_DIR)) {
        middleware::McuOtaStatusPayload st{};
        st.state    = middleware::McuOtaState::Failed;
        st.progress = 0;
        publish_mcu_ota_status(st);
    }
}

void GlavoService::handle_mcu_ota_cancel(const middleware::Message& msg) {
    (void)msg;
    McuLink::instance().cancel_mcu_ota();
}

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

} // namespace upc::glavo
