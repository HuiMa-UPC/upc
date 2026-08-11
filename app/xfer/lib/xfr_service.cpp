#include "xfr_service.hpp"

#include "msg_port.hpp"
#include "parser_manager.hpp"
#include "rs_log.hpp"
#include "upc_contract.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
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
    refresh_slice_progress_if_needed();
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
    std::string state;
    uint32_t layer = 0;
    uint32_t layer_total = 0;
    uint64_t upload_size = 0;
    uint64_t upload_offset = 0;
    uint32_t slice_progress_pct = 0;
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
        slice_progress_pct = slice_progress_pct_;
    }
    if (upload_size > 0 && upload_offset > upload_size) {
        upload_offset = upload_size;
    }
    const uint32_t progress_percent = calc_progress_percent(
        state, upload_size, upload_offset, slice_progress_pct);
    const double progress = static_cast<double>(progress_percent) / 100.0;
    const std::string state_for_rsp = (state == "slicing") ? "printing" : state;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{"
        << "\"job_id\":\"" << json_escape(job_id) << "\","
        << "\"state\":\"" << json_escape(state_for_rsp) << "\","
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

} // namespace upc::xfr
