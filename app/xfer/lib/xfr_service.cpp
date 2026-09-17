#include "xfr_service.hpp"

#include "media_paths.hpp"
#include "msg_port.hpp"
#include "parser_manager.hpp"
#include "print_history_manifest.hpp"
#include "rs_log.hpp"
#include "upc_contract.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace upc::xfr {

namespace {
constexpr auto kTransferProgressEmitMin = std::chrono::milliseconds(250);

std::string body_to_string(const HttpRequest& req)
{
    if (req.body.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(req.body.data()), req.body.size());
}

bool is_3mf_filename(const std::string& name)
{
    if(name.size() <= 4) {
        return false;
    }
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".3mf";
}

bool read_inline_pod_bytes(const middleware::MsgPayload& payload,
                           void* out, std::size_t size)
{
    if (!out ||
        payload.type != middleware::MsgPayload::Type::Inline ||
        payload.length < size) {
        return false;
    }
    std::memcpy(out, payload.data.bytes.data(), size);
    return true;
}

} // namespace

XfrService::XfrService(const XfrConfig& cfg)
    : cfg_(cfg),
      pair_mgr_(cfg.token_file, cfg.pair_pin_ttl_s, cfg.pair_pin_max_attempts),
      upload_mgr_(cfg.storage_dir, cfg.chunk_max_bytes),
      transfer_3mf_importer_(cfg.gcode_dir),
      mdns_(cfg.mdns_service_file)
{
}

bool XfrService::init()
{
    if (!upload_mgr_.ensure_dirs()) {
        UPC_LOG_XFR_SERVICE(Error, "create storage dir failed: %s", cfg_.storage_dir.c_str());
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg_.gcode_dir, ec);
    if (ec) {
        UPC_LOG_XFR_SERVICE(Error, "create gcode dir failed: %s", cfg_.gcode_dir.c_str());
        return false;
    }
    pair_mgr_.load_tokens();
    pair_mgr_.set_pin_notify([this](const std::string& pin, uint32_t expires_s) {
        notify_pair_pin(pin, expires_s);
    });

    subscribe_print_ipc();

    if (!http_.start(cfg_.http_port, [this](const HttpRequest& req) {
        return on_request(req);
    })) {
        UPC_LOG_XFR_SERVICE(Error, "http start failed");
        return false;
    }
    (void)mdns_.publish(cfg_.device_model, cfg_.device_sn, cfg_.http_port, cfg_.require_auth);
    return true;
}

void XfrService::shutdown()
{
    http_.stop();
    mdns_.unpublish();
    subs_.clear();
}

void XfrService::tick()
{
    refresh_slice_progress_if_needed();
    publish_transfer_progress_if_needed();
}

HttpResponse XfrService::on_request(const HttpRequest& req)
{
    const std::string path = request_path_only(req.path);
    if (req.method == "GET" && path == "/api/v1/info") {
        return handle_info();
    }
    if (req.method == "POST" && path == "/api/v1/pair/start") {
        return handle_pair_start(req);
    }
    if (req.method == "POST" && path == "/api/v1/pair/verify") {
        return handle_pair_verify(req);
    }
    if (req.method == "POST" && path == "/api/v1/pair/revoke") {
        return handle_pair_revoke(req);
    }
    if (req.method == "POST" && path == "/api/v1/upload/init") {
        return handle_upload_init(req);
    }
    if (req.method == "POST" && path == "/api/v1/print/start") {
        return handle_print_start(req);
    }
    if (req.method == "GET" && path == "/api/v1/print/status") {
        return handle_print_status(req);
    }
    if (req.method == "GET" && path == "/api/v1/print/history") {
        return handle_print_history(req);
    }

    static const std::regex re_put("^/api/v1/upload/([0-9a-f]+)$");
    static const std::regex re_complete("^/api/v1/upload/([0-9a-f]+)/complete$");
    static const std::regex re_status("^/api/v1/upload/([0-9a-f]+)/status$");
    static const std::regex re_timelapse_list(
        "^/api/v1/print/([^/]+)/timelapse/images$");
    static const std::regex re_timelapse_file("^/timelapse/([^/]+)/([^/]+)$");
    std::smatch m;
    if (req.method == "PUT" && std::regex_match(path, m, re_put) && m.size() == 2) {
        return handle_upload_put(req, m[1].str());
    }
    if (req.method == "POST" && std::regex_match(path, m, re_complete) && m.size() == 2) {
        return handle_upload_complete(req, m[1].str());
    }
    if (req.method == "GET" && std::regex_match(path, m, re_status) && m.size() == 2) {
        return handle_upload_status(req, m[1].str());
    }
    if (req.method == "GET" && std::regex_match(path, m, re_timelapse_list) &&
        m.size() == 2) {
        return handle_timelapse_images(req, m[1].str());
    }
    if (req.method == "GET" && std::regex_match(path, m, re_timelapse_file) &&
        m.size() == 3) {
        return handle_timelapse_file(req, m[1].str(), m[2].str());
    }
    return err(404, "NOT_FOUND", "api not found");
}

HttpResponse XfrService::handle_info() const
{
    std::ostringstream oss;
    oss << "{"
        << "\"sn\":\"" << json_escape(cfg_.device_sn) << "\","
        << "\"model\":\"" << json_escape(cfg_.device_model) << "\","
        << "\"api_version\":1,"
        << "\"need_pair\":" << (cfg_.require_auth ? "true" : "false") << ","
        << "\"state\":\"idle\""
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_pair_start(const HttpRequest& req)
{
    const std::string body = body_to_string(req);
    std::string client_name;
    (void)json_get_string(body, "client_name", &client_name);
    const PairStartResult r = pair_mgr_.start_pair(client_name);
    if (!r.ok) {
        return err(400, r.err_code, r.err_msg);
    }
    std::ostringstream oss;
    oss << "{"
        << "\"pair_session\":\"" << json_escape(r.pair_session) << "\","
        << "\"expires_in\":" << r.expires_in
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_pair_verify(const HttpRequest& req)
{
    const std::string body = body_to_string(req);
    std::string pair_session;
    std::string pin;
    std::string device_name;
    if (!json_get_string(body, "pair_session", &pair_session) ||
        !json_get_string(body, "pin", &pin)) {
        return err(400, "BAD_REQUEST", "pair_session/pin required");
    }
    (void)json_get_string(body, "device_name", &device_name);
    const PairVerifyResult r =
        pair_mgr_.verify_pair(pair_session, pin, device_name, cfg_.device_model);
    if (!r.ok) {
        return err(r.http_status, r.err_code, r.err_msg);
    }
    std::ostringstream oss;
    oss << "{"
        << "\"token\":\"" << json_escape(r.token) << "\","
        << "\"sn\":\"" << json_escape(cfg_.device_sn) << "\","
        << "\"name\":\"" << json_escape(r.name) << "\""
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_pair_revoke(const HttpRequest& req)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    (void)pair_mgr_.revoke_token(token);
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = "{\"ok\":true}";
    return rsp;
}

HttpResponse XfrService::handle_upload_init(const HttpRequest& req)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    const std::string body = body_to_string(req);
    std::string filename;
    std::string sha256;
    uint64_t size = 0;
    if (!json_get_string(body, "filename", &filename) ||
        !json_get_u64(body, "size", &size)) {
        return err(400, "BAD_REQUEST", "filename/size required");
    }
    (void)json_get_string(body, "sha256", &sha256);
    const UploadInitResult r = upload_mgr_.init(filename, size, sha256);
    if (!r.ok) {
        return err(r.http_status, r.err_code, r.err_msg);
    }
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        upload_size_ = size;
        upload_offset_ = r.offset;
        slice_progress_pct_ = 0;
        print_state_ = "uploading";
        transfer_name_ = filename;
    }
    std::ostringstream oss;
    oss << "{"
        << "\"upload_id\":\"" << json_escape(r.upload_id) << "\","
        << "\"offset\":" << r.offset << ","
        << "\"chunk_max\":" << r.chunk_max
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_upload_put(const HttpRequest& req, const std::string& upload_id)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    const std::string content_range = req.header("content-range");
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t total = 0;
    if (!parse_content_range(content_range, &start, &end, &total)) {
        return err(400, "BAD_REQUEST", "invalid Content-Range");
    }
    const UploadStatusResult r = upload_mgr_.append_chunk(upload_id,
                                                          start,
                                                          end,
                                                          total,
                                                          req.body.data(),
                                                          req.body.size());
    if (!r.ok) {
        return err(r.http_status, r.err_code, r.err_msg);
    }
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        upload_size_ = r.size;
        upload_offset_ = r.offset;
        if (print_state_ != "finished" && print_state_ != "error" && print_state_ != "slicing") {
            print_state_ = "uploading";
        }
    }
    std::ostringstream oss;
    oss << "{"
        << "\"offset\":" << r.offset
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_upload_complete(const HttpRequest& req, const std::string& upload_id)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    const std::string body = body_to_string(req);
    std::string sha256;
    (void)json_get_string(body, "sha256", &sha256);
    const UploadCompleteResult r = upload_mgr_.complete(upload_id, sha256);
    if (!r.ok) {
        return err(r.http_status, r.err_code, r.err_msg);
    }

    const std::string src_path = cfg_.storage_dir + "/" + r.file_name;
    if (is_3mf_filename(r.file_name)) {
        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            upload_size_ = r.size;
            upload_offset_ = r.size;
            slice_progress_pct_ = 0;
            /* 复用既有 Slicing 线协议表示“上传后处理”，避免改变 UI/IPC 契约。 */
            print_state_ = "slicing";
            transfer_name_ = r.file_name;
        }

        const Transfer3mfImportResult imported =
            transfer_3mf_importer_.import_file(src_path, r.file_name);
        if (!imported.ok) {
            UPC_LOG_XFR_SERVICE(Error, "3mf import failed file=%s code=%s msg=%s",
                                src_path.c_str(), imported.err_code.c_str(),
                                imported.err_msg.c_str());
            {
                std::lock_guard<std::mutex> lk(print_state_mu_);
                print_state_ = "error";
            }
            return err(imported.http_status, imported.err_code, imported.err_msg);
        }

        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            upload_size_ = imported.printable_size;
            upload_offset_ = imported.printable_size;
            slice_progress_pct_ = 100;
            print_state_ = "finished";
            transfer_name_ = imported.printable_name;
        }
        notify_file_received(imported.printable_name, imported.printable_size);
        UPC_LOG_XFR_SERVICE(Info,
            "upload complete accepted 3mf=%s gcode=%s size=%llu lines=%u layers=%u",
            r.file_name.c_str(), imported.printable_name.c_str(),
            static_cast<unsigned long long>(imported.printable_size),
            static_cast<unsigned>(imported.line_count),
            static_cast<unsigned>(imported.layer_count));

        std::ostringstream oss;
        oss << "{"
            << "\"file_id\":\"" << json_escape(r.file_id) << "\","
            << "\"size\":" << imported.printable_size << ","
            << "\"verified\":" << (r.verified ? "true" : "false")
            << "}";
        HttpResponse rsp;
        rsp.status = 200;
        rsp.body = oss.str();
        return rsp;
    }

    /* OVF 兼容路径保持不变：snowslice 输出 *_out.ovf，再写 Local manifest。 */
    const std::string output_name = output_ovf_name(r.file_name);
    const std::string dst_path = cfg_.gcode_dir + "/" + output_name;
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        upload_size_ = r.size;
        upload_offset_ = r.size;
        slice_progress_pct_ = 0;
        print_state_ = "slicing";
        transfer_name_ = output_name;
    }

    std::string slice_err;
    if (!run_snowslice_build(src_path, dst_path, &slice_err)) {
        UPC_LOG_XFR_SERVICE(Error, "snowslice failed in=%s out=%s msg=%s",
                            src_path.c_str(), dst_path.c_str(), slice_err.c_str());
        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            print_state_ = "error";
            transfer_name_ = output_name;
        }
        return err(500, "OVF_SLICE_FAILED", slice_err);
    }

    std::string preprocess_err;
    if (!preprocess_ovf_and_update_manifest(dst_path, output_name, &preprocess_err)) {
        UPC_LOG_XFR_SERVICE(Error, "upload complete preprocess failed file=%s msg=%s",
                            dst_path.c_str(), preprocess_err.c_str());
        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            print_state_ = "error";
            transfer_name_ = output_name;
        }
        return err(500, "OVF_PREPROCESS_FAILED", preprocess_err);
    }

    struct stat st{};
    uint64_t file_size = r.size;
    if (stat(dst_path.c_str(), &st) == 0 && st.st_size >= 0) {
        file_size = static_cast<uint64_t>(st.st_size);
    }
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        upload_size_ = file_size;
        upload_offset_ = file_size;
        slice_progress_pct_ = 100;
        print_state_ = "finished";
        transfer_name_ = output_name;
    }
    notify_file_received(output_name, file_size);
    UPC_LOG_XFR_SERVICE(Info, "upload complete accepted in=%s out=%s size=%llu",
                        r.file_name.c_str(), output_name.c_str(),
                        static_cast<unsigned long long>(file_size));
    std::ostringstream oss;
    oss << "{"
        << "\"file_id\":\"" << json_escape(r.file_id) << "\","
        << "\"size\":" << file_size << ","
        << "\"verified\":" << (r.verified ? "true" : "false")
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_print_start(const HttpRequest& req)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }

    const std::string body = body_to_string(req);
    uint64_t plate_id_u64 = 1;
    (void)json_get_u64(body, "plate_id", &plate_id_u64);
    const uint32_t plate_id = static_cast<uint32_t>(plate_id_u64);

    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::string job_id;
    std::string state;
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        if (print_job_id_.empty()) {
            print_job_id_ = "j_" + std::to_string(now_ms);
        }
        if (print_state_ == "idle") {
            print_state_ = "uploading";
        }
        job_id = print_job_id_;
        state = print_state_;
    }

    std::ostringstream oss;
    oss << "{"
        << "\"job_id\":\"" << json_escape(job_id) << "\","
        << "\"state\":\"" << json_escape(state) << "\","
        << "\"plate_id\":" << plate_id
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

uint32_t XfrService::calc_progress_percent(const std::string& state,
                                           uint64_t upload_size,
                                           uint64_t upload_offset,
                                           uint32_t slice_progress_pct) const
{
    if (state == "finished") {
        return 100U;
    }

    const double upload_ratio = (upload_size == 0)
        ? 0.0
        : static_cast<double>(upload_offset) / static_cast<double>(upload_size);
    const double bounded_upload_ratio = std::min(1.0, std::max(0.0, upload_ratio));
    const double bounded_slice_ratio =
        std::min(1.0, std::max(0.0, static_cast<double>(slice_progress_pct) / 100.0));

    if (state == "uploading") {
        const int pct = static_cast<int>(0.05 * bounded_upload_ratio * 100.0 + 0.5);
        return static_cast<uint32_t>(std::min(100, std::max(0, pct)));
    }
    if (state == "slicing" || state == "printing" || state == "error") {
        const int pct = static_cast<int>((0.05 + 0.95 * bounded_slice_ratio) * 100.0 + 0.5);
        return static_cast<uint32_t>(std::min(100, std::max(0, pct)));
    }
    return 0U;
}

void XfrService::publish_transfer_progress_if_needed()
{
    const auto now = std::chrono::steady_clock::now();
    if (last_progress_emit_at_.time_since_epoch().count() != 0 &&
        now - last_progress_emit_at_ < kTransferProgressEmitMin) {
        return;
    }

    std::string state;
    uint64_t upload_size = 0;
    uint64_t upload_offset = 0;
    uint32_t slice_progress_pct = 0;
    std::string transfer_name;
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        state = print_state_;
        upload_size = upload_size_;
        upload_offset = upload_offset_;
        slice_progress_pct = slice_progress_pct_;
        transfer_name = transfer_name_;
    }

    const uint32_t progress_percent = calc_progress_percent(
        state, upload_size, upload_offset, slice_progress_pct);

    uint8_t state_code = static_cast<uint8_t>(middleware::XfrTransferState::Idle);
    if (state == "uploading") {
        state_code = static_cast<uint8_t>(middleware::XfrTransferState::Uploading);
    } else if (state == "slicing" || state == "printing") {
        state_code = static_cast<uint8_t>(middleware::XfrTransferState::Slicing);
    } else if (state == "finished") {
        state_code = static_cast<uint8_t>(middleware::XfrTransferState::Finished);
    } else if (state == "error") {
        state_code = static_cast<uint8_t>(middleware::XfrTransferState::Error);
    }

    if (state_code == last_progress_state_ &&
        progress_percent == last_progress_percent_ &&
        upload_offset == last_progress_uploaded_ &&
        upload_size == last_progress_total_ &&
        transfer_name == last_progress_name_) {
        return;
    }

    middleware::XfrTransferProgressPayload payload{};
    std::strncpy(payload.file_name, transfer_name.c_str(), sizeof(payload.file_name) - 1);
    payload.state = state_code;
    payload.progress_percent = progress_percent;
    payload.uploaded_bytes = upload_offset;
    payload.total_bytes = upload_size;

    middleware::Message msg(middleware::MsgType::XFR_Transfer_Progress_Ntf,
                            middleware::ModuleId::APP_XFR,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    (void)middleware::MsgPort::instance().publish_async(msg);

    last_progress_emit_at_ = now;
    last_progress_state_ = state_code;
    last_progress_percent_ = progress_percent;
    last_progress_uploaded_ = upload_offset;
    last_progress_total_ = upload_size;
    last_progress_name_ = transfer_name;
}

HttpResponse XfrService::handle_print_status(const HttpRequest& req)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }

    refresh_slice_progress_if_needed();

    std::string job_id;
    std::string transfer_state;
    uint32_t layer = 0;
    uint32_t layer_total = 0;
    uint64_t upload_size = 0;
    uint64_t upload_offset = 0;
    uint32_t slice_progress_pct = 0;
    GlavoPhase glavo_phase = GlavoPhase::Idle;
    int32_t glavo_pct = 0;
    uint32_t glavo_layer = 0;
    uint32_t glavo_total = 0;
    uint32_t remaining_minutes = 0;
    float chamber_c = 0.f;
    uint64_t since_ms = 0;
    std::string glavo_error;
    std::string frames_dir;
    bool session_active = false;
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        if (print_job_id_.empty()) {
            const uint64_t now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            print_job_id_ = "j_" + std::to_string(now_ms);
        }
        job_id = print_job_id_;
        transfer_state = print_state_;
        layer = print_layer_;
        layer_total = print_layer_total_;
        upload_size = upload_size_;
        upload_offset = upload_offset_;
        slice_progress_pct = slice_progress_pct_;
        glavo_phase = glavo_phase_;
        glavo_pct = glavo_progress_percent_;
        glavo_layer = glavo_current_layer_;
        glavo_total = glavo_total_layers_;
        remaining_minutes = glavo_remaining_minutes_;
        chamber_c = glavo_chamber_temp_c_;
        since_ms = glavo_since_ms_;
        glavo_error = glavo_error_;
        frames_dir = active_frames_dir_;
        session_active = print_session_active_;
    }
    if (upload_size > 0 && upload_offset > upload_size) {
        upload_offset = upload_size;
    }

    /* 合成对外 state：Glavo 打印态优先；传输期 finished 对外映射 idle（打印完成才用 finished）。 */
    std::string state = "idle";
    double progress = 0.0;
    uint64_t elapsed_s = 0;
    uint64_t remaining_s = 0;
    const char* error_json = "null";
    std::string error_escaped;

    const bool glavo_active =
        glavo_phase == GlavoPhase::Printing ||
        glavo_phase == GlavoPhase::Paused ||
        glavo_phase == GlavoPhase::Finished ||
        glavo_phase == GlavoPhase::Canceled ||
        glavo_phase == GlavoPhase::Error;

    if (glavo_active) {
        switch (glavo_phase) {
            case GlavoPhase::Printing: state = "printing"; break;
            case GlavoPhase::Paused:   state = "paused"; break;
            case GlavoPhase::Finished: state = "finished"; break;
            case GlavoPhase::Canceled: state = "canceled"; break;
            case GlavoPhase::Error:    state = "error"; break;
            default: break;
        }
        progress = std::min(1.0, std::max(0.0, static_cast<double>(glavo_pct) / 100.0));
        layer = glavo_layer;
        layer_total = glavo_total;
        remaining_s = static_cast<uint64_t>(remaining_minutes) * 60ULL;
        if (since_ms > 0 &&
            (glavo_phase == GlavoPhase::Printing || glavo_phase == GlavoPhase::Paused)) {
            const uint64_t now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            if (now_ms >= since_ms) {
                elapsed_s = (now_ms - since_ms) / 1000ULL;
            }
        }
        if (!frames_dir.empty()) {
            job_id = frames_dir;
        } else if (session_active) {
            job_id = UPC_VIDEO_TMP_NAME;
        }
        if (glavo_phase == GlavoPhase::Error && !glavo_error.empty()) {
            error_escaped = json_escape(glavo_error);
            error_json = nullptr;
        }
    } else if (transfer_state == "uploading") {
        state = "uploading";
        progress = static_cast<double>(calc_progress_percent(
            transfer_state, upload_size, upload_offset, slice_progress_pct)) / 100.0;
    } else if (transfer_state == "slicing") {
        /* 对外仍映射 printing，兼容既有桌面端对切片阶段的轮询 */
        state = "printing";
        progress = static_cast<double>(calc_progress_percent(
            transfer_state, upload_size, upload_offset, slice_progress_pct)) / 100.0;
    } else if (transfer_state == "error") {
        state = "error";
        progress = static_cast<double>(calc_progress_percent(
            transfer_state, upload_size, upload_offset, slice_progress_pct)) / 100.0;
        error_escaped = "transfer or slice failed";
        error_json = nullptr;
    } else {
        /* idle / finished(传输完成) → idle */
        state = "idle";
        progress = 0.0;
        layer = 0;
        layer_total = 0;
    }

    const int chamber_temp = static_cast<int>(chamber_c * 10.0f + (chamber_c >= 0.f ? 0.5f : -0.5f));

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{"
        << "\"job_id\":\"" << json_escape(job_id) << "\","
        << "\"state\":\"" << json_escape(state) << "\","
        << "\"progress\":" << progress << ","
        << "\"layer\":" << layer << ","
        << "\"layer_total\":" << layer_total << ","
        << "\"layer_height\":0,"
        << "\"elapsed_s\":" << elapsed_s << ","
        << "\"remaining_s\":" << remaining_s << ","
        << "\"filament_total_g\":0,"
        << "\"filament_used_g\":0,"
        << "\"nozzle_temp\":0,"
        << "\"bed_temp\":0,"
        << "\"chamber_temp\":" << chamber_temp << ","
        << "\"laser_power\":0,"
        << "\"blower_speed\":0,"
        << "\"blower_state\":0,"
        << "\"blower_error\":0,"
        << "\"error\":";
    if (error_json) {
        oss << "null";
    } else {
        oss << "\"" << error_escaped << "\"";
    }
    oss << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

bool XfrService::preprocess_ovf_and_update_manifest(const std::string& ovf_path,
                                                    const std::string& ovf_name,
                                                    std::string* err_msg) const
{
    struct stat st{};
    if (stat(ovf_path.c_str(), &st) != 0) {
        UPC_LOG_XFR_SERVICE(Error, "manifest stat ovf failed path=%s", ovf_path.c_str());
        if (err_msg) { *err_msg = "stat ovf failed"; }
        return false;
    }

    parser::ParserManager& parser_mgr = parser::ParserManager::instance();
    if (parser_mgr.open_file(ovf_path) != 0) {
        UPC_LOG_XFR_SERVICE(Error, "ovf preprocess open failed path=%s", ovf_path.c_str());
        if (err_msg) { *err_msg = "open ovf failed"; }
        return false;
    }
    parser::IParserWorker* worker = parser_mgr.active();
    if (!worker) {
        UPC_LOG_XFR_SERVICE(Error, "ovf preprocess resolve worker failed path=%s", ovf_path.c_str());
        if (err_msg) { *err_msg = "ovf worker unavailable"; }
        return false;
    }
    const uint32_t line_count = static_cast<uint32_t>(std::max(0, worker->get_total_lines()));
    worker->close_file();

    std::vector<GcodeManifestEntry> entries;
    if (!gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, entries)) {
        UPC_LOG_XFR_SERVICE(Error, "manifest load failed path=%s", UPC_LOCAL_GCODE_CONFIG);
        if (err_msg) { *err_msg = "load gcode manifest failed"; }
        return false;
    }

    GcodeManifestEntry entry;
    entry.name = ovf_name;
    entry.file_size = static_cast<uint32_t>(st.st_size);
    entry.line_count = line_count;
    entry.duration_sec = 0;
    entry.material_g = 0.f;
    entry.mtime = static_cast<int64_t>(st.st_mtime);
    gcode_manifest_upsert(entries, entry);
    if (!gcode_manifest_save(UPC_LOCAL_GCODE_CONFIG, entries)) {
        UPC_LOG_XFR_SERVICE(Error, "manifest save failed path=%s", UPC_LOCAL_GCODE_CONFIG);
        if (err_msg) { *err_msg = "save gcode manifest failed"; }
        return false;
    }
    UPC_LOG_XFR_SERVICE(Info, "manifest updated ovf=%s size=%u segments=%u mtime=%lld",
                        ovf_name.c_str(), entry.file_size, entry.line_count,
                        static_cast<long long>(entry.mtime));
    return true;
}

bool XfrService::read_progress_percent_file(const std::string& path, uint32_t* out_pct)
{
    if (!out_pct) {
        return false;
    }
    std::ifstream in(path);
    if (!in.good()) {
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    std::size_t b = 0;
    std::size_t e = line.size();
    while (b < e && std::isspace(static_cast<unsigned char>(line[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1])) != 0) {
        --e;
    }
    if (b >= e) {
        return false;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(line.substr(b, e - b).c_str(), &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    *out_pct = static_cast<uint32_t>(std::min<unsigned long>(100UL, v));
    return true;
}

std::string XfrService::output_ovf_name(const std::string& input_name)
{
    const std::size_t dot = input_name.rfind('.');
    if (dot == std::string::npos) {
        return input_name + "_out.ovf";
    }
    return input_name.substr(0, dot) + "_out" + input_name.substr(dot);
}

bool XfrService::run_snowslice_build(const std::string& input_ovf_path,
                                     const std::string& output_ovf_path,
                                     std::string* err_msg) const
{
    {
        std::ofstream reset(cfg_.progress_file, std::ios::trunc);
        if (reset.good()) {
            reset << "0\n";
        }
    }

    UPC_LOG_XFR_SERVICE(Info, "run snowslice in=%s out=%s progress=%s",
                        input_ovf_path.c_str(),
                        output_ovf_path.c_str(),
                        cfg_.progress_file.c_str());
    pid_t pid = fork();
    if (pid < 0) {
        if (err_msg) {
            *err_msg = "fork snowslice failed";
        }
        return false;
    }
    if (pid == 0) {
        execl(cfg_.snowslice_bin.c_str(),
              cfg_.snowslice_bin.c_str(),
              input_ovf_path.c_str(),
              "--out",
              output_ovf_path.c_str(),
              "--outPro",
              cfg_.progress_file.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        if (err_msg) {
            *err_msg = "waitpid snowslice failed";
        }
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ostringstream oss;
        oss << "snowslice exit abnormal";
        if (WIFEXITED(status)) {
            oss << " code=" << WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            oss << " signal=" << WTERMSIG(status);
        }
        if (err_msg) {
            *err_msg = oss.str();
        }
        return false;
    }

    struct stat st{};
    if (stat(output_ovf_path.c_str(), &st) != 0 || st.st_size <= 0) {
        if (err_msg) {
            *err_msg = "snowslice output missing or empty";
        }
        return false;
    }
    return true;
}

void XfrService::refresh_slice_progress_if_needed()
{
    std::lock_guard<std::mutex> lk(print_state_mu_);
    if (print_state_ != "slicing" && print_state_ != "printing") {
        return;
    }
    uint32_t latest_pct = 0;
    if (!read_progress_percent_file(cfg_.progress_file, &latest_pct)) {
        return;
    }
    if (latest_pct > slice_progress_pct_) {
        slice_progress_pct_ = latest_pct;
    }
}

HttpResponse XfrService::handle_upload_status(const HttpRequest& req, const std::string& upload_id)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    const UploadStatusResult r = upload_mgr_.status(upload_id);
    if (!r.ok) {
        return err(r.http_status, r.err_code, r.err_msg);
    }
    std::ostringstream oss;
    oss << "{"
        << "\"offset\":" << r.offset << ","
        << "\"size\":" << r.size << ","
        << "\"state\":\"" << json_escape(r.state) << "\""
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

bool XfrService::need_auth(const HttpRequest& req, std::string* token_out) const
{
    if (!cfg_.require_auth) {
        if (token_out) {
            token_out->clear();
        }
        return true;
    }
    const std::string auth = req.header("authorization");
    static const std::string kPrefix = "Bearer ";
    if (auth.size() <= kPrefix.size() ||
        auth.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    const std::string token = auth.substr(kPrefix.size());
    if (!pair_mgr_.validate_token(token)) {
        return false;
    }
    if (token_out) {
        *token_out = token;
    }
    return true;
}

bool XfrService::json_get_string(const std::string& body, const std::string& key, std::string* out)
{
    if (!out) {
        return false;
    }
    const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(body, m, re) || m.size() < 2) {
        return false;
    }
    *out = m[1].str();
    return true;
}

bool XfrService::json_get_u64(const std::string& body, const std::string& key, uint64_t* out)
{
    if (!out) {
        return false;
    }
    const std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch m;
    if (!std::regex_search(body, m, re) || m.size() < 2) {
        return false;
    }
    *out = static_cast<uint64_t>(std::strtoull(m[1].str().c_str(), nullptr, 10));
    return true;
}

bool XfrService::parse_content_range(const std::string& v,
                                     uint64_t* start,
                                     uint64_t* end,
                                     uint64_t* total)
{
    if (!start || !end || !total) {
        return false;
    }
    std::smatch m;
    static const std::regex re("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$");
    if (!std::regex_match(v, m, re) || m.size() != 4) {
        return false;
    }
    *start = static_cast<uint64_t>(std::strtoull(m[1].str().c_str(), nullptr, 10));
    *end = static_cast<uint64_t>(std::strtoull(m[2].str().c_str(), nullptr, 10));
    *total = static_cast<uint64_t>(std::strtoull(m[3].str().c_str(), nullptr, 10));
    return true;
}

std::string XfrService::json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

HttpResponse XfrService::err(int status, const std::string& code, const std::string& message)
{
    HttpResponse rsp;
    rsp.status = status;
    std::ostringstream oss;
    oss << "{"
        << "\"code\":\"" << json_escape(code) << "\","
        << "\"message\":\"" << json_escape(message) << "\""
        << "}";
    rsp.body = oss.str();
    return rsp;
}

void XfrService::notify_pair_pin(const std::string& pin, uint32_t expires_s) const
{
    middleware::XfrPairPinPayload payload{};
    std::strncpy(payload.pin, pin.c_str(), sizeof(payload.pin) - 1);
    payload.expires_s = expires_s;

    middleware::Message msg(middleware::MsgType::XFR_Pair_Pin_Ntf,
                            middleware::ModuleId::APP_XFR,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    middleware::MsgPort::instance().publish_async(msg);
}

void XfrService::notify_file_received(const std::string& name, uint64_t size) const
{
    middleware::XfrFileReceivedPayload payload{};
    std::strncpy(payload.file_name, name.c_str(), sizeof(payload.file_name) - 1);
    payload.file_size = size;

    middleware::Message msg(middleware::MsgType::XFR_File_Received_Ntf,
                            middleware::ModuleId::APP_XFR,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    middleware::MsgPort::instance().publish_async(msg);
}

void XfrService::subscribe_print_ipc()
{
    auto& port = middleware::MsgPort::instance();
    auto add = [&](middleware::MsgType type) {
        subs_.push_back(port.subscribe(
            type,
            middleware::ModuleId::APP_XFR,
            [this](const middleware::Message& msg) { on_print_ipc(msg); }));
    };
    add(middleware::MsgType::APP_Print_Progress_Ntf);
    add(middleware::MsgType::APP_Print_Pause_Rsp);
    add(middleware::MsgType::GLAVO_Print_State_Ntf);
    add(middleware::MsgType::APP_Print_Terminate_Rsp);
    add(middleware::MsgType::APP_Print_Start_Rejected_Ntf);
    add(middleware::MsgType::APP_Print_Video_Manifest_Ntf);
}

void XfrService::on_print_ipc(const middleware::Message& msg)
{
    std::lock_guard<std::mutex> lk(print_state_mu_);
    switch (msg.type) {
        case middleware::MsgType::APP_Print_Progress_Ntf: {
            middleware::PrintProgressPayload p{};
            if (!read_inline_pod_bytes(msg.payload, &p, sizeof(p))) {
                return;
            }
            glavo_progress_percent_ = p.progress_percent;
            glavo_remaining_minutes_ = p.remaining_minutes;
            glavo_chamber_temp_c_ = p.temperature_c;
            glavo_current_layer_ = p.current_layer;
            glavo_total_layers_ = p.total_layers;
            /* 迟到订阅 / 仅收到 progress：在非终态下可进入 printing；
             * 不覆盖 canceled/error，避免终止后的迟到包把状态拉回。 */
            if (glavo_phase_ != GlavoPhase::Canceled &&
                glavo_phase_ != GlavoPhase::Error &&
                glavo_phase_ != GlavoPhase::Paused) {
                if ((glavo_phase_ == GlavoPhase::Idle ||
                     glavo_phase_ == GlavoPhase::Finished) &&
                    p.progress_percent > 0 && p.progress_percent < 100) {
                    glavo_phase_ = GlavoPhase::Printing;
                    print_session_active_ = true;
                    if (active_frames_dir_.empty()) {
                        active_frames_dir_ = UPC_VIDEO_TMP_NAME;
                    }
                }
            }
            if (glavo_phase_ == GlavoPhase::Printing ||
                glavo_phase_ == GlavoPhase::Paused) {
                print_layer_ = p.current_layer;
                print_layer_total_ = p.total_layers;
            }
            break;
        }
        case middleware::MsgType::APP_Print_Pause_Rsp: {
            middleware::PrintPauseStatePayload p{};
            if (!read_inline_pod_bytes(msg.payload, &p, sizeof(p))) {
                return;
            }
            if (p.paused != 0) {
                glavo_phase_ = GlavoPhase::Paused;
            } else if (glavo_phase_ == GlavoPhase::Paused ||
                       print_session_active_) {
                glavo_phase_ = GlavoPhase::Printing;
            }
            print_session_active_ = true;
            break;
        }
        case middleware::MsgType::GLAVO_Print_State_Ntf: {
            middleware::PrintStateNtfPayload p{};
            if (!read_inline_pod_bytes(msg.payload, &p, sizeof(p))) {
                return;
            }
            glavo_since_ms_ = p.since_ms;
            if (p.mode == static_cast<uint32_t>(middleware::UserHomeStatus::Printing)) {
                glavo_phase_ = GlavoPhase::Printing;
                print_session_active_ = true;
                glavo_error_.clear();
                if (active_frames_dir_.empty()) {
                    active_frames_dir_ = UPC_VIDEO_TMP_NAME;
                }
            } else if (p.mode == static_cast<uint32_t>(middleware::UserHomeStatus::Idle)) {
                if (glavo_phase_ == GlavoPhase::Canceled ||
                    glavo_phase_ == GlavoPhase::Error) {
                    print_session_active_ = false;
                    break;
                }
                if (print_session_active_ ||
                    glavo_phase_ == GlavoPhase::Printing ||
                    glavo_phase_ == GlavoPhase::Paused) {
                    glavo_phase_ = GlavoPhase::Finished;
                    glavo_progress_percent_ = 100;
                }
                print_session_active_ = false;
            }
            break;
        }
        case middleware::MsgType::APP_Print_Terminate_Rsp: {
            glavo_phase_ = GlavoPhase::Canceled;
            print_session_active_ = false;
            glavo_error_.clear();
            break;
        }
        case middleware::MsgType::APP_Print_Start_Rejected_Ntf: {
            glavo_phase_ = GlavoPhase::Error;
            print_session_active_ = false;
            glavo_error_ = "print start rejected";
            break;
        }
        case middleware::MsgType::APP_Print_Video_Manifest_Ntf: {
            middleware::PrintVideoManifestPayload p{};
            if (!read_inline_pod_bytes(msg.payload, &p, sizeof(p))) {
                return;
            }
            if (p.has_entry != 0 && p.frames_dir[0] != '\0') {
                active_frames_dir_ = p.frames_dir;
                print_job_id_ = active_frames_dir_;
            }
            if (p.aborted != 0) {
                glavo_phase_ = GlavoPhase::Canceled;
                print_session_active_ = false;
            } else if (p.has_entry != 0) {
                if (glavo_phase_ == GlavoPhase::Printing ||
                    glavo_phase_ == GlavoPhase::Paused ||
                    print_session_active_) {
                    glavo_phase_ = GlavoPhase::Finished;
                    glavo_progress_percent_ = 100;
                }
                print_session_active_ = false;
            }
            break;
        }
        default:
            break;
    }
}

HttpResponse XfrService::handle_print_history(const HttpRequest& req)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }

    const std::string query = request_query(req.path);
    int year = 0;
    int month = 0;
    int day = 0;
    if (!query_get_int(query, "year", &year) ||
        !query_get_int(query, "month", &month) ||
        !query_get_int(query, "day", &day) ||
        !calendar_date_valid(year, month, day)) {
        return err(422, "INVALID_DATE", "invalid year/month/day");
    }

    std::vector<upc::PrintHistoryEntry> entries;
    if (!upc::print_history_manifest_load(UPC_VIDEO_CONFIG, entries)) {
        return err(500, "INTERNAL", "load print history failed");
    }

    std::vector<std::pair<int64_t, std::string>> hits;
    hits.reserve(entries.size());
    for (const auto& e : entries) {
        if (!e.committed) {
            continue;
        }
        if (e.frames_dir.empty()) {
            continue;
        }
        if (!entry_on_local_date(e.start_unix, year, month, day)) {
            continue;
        }
        hits.emplace_back(e.start_unix, e.frames_dir);
    }
    std::sort(hits.begin(), hits.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    char date_buf[16];
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", year, month, day);

    std::ostringstream oss;
    oss << "{"
        << "\"date\":\"" << date_buf << "\","
        << "\"count\":" << hits.size() << ","
        << "\"job_ids\":[";
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << "\"" << json_escape(hits[i].second) << "\"";
    }
    oss << "]}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_timelapse_images(const HttpRequest& req,
                                                const std::string& job_id)
{
    std::string token;
    if (!need_auth(req, &token)) {
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    if (!is_safe_job_id(job_id)) {
        return err(404, "JOB_NOT_FOUND", "job_id not found");
    }

    int page = 1;
    int page_size = 50;
    const std::string query = request_query(req.path);
    (void)query_get_int(query, "page", &page);
    (void)query_get_int(query, "page_size", &page_size);
    if (page < 1) {
        page = 1;
    }
    if (page_size < 1) {
        page_size = 50;
    }
    if (page_size > 500) {
        page_size = 500;
    }

    bool known_job = false;
    bool capturing = false;
    {
        std::vector<upc::PrintHistoryEntry> entries;
        (void)upc::print_history_manifest_load(UPC_VIDEO_CONFIG, entries);
        for (const auto& e : entries) {
            if (e.frames_dir == job_id) {
                known_job = true;
                break;
            }
        }
        std::lock_guard<std::mutex> lk(print_state_mu_);
        if (!known_job) {
            if (job_id == active_frames_dir_ ||
                (job_id == UPC_VIDEO_TMP_NAME && print_session_active_)) {
                known_job = true;
            }
        }
        capturing = print_session_active_ &&
                    (job_id == active_frames_dir_ ||
                     (active_frames_dir_.empty() && job_id == UPC_VIDEO_TMP_NAME) ||
                     ((glavo_phase_ == GlavoPhase::Printing ||
                       glavo_phase_ == GlavoPhase::Paused) &&
                      job_id == UPC_VIDEO_TMP_NAME));
    }
    if (!known_job) {
        /* 目录已存在也视为有效（清单尚未写入的捕获中会话） */
        const std::string job_dir = std::string(UPC_VIDEO_DIR) + "/" + job_id;
        struct stat st{};
        if (stat(job_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            known_job = true;
        }
    }
    if (!known_job) {
        return err(404, "JOB_NOT_FOUND", "job_id not found");
    }

    const std::string job_dir = std::string(UPC_VIDEO_DIR) + "/" + job_id;
    const std::vector<uint32_t> indices = list_timelapse_png_indices(job_dir);
    const int total = static_cast<int>(indices.size());
    std::string state = "ready";
    if (total == 0) {
        state = "none";
    } else if (capturing) {
        state = "capturing";
    }

    const std::string base = public_base_url(req, cfg_.http_port);
    const int start = (page - 1) * page_size;
    std::ostringstream oss;
    oss << "{"
        << "\"job_id\":\"" << json_escape(job_id) << "\","
        << "\"state\":\"" << state << "\","
        << "\"total\":" << total << ","
        << "\"page\":" << page << ","
        << "\"page_size\":" << page_size << ","
        << "\"images\":[";
    bool first = true;
    if (start < total) {
        const int end = std::min(total, start + page_size);
        for (int i = start; i < end; ++i) {
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "\"" << json_escape(
                base + "/timelapse/" + job_id + "/" +
                std::to_string(indices[static_cast<std::size_t>(i)]) + ".png")
                << "\"";
        }
    }
    oss << "]}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_timelapse_file(const HttpRequest& req,
                                               const std::string& job_id,
                                               const std::string& file_name)
{
    (void)req;
    if (!is_safe_job_id(job_id)) {
        return err(404, "NOT_FOUND", "file not found");
    }
    uint32_t idx = 0;
    if (!is_png_frame_name(file_name, &idx)) {
        return err(404, "NOT_FOUND", "file not found");
    }

    const std::string path =
        std::string(UPC_VIDEO_DIR) + "/" + job_id + "/" + std::to_string(idx) + ".png";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return err(404, "NOT_FOUND", "file not found");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    HttpResponse rsp;
    rsp.status = 200;
    rsp.content_type = "image/png";
    rsp.body = ss.str();
    return rsp;
}

std::string XfrService::request_path_only(const std::string& raw_path)
{
    const std::size_t q = raw_path.find('?');
    return (q == std::string::npos) ? raw_path : raw_path.substr(0, q);
}

std::string XfrService::request_query(const std::string& raw_path)
{
    const std::size_t q = raw_path.find('?');
    if (q == std::string::npos || q + 1 >= raw_path.size()) {
        return {};
    }
    return raw_path.substr(q + 1);
}

bool XfrService::query_get_int(const std::string& query, const std::string& key, int* out)
{
    if (!out || key.empty()) {
        return false;
    }
    const std::string prefix = key + "=";
    std::size_t start = 0;
    while (start < query.size()) {
        std::size_t amp = query.find('&', start);
        if (amp == std::string::npos) {
            amp = query.size();
        }
        const std::string part = query.substr(start, amp - start);
        if (part.compare(0, prefix.size(), prefix) == 0) {
            const std::string val = part.substr(prefix.size());
            if (val.empty()) {
                return false;
            }
            char* end = nullptr;
            const long v = std::strtol(val.c_str(), &end, 10);
            if (!end || *end != '\0') {
                return false;
            }
            *out = static_cast<int>(v);
            return true;
        }
        start = amp + 1;
    }
    return false;
}

bool XfrService::is_safe_job_id(const std::string& job_id)
{
    if (job_id.empty() || job_id.size() > 96) {
        return false;
    }
    if (job_id == "." || job_id == "..") {
        return false;
    }
    for (unsigned char c : job_id) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool XfrService::is_png_frame_name(const std::string& name, uint32_t* index_out)
{
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0) {
        return false;
    }
    const std::string num = name.substr(0, name.size() - 4);
    if (num.empty()) {
        return false;
    }
    for (unsigned char c : num) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(num.c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0 || v > 0xffffffffUL) {
        return false;
    }
    if (index_out) {
        *index_out = static_cast<uint32_t>(v);
    }
    return true;
}

bool XfrService::calendar_date_valid(int year, int month, int day)
{
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) {
        return false;
    }
    return tm.tm_year == year - 1900 && tm.tm_mon == month - 1 && tm.tm_mday == day;
}

bool XfrService::entry_on_local_date(int64_t start_unix, int year, int month, int day)
{
    if (start_unix <= 0) {
        return false;
    }
    const std::time_t t = static_cast<std::time_t>(start_unix);
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) {
        return false;
    }
#else
    if (!localtime_r(&t, &tm)) {
        return false;
    }
#endif
    return (tm.tm_year + 1900) == year && (tm.tm_mon + 1) == month && tm.tm_mday == day;
}

std::vector<uint32_t> XfrService::list_timelapse_png_indices(const std::string& job_dir)
{
    std::vector<uint32_t> out;
    DIR* dir = ::opendir(job_dir.c_str());
    if (!dir) {
        return out;
    }
    while (struct dirent* ent = ::readdir(dir)) {
        if (!ent->d_name) {
            continue;
        }
        uint32_t idx = 0;
        if (!is_png_frame_name(ent->d_name, &idx)) {
            continue;
        }
        out.push_back(idx);
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string XfrService::public_base_url(const HttpRequest& req, uint16_t http_port)
{
    const std::string host = req.header("host");
    if (!host.empty()) {
        return "http://" + host;
    }
    return "http://127.0.0.1:" + std::to_string(http_port);
}

} // namespace upc::xfr
