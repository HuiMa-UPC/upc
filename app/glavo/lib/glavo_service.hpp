#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

/** Glavo 侧 IPC API：处理 UI 请求、推送 Ntf/Rsp。契约见 common/contract/README.md */

#include "gcode_manifest.hpp"
#include "mcu_link.hpp"
#include "optical_path_check.hpp"
#include "height_calibration.hpp"
#include "plate_height_measure.hpp"
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
    /**
     * @brief 光路检测放行后的起印后半段：open/parse + 首层参考图 gate
     * @note 从 launch_print_from_prepared 拆出，供检测通过后由 worker 续跑。
     */
    void continue_print_after_optical_ready(const std::string& full_path);
    void begin_print_motion_after_reference_ready();
    void handle_visual_first_layer_reference_ready_rsp(
        const middleware::Message& msg);
    void handle_print_terminate(const middleware::Message& msg);
    void handle_print_pause(const middleware::Message& msg);
    void handle_print_resume(const middleware::Message& msg);
    void handle_print_powder_low_ignore(const middleware::Message& msg);
    void handle_supply_status_req(const middleware::Message& msg);
    void handle_pm25_request(const middleware::Message& msg);
    void publish_pm25_rsp(uint32_t target_id, bool is_high, float pm25_cabin);
    void stop_pm25_sample_worker();
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
    void handle_debug_galvo_red_always(const middleware::Message& msg);
    /** @brief 振镜绝对坐标移动（手动控制/测高扫描）：调 McuLink 下发单点运动包。 */
    void handle_debug_galvo_abs_move(const middleware::Message& msg);
    void handle_debug_galvo_param_save(const middleware::Message& msg);
    void handle_debug_galvo_point_test_start(const middleware::Message& msg);
    void handle_debug_galvo_point_test_get(const middleware::Message& msg);
    void handle_debug_test_image_set(const middleware::Message& msg);
    void handle_debug_test_image_get(const middleware::Message& msg);
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
    void handle_debug_skip_o2_gate_set(const middleware::Message& msg);
    void handle_debug_skip_o2_gate_get(const middleware::Message& msg);
    void handle_debug_o2_calib_req(const middleware::Message& msg);
    void handle_debug_o2_calib_get(const middleware::Message& msg);
    void handle_debug_alarm_inject(const middleware::Message& msg);
    void handle_debug_first_layer_visual_check_set(const middleware::Message& msg);
    void handle_debug_first_layer_visual_check_get(const middleware::Message& msg);
    void handle_debug_optical_path_check_set(const middleware::Message& msg);
    void handle_debug_optical_path_check_get(const middleware::Message& msg);
    void handle_debug_plate_height_adj_set(const middleware::Message& msg);
    void handle_debug_plate_height_adj_get(const middleware::Message& msg);
    void handle_debug_soc_led_set(const middleware::Message& msg);
    void handle_debug_soc_led_get(const middleware::Message& msg);
    void handle_debug_cabin_pm_valve_set(const middleware::Message& msg);
    void handle_debug_cabin_pm_valve_get(const middleware::Message& msg);
    void handle_debug_status_led_set(const middleware::Message& msg);
    void handle_debug_status_led_get(const middleware::Message& msg);
    void handle_visual_optical_path_point_rsp(const middleware::Message& msg);
    void handle_height_calib_start_req(const middleware::Message& msg);
    void handle_laser_spot_measure_rsp(const middleware::Message& msg);
    void publish_debug_print_h264_record_state();
    void publish_debug_clean_record_state();
    void publish_debug_env_sim_state();
    void publish_debug_env_sim_powder_state();
    void publish_debug_skip_o2_gate_state();
    void publish_debug_o2_calib_state();
    void publish_debug_first_layer_visual_check_state();
    void publish_debug_test_image_state();
    void publish_debug_galvo_point_test_state();
    void publish_debug_optical_path_check_state();
    void publish_debug_plate_height_adj_state();
    void publish_debug_soc_led_state();
    void publish_debug_cabin_pm_valve_state();
    void publish_debug_status_led_state();
    /** @brief 停止并回收点位测试线程；仅用于服务退出。 */
    void stop_galvo_point_test_worker();

    /* ---- 打印前光路检测（优先级高于首层检测） ---- */
    /** @brief 注册 OpticalPathCheck 的依赖回调；init 阶段调用一次。 */
    void configure_optical_path_check();
    void configure_plate_height();
    bool begin_plate_height_gate();
    void on_plate_height_done();
    void publish_plate_height_visual_session(bool active);
    /** @brief 舱内灯光：与调试页 Light 同一串口 TLV（0x06 开 / 0x07 关）。 */
    void set_cabin_light(bool on);
    void send_z_inc(float delta_mm);
    void send_z_home();
    /** 校准/测高回零：AXIS_INC 下降 kHeightHomeRetractMm，短等后发 Home。 */
    void send_z_home_with_retract();
    bool wait_z_settled(float target_mm, float tolerance_mm, int timeout_ms);
    bool request_laser_spot_measure_sync(float& out_x, float& out_y, int timeout_ms);
    void publish_height_calib_progress(uint8_t stage, float current_height_mm,
                                       uint16_t collected, uint16_t total);
    void wakeup_laser_spot_wait();
    /**
     * @brief 起印前的光路检测 gate
     * @return true=已起一轮检测，调用方须立即 return 等回灌；false=无需检测，继续测高/起印
     */
    bool begin_optical_path_gate();
    /** @brief 检测线程结果回灌 worker 后的处理（续跑起印或阻断并报警）。 */
    void on_optical_path_check_done();
    /**
     * @brief 「运动还没开始就起印失败」的统一收尾
     *
     * 切红光安全态、清挂起标志与打印态、通知 UI 跳终止结果页。
     * prepare 会话（本地副本 + 行数缓存）保留，便于重试时跳过复制与扫描。
     */
    void abort_print_start(middleware::PrintStartRejectReason reason, uint8_t detail);
    /** @brief 光路检测失败：先推 Critical 告警，再走 abort_print_start。 */
    void abort_print_for_optical_path(uint8_t failed_point_index);
    void publish_optical_path_progress(
        const middleware::OpticalPathProgressPayload& payload);
    void publish_optical_path_visual_session(bool active);
    void publish_print_start_rejected(middleware::PrintStartRejectReason reason,
                                      uint8_t detail);
    /** @brief 开一轮测试图：红光 + open/parse/begin（无媒体/换层握手） */
    bool launch_test_image_pass();
    /** @brief 停测试循环：清 desired，request_terminate（不切激光） */
    void stop_test_image_session();
    void clear_test_image_session();
    bool publish_first_layer_visual_session(bool active, const char* reason);
    bool request_first_layer_visual_check(uint16_t layer_height_um);
    void handle_visual_first_layer_check_rsp(const middleware::Message& msg);
    /** @brief 联动切换红光/激光模式（TCP 红光 + 串口激光使能），每次调用都下发。 */
    bool apply_red_laser_mode(bool red_on, const char* reason);
    /** @brief 红光常亮：先入队串口 0x12，drain 后再发 TCP Red=3（V3/V4 相同）。 */
    bool apply_red_always_mode(const char* reason);
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
    void set_print_running(bool on);
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
    uint32_t    prepared_layer_count_ = 0; /**< Prepare 阶段得到的总层数（清单或 M3000） */
    bool        prepare_ready_ = false;
    bool        prepared_parser_open_ = false; /**< Prepare 阶段已打开 parser 文件，可在 Start 复用。 */

    std::string current_file_path_;

    /**
     * 上一作业仍在 terminate 清理时收到新的 Start_Req：先等 TerminateDone，
     * 再 launch_print_from_prepared，避免与 run_manual_terminate_cleanup 竞态。
     */
    bool deferred_print_start_{false};

    std::atomic<bool> print_running_{false};
    /**
     * 调试 Test image：与正式 print_running_ 互斥。
     * desired=用户开关；active=本轮正在解析/下发或等待完成事件。
     */
    bool test_image_desired_{false};
    bool test_image_active_{false};
    /**
     * 中心点 + 四角红光驻留测试：独立线程执行，避免 3 秒等待阻塞业务 worker。
     * UI 不提供取消；stop_requested 仅用于进程退出时及时回收线程。
     */
    std::thread galvo_point_test_thread_;
    std::atomic<bool> galvo_point_test_active_{false};
    std::atomic<bool> galvo_point_test_stop_requested_{false};
    std::mutex galvo_point_test_wait_mtx_;
    std::condition_variable galvo_point_test_wait_cv_;
    uint32_t print_total_layers_ = 0; /**< 当前作业层总数（纯层进度分母 / 回退时的层锚点分母） */
    /** 本作业是否走纯层台阶进度（G-code + 能数出 M3000 + 换层 Snap 已启用） */
    bool layer_progress_mode_ = false;
    int last_print_pct_     = -1;
    uint16_t last_sent_cmd_rate_  = 0xffff;
    uint16_t last_sent_data_rate_ = 0xffff;
    std::chrono::steady_clock::time_point last_drain_progress_at_{};
    /** 上次进度推送时刻；纯层段层内百分比不变时用于环境字段保活 */
    std::chrono::steady_clock::time_point last_progress_push_at_{};
    bool last_usb_mounted_ = false;
    bool door_real_open_enabled_ = (GLAVO_DOOR_REAL_OPEN_ENABLE != 0);
    bool door_real_close_check_enabled_ = (GLAVO_DOOR_REAL_CLOSE_CHECK_ENABLE != 0);
    float pm25_clean_threshold_ = 50.0f;
    uint32_t timelapse_video_fps_ = 0; /**< 0=Media 默认；来自 cal.csv TIMELAPSE_VIDEO_FPS */
    uint32_t print_h264_bitrate_ = 0; /**< 0=Media 默认中等；来自 cal.csv PRINT_H264_BITRATE */
    std::thread clean_worker_;
    std::atomic<bool> clean_running_{false};
    std::thread pm25_sample_worker_;
    std::atomic<bool> pm25_sample_busy_{false};
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
    /* ---- 打印前光路检测 ---- */
    bool optical_path_check_on_ = false; /**< 光路检测开关（仅本次运行，默认关） */
    bool plate_height_adj_on_ = false; /**< 打印前测高调 Z（仅本次运行，默认关） */
    OpticalPathCheck optical_path_check_;
    /**
     * 检测线程结果回灌 worker 的 pending 位（与 clean_test_done_pending_ 同款）。
     * 线程侧只置位 + notify，起印续跑一律回到 worker 串行执行。
     */
    std::atomic<bool> optical_path_done_pending_{false};
    std::atomic<bool> optical_path_done_passed_{false};
    std::atomic<uint8_t> optical_path_failed_point_{kNoFailedPoint};
    /**
     * 上一作业仍在 terminate 清理时，运动包会被 McuLink 拒发而误报光路异常。
     * 此时置位，等 TerminateDone/PrintError 再起检测（仿 deferred_print_start_）。
     */
    bool deferred_optical_check_ = false;
    HeightCalibration height_calib_;
    PlateHeightMeasure plate_height_;
    std::mutex laser_spot_mtx_;
    std::condition_variable laser_spot_cv_;
    bool laser_spot_waiting_ = false;
    middleware::LaserSpotMeasureRspPayload laser_spot_result_{};
    std::atomic<bool> plate_height_done_pending_{false};
    std::atomic<bool> plate_height_done_ok_{false};
    std::atomic<bool> plate_height_flatness_warn_{false};
    bool deferred_plate_height_ = false;
    std::string plate_height_resume_path_;
    bool env_sim_on_ = false;  /**< 环境上报模拟（仅本次运行，默认关） */
    bool env_sim_powder_low_on_ = false; /**< 环境模拟粉量缺粉开关（目标状态，延迟生效由 McuLink 管） */
    bool skip_o2_print_gate_on_ = false; /**< 跳过 SOC 氧打印门（仅本次运行，默认关） */
    bool ignore_powder_low_for_current_print_ = false; /**< 本次打印是否忽略缺粉检测（Cancel 后置位） */
    bool o2_auto_paused_ = false; /**< 本次因 SOC 氧过高由本层自动暂停（恢复时只清这类暂停） */
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
