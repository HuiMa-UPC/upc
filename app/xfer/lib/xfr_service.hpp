#ifndef UPC_XFR_SERVICE_HPP
#define UPC_XFR_SERVICE_HPP

#include "auth.hpp"
#include "gcode_manifest.hpp"
#include "http_server.hpp"
#include "mdns_publisher.hpp"
#include "msg_port.hpp"
#include "transfer_3mf_importer.hpp"
#include "upload_manager.hpp"
#include "xfr_config.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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
    HttpResponse handle_print_cancel(const HttpRequest& req);
    HttpResponse handle_print_pause(const HttpRequest& req);
    HttpResponse handle_print_resume(const HttpRequest& req);
    HttpResponse handle_print_history(const HttpRequest& req);

    enum class PrintCtrlKind : uint8_t { Cancel, Pause, Resume };
    HttpResponse handle_print_control(const HttpRequest& req, PrintCtrlKind kind);
    bool job_id_matches_locked(const std::string& job_id) const;
    bool publish_print_ctrl(PrintCtrlKind kind) const;
    bool wait_print_ctrl_done(std::chrono::milliseconds timeout);
    bool publish_print_prepare(uint32_t file_index) const;
    bool publish_print_start_req(uint32_t file_index) const;
    bool find_local_file_index(const std::string& name, uint32_t* index_out) const;
    HttpResponse handle_timelapse_images(const HttpRequest& req, const std::string& job_id);
    HttpResponse handle_timelapse_file(const HttpRequest& req,
                                       const std::string& job_id,
                                       const std::string& file_name);

    void subscribe_print_ipc();
    void on_print_ipc(const middleware::Message& msg);

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

    static std::string request_path_only(const std::string& raw_path);
    static std::string request_query(const std::string& raw_path);
    static bool query_get_int(const std::string& query, const std::string& key, int* out);
    static bool is_safe_job_id(const std::string& job_id);
    static bool is_jpg_frame_name(const std::string& name, uint32_t* index_out);
    static bool calendar_date_valid(int year, int month, int day);
    static bool entry_on_local_date(int64_t start_unix, int year, int month, int day);
    static std::vector<uint32_t> list_timelapse_jpg_indices(const std::string& job_dir);
    static std::string public_base_url(const HttpRequest& req, uint16_t http_port);

    XfrConfig cfg_;
    PairManager pair_mgr_;
    UploadManager upload_mgr_;
    Transfer3mfImporter transfer_3mf_importer_;
    MdnsPublisher mdns_;
    HttpServer http_;
    std::vector<middleware::Subscription> subs_;

    mutable std::mutex print_state_mu_;
    std::string print_job_id_;
    std::string print_state_ = "idle";
    uint32_t print_layer_ = 0;
    uint32_t print_layer_total_ = 0;
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

    /** Glavo / Media IPC 缓存（供桌面端 /print/status 与延时摄影绑定） */
    enum class GlavoPhase : uint8_t {
        Idle = 0,
        Printing,
        Paused,
        Finished,
        Canceled,
        Error,
    };
    GlavoPhase glavo_phase_ = GlavoPhase::Idle;
    int32_t  glavo_progress_percent_ = 0;
    uint32_t glavo_current_layer_ = 0;
    uint32_t glavo_total_layers_ = 0;
    uint16_t glavo_layer_height_um_ = 0;
    uint32_t glavo_remaining_minutes_ = 0;
    float    glavo_chamber_temp_c_ = 0.f;
    uint64_t glavo_since_ms_ = 0;
    std::string glavo_error_;
    std::string active_frames_dir_;
    bool print_session_active_ = false;

    /** HTTP 线程等 Glavo 暂停/恢复/终止/Prepare 回包（主循环 process 后 notify） */
    enum class PrintCtrlWait : uint8_t { None, PauseOn, PauseOff, Canceled, Prepare };
    std::condition_variable print_ctrl_cv_;
    PrintCtrlWait print_ctrl_wait_ = PrintCtrlWait::None;
    bool print_ctrl_got_ = false;
    bool prepare_ok_ = false;
    std::string last_file_id_;
    std::string last_printable_name_;
};

} // namespace upc::xfr

#endif
