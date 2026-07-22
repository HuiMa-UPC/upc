#ifndef UPC_XFR_SERVICE_HPP
#define UPC_XFR_SERVICE_HPP

#include "auth.hpp"
#include "gcode_manifest.hpp"
#include "http_server.hpp"
#include "mdns_publisher.hpp"
#include "upload_manager.hpp"
#include "xfr_config.hpp"

#include <mutex>
#include <string>

namespace upc::xfr {

class XfrService {
public:
    explicit XfrService(const XfrConfig& cfg);

    bool init();
    void shutdown();

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
    HttpResponse handle_build_start(const HttpRequest& req);
    HttpResponse handle_build_status(const HttpRequest& req);

    bool need_auth(const HttpRequest& req, std::string* token_out) const;
    static bool json_get_string(const std::string& body, const std::string& key, std::string* out);
    static bool json_get_u64(const std::string& body, const std::string& key, uint64_t* out);
    static bool parse_content_range(const std::string& v, uint64_t* start, uint64_t* end, uint64_t* total);
    static std::string json_escape(const std::string& s);
    static std::string shell_quote(const std::string& s);
    static HttpResponse err(int status, const std::string& code, const std::string& message);

    bool find_single_transport_pair(std::string* json_path,
                                    std::string* svg_path,
                                    std::string* stem,
                                    std::string* err_code,
                                    std::string* err_msg) const;
    bool run_snowslice_build(const std::string& json_path,
                             const std::string& svg_path,
                             const std::string& gcode_path,
                             std::string* err_code,
                             std::string* err_msg);
    bool update_gcode_manifest(const std::string& gcode_path,
                               const std::string& gcode_name,
                               std::string* err_msg) const;
    static bool count_gcode_lines(const std::string& gcode_path, uint32_t* out_lines);
    void set_build_state(bool running,
                         const std::string& state,
                         const std::string& message,
                         const std::string& gcode_name,
                         int exit_code);
    bool trigger_auto_build_async();
    void run_build_worker();

    void notify_pair_pin(const std::string& pin, uint32_t expires_s) const;
    void notify_file_received(const std::string& name, uint64_t size) const;

    XfrConfig cfg_;
    PairManager pair_mgr_;
    UploadManager upload_mgr_;
    MdnsPublisher mdns_;
    HttpServer http_;
    mutable std::mutex build_mu_;
    mutable std::mutex build_state_mu_;
    bool build_running_ = false;
    std::string build_state_ = "idle";
    std::string build_message_;
    std::string build_gcode_name_;
    int build_exit_code_ = 0;
};

} // namespace upc::xfr

#endif
