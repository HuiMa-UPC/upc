#ifndef UPC_XFR_SERVICE_HPP
#define UPC_XFR_SERVICE_HPP

#include "auth.hpp"
#include "gcode_manifest.hpp"
#include "http_server.hpp"
#include "mdns_publisher.hpp"
#include "upload_manager.hpp"
#include "xfr_config.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace upc::xfr {

class XfrService {
public:
    explicit XfrService(const XfrConfig& cfg);

    bool init();
    void shutdown();
    void tick();

private:
    HttpResponse on_request(const HttpRequest& req);
    HttpResponse handle_info() const;
    HttpResponse handle_pair_start(const HttpRequest& req);
    HttpResponse handle_pair_verify(const HttpRequest& req);
    HttpResponse handle_pair_revoke(const HttpRequest& req);
    HttpResponse handle_upload_init(const HttpRequest& req);
    HttpResponse handle_upload_put(const HttpRequest& req, const std::string& upload_id);
    HttpResponse handle_upload_complete(const HttpRequest& req, const std::string& upload_id);
    HttpResponse handle_upload_status(const HttpRequest& req, const std::string& upload_id);
    HttpResponse handle_print_start(const HttpRequest& req);
    HttpResponse handle_print_status(const HttpRequest& req);

    bool need_auth(const HttpRequest& req, std::string* token_out) const;
    static bool json_get_string(const std::string& body, const std::string& key, std::string* out);
    static bool json_get_u64(const std::string& body, const std::string& key, uint64_t* out);
    static bool parse_content_range(const std::string& v, uint64_t* start, uint64_t* end, uint64_t* total);
    static std::string json_escape(const std::string& s);
    static HttpResponse err(int status, const std::string& code, const std::string& message);
    static bool read_progress_percent_file(const std::string& path, uint32_t* out_pct);
    static std::string output_ovf_name(const std::string& input_name);
    bool run_snowslice_build(const std::string& input_ovf_path,
                             const std::string& output_ovf_path,
                             std::string* err_msg) const;
    void refresh_slice_progress_if_needed();

    bool preprocess_ovf_and_update_manifest(const std::string& ovf_path,
                               const std::string& ovf_name,
                               std::string* err_msg) const;
    uint32_t calc_progress_percent(const std::string& state,
                                   uint64_t upload_size,
                                   uint64_t upload_offset,
                                   uint32_t slice_progress_pct) const;
    void publish_transfer_progress_if_needed();

    void notify_pair_pin(const std::string& pin, uint32_t expires_s) const;
    void notify_file_received(const std::string& name, uint64_t size) const;

    XfrConfig cfg_;
    PairManager pair_mgr_;
    UploadManager upload_mgr_;
    MdnsPublisher mdns_;
    HttpServer http_;
    mutable std::mutex print_state_mu_;
    std::string print_job_id_;
    std::string print_state_ = "idle";
    uint32_t print_layer_ = 0;
    uint32_t print_layer_total_ = 200;
    uint64_t upload_size_ = 0;
    uint64_t upload_offset_ = 0;
    uint32_t slice_progress_pct_ = 0;
    std::string transfer_name_;
    std::chrono::steady_clock::time_point last_progress_emit_at_{};
    uint8_t  last_progress_state_ = 0xff;
    uint32_t last_progress_percent_ = 0xffffffffu;
    uint64_t last_progress_uploaded_ = UINT64_MAX;
    uint64_t last_progress_total_ = UINT64_MAX;
    std::string last_progress_name_;
};

} // namespace upc::xfr

#endif
