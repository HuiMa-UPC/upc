#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

/** Glavo 侧 IPC API：处理 UI 请求、推送 Ntf/Rsp。契约见 common/contract/README.md */

#include "gcode_manifest.hpp"
#include "mcu_link.hpp"
#include "msg_port.hpp"
#include "msg_types.hpp"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace upc::glavo {

class GlavoService {
public:
    GlavoService();
    ~GlavoService();

    GlavoService(const GlavoService&) = delete;
    GlavoService& operator=(const GlavoService&) = delete;

    bool init();
    bool start();
    bool stop();

private:
    enum class ServiceState {
        Idle,
        Initializing,
        Running,
        Stopping,
        Error
    };

    void handle_message(const middleware::Message& msg);
    void handle_dir_open(const middleware::Message& msg);
    void handle_fs_delete_ntf(const middleware::Message& msg);
    void handle_print_prepare(const middleware::Message& msg);
    void handle_parser_exit(const middleware::Message& msg);
    void handle_print_start(const middleware::Message& msg);
    void handle_print_terminate(const middleware::Message& msg);
    void handle_print_pause(const middleware::Message& msg);
    void handle_print_resume(const middleware::Message& msg);
    void handle_supply_status_req(const middleware::Message& msg);
    void handle_pm25_request(const middleware::Message& msg);
    void handle_close_door_request(const middleware::Message& msg);
    void handle_open_door_request(const middleware::Message& msg);
    void handle_purify_request(const middleware::Message& msg);
    void handle_mcu_ota_start(const middleware::Message& msg);
    void handle_mcu_ota_cancel(const middleware::Message& msg);
    void publish_mcu_ota_status(const middleware::McuOtaStatusPayload& st);

    /* ---- 调试页手动控制：UI_Debug_* → serial_debug 组帧 → McuLink 串口下发 ----
     * 振镜（以太网 TCP）、回粉电机 后端逻辑预留，故不订阅、不下发。 */
    void handle_debug_axis_jog(const middleware::Message& msg);
    void handle_debug_axis_hold_stop(const middleware::Message& msg);
    void handle_debug_axis_set_zero(const middleware::Message& msg);
    void handle_debug_axis_home(const middleware::Message& msg);
    void handle_debug_device(const middleware::Message& msg);
    void handle_debug_powder_bucket(const middleware::Message& msg);
    void handle_debug_pusher(const middleware::Message& msg);
    void handle_debug_inner_door(const middleware::Message& msg);
    void handle_debug_fan(const middleware::Message& msg);
    void handle_debug_fan_duty(const middleware::Message& msg);
    void handle_debug_switch(const middleware::Message& msg);
    void handle_debug_clean_test_start(const middleware::Message& msg);
    void handle_debug_print_param(const middleware::Message& msg);
    void handle_debug_laser_power_set(const middleware::Message& msg);
    void handle_debug_laser_power_reset(const middleware::Message& msg);
    void handle_debug_laser_alarm_reset(const middleware::Message& msg);
    void debug_tx(const uint8_t* frame, int n);

    void send_door_rsp(const middleware::Message& msg, middleware::MsgType rsp_type);
    void send_dir_open_empty(const middleware::Message& req);
    void send_dir_open_entries(const middleware::Message& req,
                               const std::vector<GcodeManifestEntry>& local_entries,
                               const std::string& base_path);
    void send_dir_open_usb(const middleware::Message& req, const std::string& usb_path);
    void stop_print_job();
    void clear_prepare_session();
    bool light_scan_line_count(const std::string& path, uint32_t& line_count);
    bool copy_usb_to_local(const std::string& src, const std::string& dst);
    bool run_print_prepare(middleware::PrintFileSource source, uint32_t index,
                           std::string& local_path_out);
    void on_mcu_link_event(McuLinkEvent ev);
    void on_motion_batch_sent();
    void publish_terminate_done();
    void publish_print_pause_done(bool paused);
    void reset_print_progress_state();
    void send_print_progress(bool force_complete);

    void on_message(const middleware::Message& msg);
    void worker_loop();
    [[nodiscard]] bool publish_async(const middleware::Message& msg);
    void subscribe(middleware::MsgType type, middleware::MsgHandler handler);
    [[nodiscard]] ServiceState state() const noexcept;
    [[nodiscard]] static uint32_t module_id() noexcept;

    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out) {
        if (payload.type != middleware::MsgPayload::Type::Inline ||
            payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, payload.data.bytes.data(), sizeof(T));
        return true;
    }

    std::string make_full_path(const std::string& dir, const std::string& name);
    static std::string payload_to_string(const middleware::MsgPayload& payload);

    std::atomic<ServiceState> state_{ServiceState::Idle};
    std::vector<middleware::Subscription> subscriptions_;

    std::thread worker_thread_;
    std::queue<middleware::Message> worker_queue_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::atomic<bool> worker_running_{false};

    std::vector<GcodeManifestEntry> local_manifest_;
    std::vector<std::string>        usb_entries_;

    std::string prepared_local_path_;
    bool        prepare_ready_ = false;

    std::string current_file_path_;

    std::atomic<bool> print_running_{false};
    int last_print_pct_     = -1;
    bool last_parse_done_   = false;
};

} // namespace upc::glavo

#endif
