#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

/** Glavo 侧 IPC API：处理 UI 请求、推送 Ntf/Rsp。契约见 common/contract/README.md */

#include "gcode_manifest.hpp"
#include "mcu_link.hpp"
#include "wifi_manager.hpp"
#include "msg_port.hpp"
#include "msg_runtime.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
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
    /** @brief 在已 prepare 的路径上真正 open/parse/begin（供起印与延迟起印共用） */
    void launch_print_from_prepared(const std::string& full_path);
    void begin_print_motion_after_reference_ready();
    void handle_visual_first_layer_reference_ready_rsp(
        const middleware::Message& msg);
    void handle_print_terminate(const middleware::Message& msg);
    void handle_print_pause(const middleware::Message& msg);
    void handle_print_resume(const middleware::Message& msg);
    void handle_print_powder_low_ignore(const middleware::Message& msg);
    void handle_supply_status_req(const middleware::Message& msg);
    void handle_pm25_request(const middleware::Message& msg);
    void handle_close_door_request(const middleware::Message& msg);
    void handle_open_door_request(const middleware::Message& msg);
    void handle_purify_request(const middleware::Message& msg);
    void handle_clean_start_request(const middleware::Message& msg);
    void handle_clean_stop_request(const middleware::Message& msg);
    void handle_clean_test_start_request(const middleware::Message& msg);
    void handle_clean_test_close_request(const middleware::Message& msg);
    void handle_transport_request(const middleware::Message& msg);
    void publish_transport_result(uint8_t result);
    void on_transport_result(uint8_t result);
    void stop_clean_worker();
    void publish_clean_rsp(middleware::MsgType type, uint32_t target_id);
    void publish_clean_test_start_rsp(uint32_t target_id, bool accepted);
    void publish_clean_test_done(uint8_t result_code, bool raw_retained,
                                 bool has_mp4, const std::string& mp4_rel_path);
    void publish_clean_test_close_rsp(uint32_t target_id);
    void on_clean_test_done(uint8_t result_code);
    void notify_clean_video_start(const middleware::CleanTestStartReqPayload& req);
    void notify_clean_video_stop(uint8_t result_code, bool keep_raw);
    void handle_mcu_ota_start(const middleware::Message& msg);
    void handle_mcu_ota_cancel(const middleware::Message& msg);
    void publish_mcu_ota_status(const middleware::McuOtaStatusPayload& st);
    void handle_serial_mcu_ota_start(const middleware::Message& msg);
    void handle_serial_mcu_ota_cancel(const middleware::Message& msg);
    void publish_serial_mcu_ota_status(const middleware::McuOtaStatusPayload& st);

    /* ---- 网络 / WiFi：UI_Wifi_* → WifiManager(ConnMan)，回推 State/Scan/Saved ---- */
    void handle_wifi_enable(const middleware::Message& msg);
    void handle_wifi_scan(const middleware::Message& msg);
    void handle_wifi_connect(const middleware::Message& msg);
    void handle_wifi_forget(const middleware::Message& msg);
    void handle_wifi_disconnect(const middleware::Message& msg);
    void handle_wifi_info(const middleware::Message& msg);
    void handle_wifi_saved(const middleware::Message& msg);
    void publish_wifi_state(const WifiSnapshot& snap);
    void publish_wifi_list(WifiListKind kind, const std::vector<WifiEntry>& entries);
    void publish_wifi_connect_result(bool ok, const std::string& ssid);
    void publish_usb_hotplug(bool mounted);
    void start_usb_hotplug_monitor();
    void stop_usb_hotplug_monitor();
    void usb_hotplug_loop();

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
    void handle_debug_galvo_red_light(const middleware::Message& msg);
    void handle_debug_clean_test_start(const middleware::Message& msg);
    void handle_debug_clean_test_close(const middleware::Message& msg);
    void handle_debug_print_param(const middleware::Message& msg);
    void handle_debug_laser_power_set(const middleware::Message& msg);
    void handle_debug_laser_power_reset(const middleware::Message& msg);
    void handle_debug_laser_alarm_reset(const middleware::Message& msg);
    void handle_debug_print_h264_record_set(const middleware::Message& msg);
    void handle_debug_print_h264_record_get(const middleware::Message& msg);
    void handle_debug_clean_record_set(const middleware::Message& msg);
    void handle_debug_clean_record_get(const middleware::Message& msg);
    void handle_debug_env_sim_set(const middleware::Message& msg);
    void handle_debug_env_sim_get(const middleware::Message& msg);
    void handle_debug_env_sim_powder_set(const middleware::Message& msg);
    void handle_debug_env_sim_powder_get(const middleware::Message& msg);
    void handle_debug_first_layer_visual_check_set(const middleware::Message& msg);
    void handle_debug_first_layer_visual_check_get(const middleware::Message& msg);
    void publish_debug_print_h264_record_state();
    void publish_debug_clean_record_state();
    void publish_debug_env_sim_state();
    void publish_debug_env_sim_powder_state();
    void publish_debug_first_layer_visual_check_state();
    bool publish_first_layer_visual_session(bool active, const char* reason);
    bool request_first_layer_visual_check(uint16_t layer_height_um);
    void handle_visual_first_layer_check_rsp(const middleware::Message& msg);
    /** @brief 联动切换红光/激光模式（TCP 红光 + 串口激光使能），每次调用都下发。 */
    bool apply_red_laser_mode(bool red_on, const char* reason);
    void debug_tx(const uint8_t* frame, int n);

    /** @brief About 页版本查询：下发串口 TLV_GET_VERSION，应答由 McuLink 直接推 UI */
    void handle_mcu_version_req(const middleware::Message& msg);

    /** @brief About 页振镜板版本：回读 McuLink 缓存的 TCP 0x0F 首包版本并推 UI */
    void handle_galvo_version_req(const middleware::Message& msg);

    /** @brief 报警复位请求：组 0xA3/0x82 帧下发串口；结果以后续单条上报为准 */
    void handle_alarm_reset(const middleware::Message& msg);

    /** @brief 报警快照请求：组 0xA3/0x81 TLV 0x05 帧下发；MCU 不主动推快照 */
    void handle_alarm_snapshot_req(const middleware::Message& msg);

    void send_door_rsp(const middleware::Message& msg, middleware::MsgType rsp_type, bool ok);
    void send_dir_open_empty(const middleware::Message& req);
    void send_dir_open_usb(const middleware::Message& req, const std::string& usb_path);
    void stop_print_job();
    void clear_prepare_session();
    bool copy_usb_to_local(const std::string& src, const std::string& dst);
    bool run_print_prepare(middleware::PrintFileSource source, uint32_t index,
                           std::string& local_path_out);
    void on_mcu_link_event(McuLinkEvent ev);
    void enqueue_link_event(McuLinkEvent ev);
    void publish_terminate_done();
    void publish_print_pause_done(bool paused);
    void publish_print_h264_record_pause(bool paused);
    void publish_print_powder_low_ntf();
    /** @brief 向 MQTT 推送 GLAVO_Print_State_Ntf（开始打印/打印结束状态边沿） */
    void publish_print_state_ntf(uint32_t mode);
    void publish_device_event(middleware::DeviceEventCode code,
                              middleware::DeviceEventSeverity severity,
                              bool active);
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

    std::atomic<ServiceState> state_{ServiceState::Idle};
    std::vector<middleware::Subscription> subscriptions_;

    std::thread worker_thread_;
    std::thread usb_hotplug_thread_;
    std::queue<middleware::Message> worker_queue_;
    std::deque<McuLinkEvent> link_event_queue_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::atomic<bool> worker_running_{false};
    std::atomic<bool> usb_hotplug_running_{false};

    std::vector<upc::GcodeManifestEntry> local_manifest_;
    std::vector<std::string>             usb_entries_;

    std::string prepared_local_path_;
    uint32_t    prepared_line_count_ = 0;
    uint32_t    prepared_layer_count_ = 0; /**< Prepare 阶段扫描到的层标记(M3000)数量 */
    bool        prepare_ready_ = false;
    bool        prepared_parser_open_ = false; /**< Prepare 阶段已打开 parser 文件，可在 Start 复用。 */

    std::string current_file_path_;

    /**
     * 上一作业仍在 terminate 清理时收到新的 Start_Req：先等 TerminateDone，
     * 再 launch_print_from_prepared，避免与 run_manual_terminate_cleanup 竞态。
     */
    bool deferred_print_start_{false};

    std::atomic<bool> print_running_{false};
    uint32_t print_total_layers_ = 0; /**< 当前作业层总数（用于进度融合） */
    int last_print_pct_     = -1;
    bool last_parse_done_   = false;
    uint16_t last_sent_cmd_rate_  = 0xffff;
    uint16_t last_sent_data_rate_ = 0xffff;
    std::chrono::steady_clock::time_point last_drain_progress_at_{};
    bool last_usb_mounted_ = false;
    bool door_real_open_enabled_ = (GLAVO_DOOR_REAL_OPEN_ENABLE != 0);
    bool door_real_close_check_enabled_ = (GLAVO_DOOR_REAL_CLOSE_CHECK_ENABLE != 0);
    float pm25_clean_threshold_ = 50.0f;
    uint32_t timelapse_video_fps_ = 0; /**< 0=Media 默认；来自 cal.csv TIMELAPSE_VIDEO_FPS */
    uint32_t print_h264_bitrate_ = 0; /**< 0=Media 默认中等；来自 cal.csv PRINT_H264_BITRATE */
    std::thread clean_worker_;
    std::atomic<bool> clean_running_{false};
    std::atomic<bool> clean_cancel_requested_{false};
    std::atomic<bool> clean_stop_rsp_pending_{false};
    std::atomic<uint32_t> clean_stop_rsp_target_{0};
    std::atomic<bool> clean_test_done_pending_{false};
    std::atomic<uint8_t> clean_test_result_code_{0};
    std::atomic<bool> transport_result_pending_{false};
    std::atomic<uint8_t> transport_result_code_{0};
    bool clean_test_active_ = false;
    bool clean_video_record_on_ = true; /**< 仅调试页状态回读；Purify 录像不再受其门控 */
    bool clean_video_session_started_ = false;
    char clean_test_session_id_[32]{};
    char clean_test_job_name_[64]{};
    bool print_h264_continuous_on_ = false;
    bool first_layer_visual_check_on_ = false; /**< 首层视觉检测开关（仅本次运行） */
    bool first_layer_visual_session_active_ = false; /**< Visual Camera/定位预热会话状态 */
    bool first_layer_reference_waiting_ = false; /**< 等待Visual锁定本次打印裸板B0 */
    bool env_sim_on_ = false;  /**< 环境上报模拟（仅本次运行，默认关） */
    bool env_sim_powder_low_on_ = false; /**< 环境模拟粉量缺粉开关（目标状态，延迟生效由 McuLink 管） */
    bool ignore_powder_low_for_current_print_ = false; /**< 本次打印是否忽略缺粉检测（Cancel 后置位） */
    std::mutex first_layer_visual_mtx_;
    std::condition_variable first_layer_visual_cv_;
    bool first_layer_visual_waiting_ = false;
    uint8_t first_layer_visual_result_ = 0;

    /** @brief MCU OTA 状态上报节流：同值去重 + 高频降采样 */
    std::mutex mcu_ota_status_pub_mtx_;
    bool mcu_ota_status_pub_has_last_ = false;
    middleware::McuOtaStatusPayload mcu_ota_status_pub_last_{};
    std::chrono::steady_clock::time_point mcu_ota_status_pub_last_ts_{};
};

} // namespace upc::glavo

#endif
