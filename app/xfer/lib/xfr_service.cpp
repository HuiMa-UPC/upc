#include "xfr_service.hpp"

#include "msg_port.hpp"
#include "rs_log.hpp"
#include "upc_contract.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <exception>
#include <iomanip>
#include <regex>
#include <sstream>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
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

std::string lower_ext(const std::filesystem::path& p)
{
    std::string s = p.extension().string();
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool read_progress_percent_file(const std::string& path, uint32_t* out_pct)
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
    const std::string num = line.substr(b, e - b);
    char* end = nullptr;
    const unsigned long v = std::strtoul(num.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    *out_pct = static_cast<uint32_t>(std::min<unsigned long>(100UL, v));
    return true;
}

} // namespace

XfrService::XfrService(const XfrConfig& cfg)
    : cfg_(cfg),
      pair_mgr_(cfg.token_file, cfg.pair_pin_ttl_s, cfg.pair_pin_max_attempts),
      upload_mgr_(cfg.storage_dir, cfg.chunk_max_bytes),
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
}

void XfrService::tick()
{
    publish_transfer_progress_if_needed();
}

HttpResponse XfrService::on_request(const HttpRequest& req)
{
    const std::string path = req.path;
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
    if (req.method == "POST" && path == "/api/v1/build/start") {
        return handle_build_start(req);
    }
    if (req.method == "GET" && path == "/api/v1/build/status") {
        return handle_build_status(req);
    }

    static const std::regex re_put("^/api/v1/upload/([0-9a-f]+)$");
    static const std::regex re_complete("^/api/v1/upload/([0-9a-f]+)/complete$");
    static const std::regex re_status("^/api/v1/upload/([0-9a-f]+)/status$");
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
        if (print_state_ != "finished" && print_state_ != "error") {
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
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        upload_size_ = r.size;
        upload_offset_ = r.size;
        slice_progress_pct_ = 0;
        if (print_state_ != "finished" && print_state_ != "error") {
            print_state_ = "printing";
        }
        transfer_name_ = r.file_name;
    }
    const bool started = trigger_auto_build_async();
    UPC_LOG_XFR_SERVICE(Info, "upload complete accepted file=%s auto_build_started=%d",
                        r.file_name.c_str(), started ? 1 : 0);
    std::ostringstream oss;
    oss << "{"
        << "\"file_id\":\"" << json_escape(r.file_id) << "\","
        << "\"size\":" << r.size << ","
        << "\"verified\":" << (r.verified ? "true" : "false") << ","
        << "\"auto_build_started\":" << (started ? "true" : "false")
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

HttpResponse XfrService::handle_build_start(const HttpRequest& req)
{
    UPC_LOG_XFR_SERVICE(Info, "build/start requested path=%s body_bytes=%zu",
                        req.path.c_str(), req.body.size());
    std::string token;
    if (!need_auth(req, &token)) {
        UPC_LOG_XFR_SERVICE(Warn, "build/start auth failed");
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    if (!trigger_auto_build_async()) {
        UPC_LOG_XFR_SERVICE(Warn, "build/start rejected: build already running");
        return err(409, "BUILD_BUSY", "build already running");
    }

    std::ostringstream oss;
    oss << "{"
        << "\"ok\":true,"
        << "\"state\":\"running\""
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

void XfrService::refresh_slice_progress_locked(std::string& state, uint32_t& slice_pct)
{
    if (state != "printing") {
        return;
    }
    uint32_t latest_pct = 0;
    if (!read_progress_percent_file(cfg_.progress_file, &latest_pct)) {
        return;
    }
    if (latest_pct > slice_progress_pct_) {
        slice_progress_pct_ = latest_pct;
    }
    slice_pct = slice_progress_pct_;
    if (slice_progress_pct_ >= 100U) {
        print_state_ = "finished";
        state = "finished";
        return;
    }
    state = print_state_;
}

uint32_t XfrService::calc_progress_percent(const std::string& state,
                                           uint64_t upload_size,
                                           uint64_t upload_offset,
                                           uint32_t slice_pct) const
{
    if (state == "finished") {
        return 100U;
    }
    const double upload_ratio = (upload_size == 0)
        ? 0.0
        : static_cast<double>(upload_offset) / static_cast<double>(upload_size);
    const double bounded_upload_ratio = std::min(1.0, std::max(0.0, upload_ratio));
    const double bounded_slice_ratio =
        std::min(1.0, std::max(0.0, static_cast<double>(slice_pct) / 100.0));
    double progress = 0.0;
    if (state == "printing") {
        progress = 0.05 + 0.95 * bounded_slice_ratio;
    } else if (state == "uploading") {
        progress = 0.05 * bounded_upload_ratio;
    } else if (state == "error") {
        progress = 0.05 + 0.95 * bounded_slice_ratio;
    }
    const int pct = static_cast<int>(progress * 100.0 + 0.5);
    return static_cast<uint32_t>(std::min(100, std::max(0, pct)));
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
    uint32_t slice_pct = 0;
    std::string transfer_name;
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        state = print_state_;
        upload_size = upload_size_;
        upload_offset = upload_offset_;
        slice_pct = slice_progress_pct_;
        refresh_slice_progress_locked(state, slice_pct);
        transfer_name = transfer_name_;
    }

    const uint32_t progress_percent = calc_progress_percent(state, upload_size, upload_offset, slice_pct);

    uint8_t state_code = static_cast<uint8_t>(middleware::XfrTransferState::Idle);
    if (state == "uploading") {
        state_code = static_cast<uint8_t>(middleware::XfrTransferState::Uploading);
    } else if (state == "printing") {
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

    std::string job_id;
    std::string state;
    uint32_t layer = 0;
    uint32_t layer_total = 0;
    uint64_t upload_size = 0;
    uint64_t upload_offset = 0;
    uint32_t slice_pct = 0;
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
        state = print_state_;
        layer = print_layer_;
        layer_total = print_layer_total_;
        upload_size = upload_size_;
        upload_offset = upload_offset_;
        slice_pct = slice_progress_pct_;
        refresh_slice_progress_locked(state, slice_pct);
    }

    if (state == "finished") {
        slice_pct = 100U;
    }
    if (upload_size > 0 && upload_offset > upload_size) {
        upload_offset = upload_size;
    }
    const uint32_t progress_percent = calc_progress_percent(state, upload_size, upload_offset, slice_pct);
    const double progress = static_cast<double>(progress_percent) / 100.0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{"
        << "\"job_id\":\"" << json_escape(job_id) << "\","
        << "\"state\":\"" << json_escape(state) << "\","
        << "\"progress\":" << progress << ","
        << "\"layer\":" << layer << ","
        << "\"layer_total\":" << layer_total << ","
        << "\"elapsed_s\":12,"
        << "\"remaining_s\":120,"
        << "\"nozzle_temp\":0,"
        << "\"bed_temp\":0,"
        << "\"error\":null"
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
}

bool XfrService::trigger_auto_build_async()
{
    {
        std::lock_guard<std::mutex> lk(build_mu_);
        if (build_running_) {
            return false;
        }
        build_running_ = true;
    }
    set_build_state(true, "running", "build running", "", 0);
    std::thread([this]() { run_build_worker(); }).detach();
    return true;
}

void XfrService::run_build_worker()
{
    std::string json_path;
    std::string svg_path;
    std::string stem;
    std::string err_code;
    std::string err_msg;

    if (!find_single_transport_pair(&json_path, &svg_path, &stem, &err_code, &err_msg)) {
        if (err_code == "BUILD_INPUT_MISSING") {
            UPC_LOG_XFR_SERVICE(Info, "auto build waiting pair: %s", err_msg.c_str());
            set_build_state(false, "idle", "waiting for json/svg pair", "", 0);
        } else {
            UPC_LOG_XFR_SERVICE(Warn, "auto build input check failed code=%s msg=%s",
                                err_code.c_str(), err_msg.c_str());
            set_build_state(false, "failed", err_msg, "", -1);
        }
        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            print_state_ = (err_code == "BUILD_INPUT_MISSING") ? "uploading" : "error";
        }
        std::lock_guard<std::mutex> lk(build_mu_);
        build_running_ = false;
        return;
    }

    UPC_LOG_XFR_SERVICE(Info, "auto build input pair json=%s svg=%s stem=%s",
                        json_path.c_str(), svg_path.c_str(), stem.c_str());
    const std::string gcode_name = stem + ".gcode";
    const std::string gcode_path = cfg_.gcode_dir + "/" + gcode_name;
    UPC_LOG_XFR_SERVICE(Info, "auto build output gcode=%s", gcode_path.c_str());

    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        print_state_ = "printing";
        slice_progress_pct_ = 0;
        transfer_name_ = gcode_name;
    }
    if (!run_snowslice_build(json_path, svg_path, gcode_path, &err_code, &err_msg)) {
        UPC_LOG_XFR_SERVICE(Error, "auto build exec failed code=%s msg=%s",
                            err_code.c_str(), err_msg.c_str());
        set_build_state(false, "failed", err_msg, gcode_name, -1);
        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            print_state_ = "error";
        }
        std::lock_guard<std::mutex> lk(build_mu_);
        build_running_ = false;
        return;
    }
    {
        std::error_code rm_progress_ec;
        std::filesystem::remove(cfg_.progress_file, rm_progress_ec);
        if (rm_progress_ec) {
            UPC_LOG_XFR_SERVICE(Warn, "cleanup progress file failed path=%s ec=%d",
                                cfg_.progress_file.c_str(), rm_progress_ec.value());
        } else {
            UPC_LOG_XFR_SERVICE(Info, "cleanup progress file ok path=%s",
                                cfg_.progress_file.c_str());
        }
    }

    if (!update_gcode_manifest(gcode_path, gcode_name, &err_msg)) {
        UPC_LOG_XFR_SERVICE(Error, "auto build manifest update failed msg=%s",
                            err_msg.c_str());
        set_build_state(false, "failed", err_msg, gcode_name, -1);
        {
            std::lock_guard<std::mutex> lk(print_state_mu_);
            print_state_ = "error";
        }
        std::lock_guard<std::mutex> lk(build_mu_);
        build_running_ = false;
        return;
    }

    std::error_code rm_ec1;
    std::error_code rm_ec2;
    std::filesystem::remove(json_path, rm_ec1);
    std::filesystem::remove(svg_path, rm_ec2);
    if (rm_ec1 || rm_ec2) {
        UPC_LOG_XFR_SERVICE(Warn, "cleanup transport failed json_ec=%d svg_ec=%d",
                            rm_ec1.value(), rm_ec2.value());
    } else {
        UPC_LOG_XFR_SERVICE(Info, "cleanup transport ok json=%s svg=%s",
                            json_path.c_str(), svg_path.c_str());
    }

    struct stat st{};
    uint64_t fsize = 0;
    if (stat(gcode_path.c_str(), &st) == 0) {
        fsize = static_cast<uint64_t>(st.st_size);
    }
    UPC_LOG_XFR_SERVICE(Info, "auto build success gcode=%s size=%llu",
                        gcode_path.c_str(),
                        static_cast<unsigned long long>(fsize));
    notify_file_received(gcode_name, fsize);
    set_build_state(false, "success", "build complete", gcode_name, 0);
    {
        std::lock_guard<std::mutex> lk(print_state_mu_);
        print_state_ = "finished";
        slice_progress_pct_ = 100;
        transfer_name_ = gcode_name;
    }

    std::lock_guard<std::mutex> lk(build_mu_);
    build_running_ = false;
}

HttpResponse XfrService::handle_build_status(const HttpRequest& req)
{
    std::string token;
    if (!need_auth(req, &token)) {
        UPC_LOG_XFR_SERVICE(Warn, "build/status auth failed");
        return err(401, "AUTH_INVALID", "missing or invalid token");
    }
    std::string state;
    std::string msg;
    std::string gcode;
    int exit_code = 0;
    {
        std::lock_guard<std::mutex> lk(build_state_mu_);
        state = build_state_;
        msg = build_message_;
        gcode = build_gcode_name_;
        exit_code = build_exit_code_;
    }
    const bool running = (state == "running");
    std::ostringstream oss;
    oss << "{"
        << "\"state\":\"" << json_escape(state) << "\","
        << "\"running\":" << (running ? "true" : "false") << ","
        << "\"message\":\"" << json_escape(msg) << "\","
        << "\"gcode\":\"" << json_escape(gcode) << "\","
        << "\"exit_code\":" << exit_code
        << "}";
    HttpResponse rsp;
    rsp.status = 200;
    rsp.body = oss.str();
    return rsp;
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

std::string XfrService::shell_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
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

bool XfrService::find_single_transport_pair(std::string* json_path,
                                            std::string* svg_path,
                                            std::string* stem,
                                            std::string* err_code,
                                            std::string* err_msg) const
{
    if (!json_path || !svg_path || !stem || !err_code || !err_msg) {
        return false;
    }
    *json_path = "";
    *svg_path = "";
    *stem = "";
    try {
        for (const auto& ent : std::filesystem::directory_iterator(cfg_.storage_dir)) {
            if (!ent.is_regular_file()) {
                continue;
            }
            const std::string ext = lower_ext(ent.path());
            if (ext == ".json") {
                if (!json_path->empty()) {
                    *err_code = "BUILD_INPUT_NOT_UNIQUE";
                    *err_msg = "multiple json files found in transport";
                    UPC_LOG_XFR_SERVICE(Warn, "build input duplicated json: %s and %s",
                                        json_path->c_str(), ent.path().string().c_str());
                    return false;
                }
                *json_path = ent.path().string();
            } else if (ext == ".svg") {
                if (!svg_path->empty()) {
                    *err_code = "BUILD_INPUT_NOT_UNIQUE";
                    *err_msg = "multiple svg files found in transport";
                    UPC_LOG_XFR_SERVICE(Warn, "build input duplicated svg: %s and %s",
                                        svg_path->c_str(), ent.path().string().c_str());
                    return false;
                }
                *svg_path = ent.path().string();
            }
        }
    } catch (const std::exception& ex) {
        *err_code = "BUILD_INPUT_ENUM_FAILED";
        *err_msg = std::string("scan transport failed: ") + ex.what();
        UPC_LOG_XFR_SERVICE(Error, "scan transport dir failed dir=%s err=%s",
                            cfg_.storage_dir.c_str(), ex.what());
        return false;
    }
    if (json_path->empty() || svg_path->empty()) {
        *err_code = "BUILD_INPUT_MISSING";
        *err_msg = "json/svg pair not found in transport";
        return false;
    }
    *stem = std::filesystem::path(*json_path).stem().string();
    UPC_LOG_XFR_SERVICE(Info, "build pair accepted with different names json=%s svg=%s",
                        json_path->c_str(), svg_path->c_str());
    return true;
}

bool XfrService::run_snowslice_build(const std::string& json_path,
                                     const std::string& svg_path,
                                     const std::string& gcode_path,
                                     std::string* err_code,
                                     std::string* err_msg)
{
    {
        std::ofstream reset(cfg_.progress_file, std::ios::trunc);
        if (reset.good()) {
            reset << "0\n";
        }
    }
    const std::string cmd = shell_quote(cfg_.snowslice_bin) +
        " --config " + shell_quote(json_path) +
        " --infill-svg " + shell_quote(svg_path) +
        " --out " + shell_quote(gcode_path) +
        " --outPro " + shell_quote(cfg_.progress_file);
    UPC_LOG_XFR_SERVICE(Info, "run build cmd: %s", cmd.c_str());

    pid_t pid = fork();
    if (pid < 0) {
        UPC_LOG_XFR_SERVICE(Error, "build fork failed");
        if (err_code) { *err_code = "BUILD_EXEC_FAILED"; }
        if (err_msg) { *err_msg = "fork failed"; }
        return false;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    UPC_LOG_XFR_SERVICE(Info, "build process started pid=%d wait_mode=blocking",
                        static_cast<int>(pid));

    int status = 0;
    const pid_t r = waitpid(pid, &status, 0);
    if (r < 0) {
        UPC_LOG_XFR_SERVICE(Error, "build waitpid failed pid=%d", static_cast<int>(pid));
        if (err_code) { *err_code = "BUILD_EXEC_FAILED"; }
        if (err_msg) { *err_msg = "waitpid failed"; }
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        UPC_LOG_XFR_SERVICE(Error, "build process exit abnormal exited=%d code=%d",
                            WIFEXITED(status) ? 1 : 0,
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        if (err_code) { *err_code = "BUILD_EXEC_FAILED"; }
        if (err_msg) {
            std::ostringstream oss;
            oss << "snowslice_demo exit code "
                << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            *err_msg = oss.str();
        }
        return false;
    }

    struct stat st{};
    if (stat(gcode_path.c_str(), &st) != 0 || st.st_size <= 0) {
        UPC_LOG_XFR_SERVICE(Error, "build output missing/empty path=%s", gcode_path.c_str());
        if (err_code) { *err_code = "BUILD_OUTPUT_MISSING"; }
        if (err_msg) { *err_msg = "gcode output missing or empty"; }
        return false;
    }
    UPC_LOG_XFR_SERVICE(Info, "build output ready path=%s size=%lld",
                        gcode_path.c_str(), static_cast<long long>(st.st_size));
    return true;
}

bool XfrService::count_gcode_lines(const std::string& gcode_path, uint32_t* out_lines)
{
    if (!out_lines) {
        return false;
    }
    *out_lines = 0;
    std::ifstream in(gcode_path);
    if (!in.good()) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        ++(*out_lines);
    }
    return true;
}

bool XfrService::update_gcode_manifest(const std::string& gcode_path,
                                       const std::string& gcode_name,
                                       std::string* err_msg) const
{
    struct stat st{};
    if (stat(gcode_path.c_str(), &st) != 0) {
        UPC_LOG_XFR_SERVICE(Error, "manifest stat gcode failed path=%s", gcode_path.c_str());
        if (err_msg) { *err_msg = "stat gcode failed"; }
        return false;
    }
    uint32_t line_count = 0;
    if (!count_gcode_lines(gcode_path, &line_count)) {
        UPC_LOG_XFR_SERVICE(Error, "manifest count lines failed path=%s", gcode_path.c_str());
        if (err_msg) { *err_msg = "count gcode lines failed"; }
        return false;
    }

    std::vector<GcodeManifestEntry> entries;
    if (!gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, entries)) {
        UPC_LOG_XFR_SERVICE(Error, "manifest load failed path=%s", UPC_LOCAL_GCODE_CONFIG);
        if (err_msg) { *err_msg = "load gcode manifest failed"; }
        return false;
    }

    GcodeManifestEntry entry;
    entry.name = gcode_name;
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
    UPC_LOG_XFR_SERVICE(Info, "manifest updated gcode=%s size=%u lines=%u mtime=%lld",
                        gcode_name.c_str(), entry.file_size, entry.line_count,
                        static_cast<long long>(entry.mtime));
    return true;
}

void XfrService::set_build_state(bool running,
                                 const std::string& state,
                                 const std::string& message,
                                 const std::string& gcode_name,
                                 int exit_code)
{
    (void)running;
    std::lock_guard<std::mutex> lk(build_state_mu_);
    build_state_ = state;
    build_message_ = message;
    build_gcode_name_ = gcode_name;
    build_exit_code_ = exit_code;
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

} // namespace upc::xfr
