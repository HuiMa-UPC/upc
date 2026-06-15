#include "glavo_service.hpp"
#include "glavo_parser_bridge.hpp"
#include "parser_config.hpp"
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
#include <cstring>
#include <cstdint>
#include <string>

namespace upc::glavo {

namespace parser = upc::parser;

namespace {

constexpr size_t kFsDirMaxEntries =
    (16384u - sizeof(middleware::FsDirHeader)) / sizeof(middleware::FileEntryPayload);

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

/** 打印进度分母：解析完成后用最终包数；解析中用行数比例外推，避免 sent/已解析 虚高 */
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

GlavoService::GlavoService()
    : ServiceBase(ServiceConfig{
          "glavo",
          static_cast<uint32_t>(middleware::ModuleId::APP_GLAVO),
          true
      }) {}

GlavoService::~GlavoService() {
    stop_print_job();
}

bool GlavoService::do_init() {
    parser::ParserContext::instance().load_calibration(CAL_DEFAULT_FILE_PATH);
    glavo_parser_bind_init();
    McuLink::instance().set_job_event_callback(
        [this](McuLinkEvent ev) { on_mcu_link_event(ev); });
    McuLink::instance().set_progress_callback(
        [this]() { on_motion_batch_sent(); });
    return true;
}

bool GlavoService::do_start() {
    subscribe(middleware::MsgType::Fs_DirOpen_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_FileInfo_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_Print_Start_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_Print_Terminate_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_PM25_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_CLOSE_DOOR_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_OpenDoor_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_Purify_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    return true;
}

bool GlavoService::do_stop() {
    stop_print_job();
    return true;
}

void GlavoService::handle_message(const middleware::Message& msg) {
    switch (msg.type) {
        case middleware::MsgType::Fs_DirOpen_Req:
            handle_dir_open(msg);
            break;
        case middleware::MsgType::UI_FileInfo_Req:
            handle_file_info_req(msg);
            break;
        case middleware::MsgType::UI_Print_Start_Req:
            handle_print_start(msg);
            break;
        case middleware::MsgType::UI_Print_Terminate_Req:
            handle_print_terminate(msg);
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
        default:
            break;
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
    } else {
        UPC_LOG_GLAVO_SERVICE(Debug, "pm25_rsp high=%u cabin=%.1f",
            p.is_high, p.pm25_cabin);
    }
}

void GlavoService::handle_close_door_request(const middleware::Message& msg) {
    middleware::DoorResultPayload p{};
    p.ok = 1;

    middleware::Message rsp(middleware::MsgType::APP_CLOSE_DOOR_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "close_door_rsp publish_async failed");
    }
}

void GlavoService::handle_open_door_request(const middleware::Message& msg) {
    middleware::DoorResultPayload p{};
    p.ok = 1;

    middleware::Message rsp(middleware::MsgType::APP_OpenDoor_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "open_door_rsp publish_async failed");
    }
}

void GlavoService::handle_purify_request(const middleware::Message& msg) {
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Debug, "purify_req: not implemented");
}

void GlavoService::on_mcu_link_event(McuLinkEvent ev) {
    switch(ev) {
        case McuLinkEvent::PrintCompleted:
            UPC_LOG_GLAVO_SERVICE(Info, "tcp_job_event: PrintCompleted");
            send_print_progress(true);
            print_running_.store(false);
            break;
        case McuLinkEvent::TerminateDone:
            UPC_LOG_GLAVO_SERVICE(Info, "tcp_job_event: TerminateDone");
            print_running_.store(false);
            publish_terminate_done();
            break;
        case McuLinkEvent::PrintError:
            UPC_LOG_GLAVO_SERVICE(Warn, "tcp_job_event: PrintError");
            print_running_.store(false);
            publish_terminate_done();
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

void GlavoService::handle_dir_open(const middleware::Message& msg) {
    if(!parser::ParserRegistry::instance().has_any()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: no parser registered, returning empty list");
        send_dir_open_empty(msg);
        return;
    }

    std::string path = payload_to_string(msg.payload);
    if (path.empty()) {
        path = "/home/Gcode";
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        send_dir_open_empty(msg);
        return;
    }

    current_dir_path_ = path;
    current_entries_.clear();
    current_entries_.reserve(kFsDirMaxEntries);

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (current_entries_.size() >= kFsDirMaxEntries) {
            UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: truncated at %zu entries", kFsDirMaxEntries);
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if(!parser::ParserRegistry::instance().match_filename(entry->d_name)) {
            continue;
        }
        current_entries_.push_back(entry->d_name);
    }
    closedir(dir);

    const uint32_t count = static_cast<uint32_t>(current_entries_.size());
    const size_t header_size = sizeof(middleware::FsDirHeader);
    const size_t entries_size = static_cast<size_t>(count) * sizeof(middleware::FileEntryPayload);
    const size_t total_size = header_size + entries_size;

    if (count == 0) {
        send_dir_open_empty(msg);
        return;
    }

    const uint32_t buf_id = middleware::BufferPool::instance().allocate(total_size);
    if (buf_id == middleware::INVALID_BUFFER_ID) {
        UPC_LOG_GLAVO_SERVICE(Error, "dir_open: BufferPool allocate failed (size=%zu)", total_size);
        send_dir_open_empty(msg);
        return;
    }

    uint8_t* base = middleware::BufferPool::instance().get_ptr(buf_id);
    if (!base) {
        middleware::BufferPool::instance().release(buf_id);
        send_dir_open_empty(msg);
        return;
    }

    auto* hdr = reinterpret_cast<middleware::FsDirHeader*>(base);
    hdr->count = count;
    auto* files = reinterpret_cast<middleware::FileEntryPayload*>(base + header_size);
    for (uint32_t i = 0; i < count; i++) {
        files[i].index = i;
        snprintf(files[i].name, sizeof(files[i].name), "%s", current_entries_[i].c_str());
        const std::string full = make_full_path(path, current_entries_[i]);
        snprintf(files[i].path, sizeof(files[i].path), "%s", full.c_str());
        files[i].duration_min = 0;
        files[i].material_g   = 0.0f;
    }

    middleware::Message rsp(middleware::MsgType::Fs_DirOpen_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_buffer(buf_id, 0, static_cast<uint32_t>(total_size));
    if (!publish_async(rsp)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "dir_open: publish_async failed (count=%u)", count);
    }
}

void GlavoService::handle_file_info_req(const middleware::Message& msg) {
    middleware::PrintStartReqPayload req{};
    if (!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "file_info_req: invalid payload");
        return;
    }

    middleware::FileInfoPayload rsp{};
    rsp.file_index = req.file_index;
    rsp.ok = 0;
    rsp.total_lines = 0;
    rsp.file_size = 0;

    const size_t idx = static_cast<size_t>(req.file_index);
    if (idx < current_entries_.size()) {
        const std::string full_path = make_full_path(current_dir_path_, current_entries_[idx]);
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            rsp.file_size = static_cast<uint32_t>(st.st_size);
            stop_print_job();
            if(McuLink::instance().is_sending()) {
                McuLink::instance().request_terminate();
            }
            if(parser::ParserManager::instance().open_file(full_path) == 0) {
                parser::IParserWorker* parser = active_parser();
                rsp.total_lines = parser
                    ? static_cast<uint32_t>(parser->get_total_lines())
                    : 0u;
                rsp.ok = 1;
                current_file_path_ = full_path;
                PrintJobState::instance().reset_job_counters();
                reset_print_progress_state();
                parser::ParserManager::instance().start_parse();
            }
        }
    }

    middleware::Message out(middleware::MsgType::APP_FileInfo_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    if (!publish_async(out)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "file_info_req: publish_async failed");
    }
}

void GlavoService::handle_print_start(const middleware::Message& msg) {
    middleware::PrintStartReqPayload req{};
    if (!read_inline_pod(msg.payload, req)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_start: invalid payload");
        return;
    }

    const size_t idx = static_cast<size_t>(req.file_index);
    if (idx >= current_entries_.size()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_start: index out of range");
        return;
    }

    const std::string full_path = make_full_path(current_dir_path_, current_entries_[idx]);

    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_start: stat failed");
        return;
    }

    parser::IParserWorker* parser = active_parser();
    const bool already_parsing = (full_path == current_file_path_) &&
                                 parser &&
                                 parser->is_file_open() &&
                                 parser->is_parsing();

    if(!already_parsing) {
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
    }

    print_running_.store(true);
    reset_print_progress_state();
    McuLink::instance().begin_sending();

    UPC_LOG_GLAVO_SERVICE(Info, "print_start: index=%u", req.file_index);
}

void GlavoService::stop_print_job() {
    print_running_.store(false);
    parser::ParserManager::instance().stop_parse();
}

void GlavoService::handle_print_terminate(const middleware::Message& msg) {
    (void)msg;
    UPC_LOG_GLAVO_SERVICE(Info, "print_terminate requested");
    print_running_.store(false);
    parser::ParserManager::instance().stop_parse();
    McuLink::instance().request_terminate();
}

} // namespace upc::glavo
