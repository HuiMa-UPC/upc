#include "glavo_service.hpp"
#include "gcode_worker.hpp"
#include "msg_types.hpp"
#include "buffer_pool.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

namespace services {

namespace {

/** 适配 MemPool 16KB 档块：header + N × FileEntryPayload */
constexpr size_t kFsDirMaxEntries =
    (16384u - sizeof(middleware::FsDirHeader)) / sizeof(middleware::FileEntryPayload);

/** 检查文件名是否匹配允许的后缀 */
bool has_allowed_ext(const std::string& name) {
    static const char* allowed[] = {".txt", ".nc", ".gcode"};
    for (const char* ext : allowed) {
        const size_t ext_len = std::strlen(ext);
        if (name.size() >= ext_len &&
            std::strcmp(name.c_str() + name.size() - ext_len, ext) == 0) {
            return true;
        }
    }
    return false;
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
    GcodeWorker::instance().parser_load_gains();
    return true;
}

bool GlavoService::do_start() {
    subscribe(middleware::MsgType::Fs_DirOpen_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_Print_Start_Req,
        [this](const middleware::Message& msg) { on_message(msg); });
    subscribe(middleware::MsgType::UI_Print_Terminate_Req,
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
        case middleware::MsgType::UI_Print_Start_Req:
            handle_print_start(msg);
            break;
        case middleware::MsgType::UI_Print_Terminate_Req:
            handle_print_terminate(msg);
            break;
        default:
            break;
    }
}

void GlavoService::send_dir_open_empty(const middleware::Message& req) {
    middleware::Message rsp(middleware::MsgType::Fs_DirOpen_Rsp,
        static_cast<middleware::ModuleId>(module_id()), req.sender_id);
    if (!publish_async(rsp)) {
#if SERVICES_DEBUG
        printf("[GlavoService] dir_open: publish_async empty rsp failed\n");
#endif
    }
}

void GlavoService::handle_dir_open(const middleware::Message& msg) {
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
#if SERVICES_DEBUG
            printf("[GlavoService] dir_open: truncated at %zu entries\n", kFsDirMaxEntries);
#endif
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!has_allowed_ext(entry->d_name)) {
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
#if SERVICES_DEBUG
        printf("[GlavoService] dir_open: BufferPool allocate failed (size=%zu)\n", total_size);
#endif
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
#if SERVICES_DEBUG
        printf("[GlavoService] dir_open: publish_async failed (count=%u)\n", count);
#endif
    }

#if SERVICES_DEBUG
    printf("[GlavoService] dir_open: path=%s count=%u size=%zu\n", path.c_str(), count, total_size);
#endif
}

void GlavoService::handle_print_start(const middleware::Message& msg) {
    middleware::PrintStartReqPayload req{};
    if (!read_inline_pod(msg.payload, req)) {
#if SERVICES_DEBUG
        printf("[GlavoService] print_start: invalid payload\n");
#endif
        send_print_progress(100);
        return;
    }

    const size_t idx = static_cast<size_t>(req.file_index);
    if (idx >= current_entries_.size()) {
#if SERVICES_DEBUG
        printf("[GlavoService] print_start: index %zu out of range (count=%zu)\n",
            idx, current_entries_.size());
#endif
        send_print_progress(100);
        return;
    }

    const std::string full_path = make_full_path(current_dir_path_, current_entries_[idx]);

    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
#if SERVICES_DEBUG
        printf("[GlavoService] print_start: stat failed path=%s\n", full_path.c_str());
#endif
        send_print_progress(100);
        return;
    }

    stop_print_job();

    if (GcodeWorker::instance().open_file(full_path) != 0) {
#if SERVICES_DEBUG
        printf("[GlavoService] print_start: open failed path=%s\n", full_path.c_str());
#endif
        send_print_progress(100);
        return;
    }

    GcodeWorker::instance().start_parse();
    print_running_.store(true);
    print_progress_thread_ = std::thread(&GlavoService::print_progress_thread_fn, this);

#if SERVICES_DEBUG
    printf("[GlavoService] print_start: index=%u path=%s\n",
        req.file_index, full_path.c_str());
#endif
}

void GlavoService::send_print_progress(int progress_percent) {
    middleware::PrintProgressPayload p{};
    p.progress_percent = progress_percent;
    p.remaining_minutes = 0;
    p.pm25_indoor = 0.0f;
    p.pm25_cabin = 0.0f;
    p.temperature_c = 0.0f;
    p.oxygen_percent = 0.0f;

    middleware::Message ntf(middleware::MsgType::APP_Print_Progress_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if (!publish_async(ntf)) {
#if SERVICES_DEBUG
        printf("[GlavoService] print progress publish failed (%d%%)\n", progress_percent);
#endif
    }
}

void GlavoService::stop_print_job() {
    print_running_.store(false);
    GcodeWorker::instance().stop_parse();
    if (print_progress_thread_.joinable()) {
        print_progress_thread_.join();
    }
}

void GlavoService::print_progress_thread_fn() {
    while (print_running_.load()) {
        const int total = GcodeWorker::instance().get_total_lines();
        const int parsed = GcodeWorker::instance().get_parsed_lines();
        const bool exhausted = GcodeWorker::instance().is_file_read_exhausted();
        const bool parsing = GcodeWorker::instance().is_parsing();

        int pct = 0;
        if (total > 0) {
            pct = parsed * 100 / total;
            if (pct > 100) {
                pct = 100;
            }
        }
        if (exhausted && !parsing) {
            pct = 100;
        }

        send_print_progress(pct);

        if (pct >= 100 || (exhausted && !parsing)) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    print_running_.store(false);
    send_print_progress(100);
}

void GlavoService::handle_print_terminate(const middleware::Message& msg) {
    (void)msg;
    stop_print_job();
}

} // namespace services
