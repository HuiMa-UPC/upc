#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

/** Glavo 侧 IPC API：处理 UI 请求、推送 Ntf/Rsp。契约见 common/contract/README.md */

#include "gcode_manifest.hpp"
#include "mcu_link.hpp"
#include "optical_path_check.hpp"
#include "height_calibration.hpp"
#include "plate_height_measure.hpp"
#include "wifi_manager.hpp"
#include "n2_ble_client.hpp"
#include "msg_port.hpp"
#include "msg_runtime.hpp"
#include "cabin_visual_exchange.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace upc::glavo {

/** 起印前半段（Z→−10 / 外门 / 关内门 / 读码）跑在独立线程，结果回灌 worker。 */
enum class EarlyPrintPrepPhase : uint8_t {
    FromLaunch = 0,      /**< Z + 外门 + 内门 + 读码 */
    AfterOuterDoor = 1,  /**< 外门确认后：内门 + 读码 */
    AfterPlatePlace = 2, /**< 放板确认后再检：内门 + 读码 */
};

enum class EarlyPrintPrepResult : uint8_t {
    Continue = 0,         /**< 可进光路/测高 */
    WaitOuterDoor = 1,    /**< 已挂起等 UI 外门确认 */
    WaitPlatePlace = 2,   /**< 已挂起等 UI 放板确认 */
    FailZ = 3,            /**< Z 未到 −10 */
    FailInnerDoor = 4,    /**< 关内门套件失败 */
    FailPlatePresent = 5, /**< 读码 unknown/超时 */
    Aborted = 6,          /**< 用户终止打断 */
};

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
    void handle_transfer_demo_prep(const middleware::Message& msg);
    void handle_transfer_demo_confirm(const middleware::Message& msg);
    void stop_transfer_demo_worker();
    /**
     * @brief 一键演示前半段：Z→−100 → 固定关内门 → 开外门（传感器确认）。
     *        不看 Keep Z / 挡门视觉。true=外门已开；转运等装盒 Done 后再发。
     */
    bool transfer_demo_z_close_inner_and_open_outer();
    void publish_transport_result(uint8_t result);
    void on_transport_result(uint8_t result);
    void stop_clean_worker();
    /** @brief 回收真实开门轮询线程（stop / 析构前调用）。 */
    void stop_door_open_worker();
    /** @brief join 起印前半段准备线程；终止路径先 abort 再调。 */
    void stop_early_print_prep_worker();
    void publish_clean_rsp(middleware::MsgType type, uint32_t target_id);
    void publish_clean_test_start_rsp(uint32_t target_id, bool accepted);
    void publish_clean_test_done(uint8_t result_code, bool raw_retained,
                                 bool has_mp4, const std::string& mp4_rel_path);
    void publish_clean_test_close_rsp(uint32_t target_id);
    void on_clean_test_done(uint8_t result_code);
    /** @brief MCU 清粉结束 → GLAVO_Debug_Clean_Test_Ntf（不论发起方，调试页据此复位） */
    void publish_debug_clean_test_result(uint8_t result_code);
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
    void publish_n2_status(const N2BleSnapshot& snap);
    void publish_wifi_list(WifiListKind kind, const std::vector<WifiEntry>& entries);
    void publish_wifi_connect_result(bool ok, const std::string& ssid);
    void publish_usb_hotplug(bool mounted);
    void start_usb_hotplug_monitor();
    void stop_usb_hotplug_monitor();
    void usb_hotplug_loop();

    /* ---- 调试页手动控制：UI_Debug_* → serial_debug 组帧 → McuLink 串口下发 ----
     * 振镜（以太网 TCP）后端逻辑预留，故不订阅、不下发。 */
    void handle_debug_axis_jog(const middleware::Message& msg);
    void handle_debug_axis_hold_stop(const middleware::Message& msg);
    void handle_debug_axis_set_zero(const middleware::Message& msg);
    void handle_debug_axis_home(const middleware::Message& msg);
    void handle_debug_device(const middleware::Message& msg);
    void handle_debug_powder_bucket(const middleware::Message& msg);
    void handle_debug_pusher(const middleware::Message& msg);
    void handle_debug_inner_door(const middleware::Message& msg);
    void handle_debug_return_motor(const middleware::Message& msg);
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
    void handle_debug_skip_outer_door_set(const middleware::Message& msg);
    void handle_debug_skip_outer_door_get(const middleware::Message& msg);
    void handle_debug_inner_door_busy_get(const middleware::Message& msg);
    void handle_outer_door_confirm_rsp(const middleware::Message& msg);
    void handle_plate_place_confirm_rsp(const middleware::Message& msg);
    void handle_debug_o2_calib_req(const middleware::Message& msg);
    void handle_debug_o2_calib_get(const middleware::Message& msg);
    void handle_debug_alarm_inject(const middleware::Message& msg);
    void handle_debug_first_layer_visual_check_set(const middleware::Message& msg);
    void handle_debug_first_layer_visual_check_get(const middleware::Message& msg);
    void handle_debug_optical_path_check_set(const middleware::Message& msg);
    void handle_debug_optical_path_check_get(const middleware::Message& msg);
    void handle_debug_plate_height_adj_set(const middleware::Message& msg);
    void handle_debug_plate_height_adj_get(const middleware::Message& msg);
    void handle_debug_hold_prep_z_set(const middleware::Message& msg);
    void handle_debug_hold_prep_z_get(const middleware::Message& msg);
    void handle_debug_plate_work_z_set(const middleware::Message& msg);
    void handle_debug_plate_work_z_get(const middleware::Message& msg);
    void publish_debug_plate_work_z_state();
    /** 测高已收到约 1mm 之后补 (target-1) mm。成功或无需移动返回 true。 */
    bool apply_plate_work_z_lift();
    void handle_debug_inner_door_occlusion_set(const middleware::Message& msg);
    void handle_debug_inner_door_occlusion_get(const middleware::Message& msg);
    void handle_debug_plate_present_set(const middleware::Message& msg);
    void handle_debug_plate_present_get(const middleware::Message& msg);
    void handle_debug_soc_led_set(const middleware::Message& msg);
    void handle_debug_soc_led_get(const middleware::Message& msg);
    void handle_debug_cabin_pm_valve_set(const middleware::Message& msg);
    void handle_debug_cabin_pm_valve_get(const middleware::Message& msg);
    void handle_debug_status_led_set(const middleware::Message& msg);
    void handle_debug_status_led_get(const middleware::Message& msg);
    void handle_debug_spi_pm25_lcd_set(const middleware::Message& msg);
    void handle_debug_spi_pm25_lcd_get(const middleware::Message& msg);
    void handle_visual_optical_path_point_rsp(const middleware::Message& msg);
    /** @brief 舱检挡门 Rsp：在 on_message 里直接投递，避免套件阻塞 worker 时死锁。 */
    void handle_visual_occlusion_rsp(const middleware::Message& msg);
    void handle_visual_plate_present_rsp(const middleware::Message& msg);
    void handle_height_calib_start_req(const middleware::Message& msg);
    void handle_laser_spot_measure_rsp(const middleware::Message& msg);
    void publish_debug_print_h264_record_state();
    void publish_debug_clean_record_state();
    void publish_debug_env_sim_state();
    void publish_debug_env_sim_powder_state();
    void publish_debug_skip_o2_gate_state();
    void publish_debug_skip_outer_door_state();
    /** @brief 推送内门套件忙闲（Manual 禁钮）；值变化才发。 */
    void publish_inner_door_busy(bool busy);
    /** @brief 外门补关的 Z 下降/回位：置灰 Manual 的 Top/Bottom 与 Z 升降复位。 */
    void set_manual_z_lock(bool lock);
    /** @brief 外门补关套件整段进行中（reserved[1]）；值变化才发。 */
    void set_clearance_kit_active(bool active);
    /** @brief 外门补关 Z 运动中收到 1204：立刻解锁，并让本次等待失败、不再自动回位。 */
    void note_z_axis_motion_fault();
    void publish_inner_door_busy_payload();
    void publish_debug_o2_calib_state();
    void publish_debug_first_layer_visual_check_state();
    void publish_debug_test_image_state();
    void publish_debug_galvo_point_test_state();
    void publish_debug_optical_path_check_state();
    void publish_debug_plate_height_adj_state();
    void publish_debug_hold_prep_z_state();
    void publish_debug_inner_door_occlusion_state();
    void publish_debug_plate_present_state();
    void publish_debug_soc_led_state();
    void publish_debug_cabin_pm_valve_state();
    void publish_debug_status_led_state();
    void publish_debug_spi_pm25_lcd_state();
    /** @brief 停止并回收点位测试线程；仅用于服务退出。 */
    void stop_galvo_point_test_worker();

    /* ---- 打印前光路检测（优先级高于首层检测） ---- */
    /** @brief 注册 OpticalPathCheck 的依赖回调；init 阶段调用一次。 */
    void configure_optical_path_check();
    void configure_plate_height();
    bool begin_plate_height_gate();
    void on_plate_height_done();
    void publish_plate_height_visual_session(bool active);
    /**
     * @brief 舱内灯光：SOC 六路 `lgt*` 全开/全关（与调试页 Cabin LED 同源）。
     *        仅光路/测高期间关灯；失败 abort 与检测通过后开灯。
     */
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
     * @brief 起印前外门闸门
     * @return true=已发确认 Req，调用方须 return 等 UI_Outer_Door_Confirm_Rsp；
     *         false=已关或跳过，可继续关内门套件
     */
    bool begin_outer_door_gate();
    /** @brief 外门闸通过后：关内门套件 → 光路/测高 → 起印后半段。 */
    void continue_print_after_outer_door_ready();
    /**
     * @brief 内门套件动 Z 的方式。挡门只决定是否跳过整套，不决定动不动 Z。
     *        Keep Z Position 打开时三种都不动 Z，门指令照发。
     */
    enum class InnerDoorZMotion : uint8_t {
        /** Manual / 调试点击：只开关门，不联动 Z。 */
        None = 0,
        /** 起印关/开内门：绝对到 −10 mm，不回原高度。 */
        PlateSafeStay,
        /** 外门开→关补关：先到 −100 mm，关门后再回到进入前高度。 */
        ClearanceReturn,
    };
    /**
     * @brief 内门开/关套件。不读基板视觉。进套件前读全开：挡门关时只信 bit1；
     *        开时一轮 Occlusion 后 OR，仅用于「已在目标位则跳过整套」。
     * @param opening true=开内门；false=关内门
     * @param blocking true=阻塞等锁；false=try_lock，忙则跳过
     * @param z_motion 见 InnerDoorZMotion
     * @return false 仅 Z 未到位、终止或 blocking 且套件失败时
     */
    bool run_inner_door_motion_kit(bool opening, bool blocking,
                                   InnerDoorZMotion z_motion);
    /**
     * @brief 关内门套件（Skip Outer Door 时直接成功）。
     * @param blocking true=起印路径阻塞等锁；false=边沿路径 try_lock，忙则跳过
     * @param z_motion 起印用 PlateSafeStay；外门开→关补关用 ClearanceReturn
     */
    bool run_inner_door_close_kit(bool blocking, InnerDoorZMotion z_motion);
    /** @brief 读 1ST bit1：valid 时 true=全开；false=非全开；无效 nullopt。 */
    [[nodiscard]] static std::optional<bool> read_inner_door_open_sensor();
    /**
     * @brief 决策点全开融合：开关关→只读 bit1；开→灯全开+舱检一轮 Occlusion 后 OR。
     *        同一决策只调用一次（套件锁前）；超时/unknown 退回只信 bit1。
     */
    [[nodiscard]] std::optional<bool> read_fully_open_fused();
    /** @brief 舱检会话 Start/Stop；载荷为 VisualCabinSessionPayload。 */
    void publish_cabin_visual_session(uint32_t session_id, bool active);
    /** @brief 起印路径：静止全开或未知才跑关套件；失败返回 false（不 abort）。 */
    bool ensure_inner_door_kit_for_print();
    /** @brief 外门开→关边沿：内门静止全开或未知才跑关套件；忙/发送中/预检则记 pending 补做。无板弹窗等待中跳过。 */
    void on_outer_door_closed_edge();
    /** @brief 套件/发送结束后：若仍记着外门已关，再尝试关内门。 */
    void try_pending_inner_close_after_outer();
    void publish_outer_door_confirm_req();
    void publish_plate_place_confirm_req();
    /**
     * @brief 每次起印：Z 绝对到位 −10 mm（公差 ±1；偏低也抬回）。与基板读码开关无关。
     * @return false=终止或到位失败（不在此 abort；由 early prep 回灌 worker 处理）
     */
    bool ensure_z_at_plate_safe();
    /** Keep Z 且打印会话中：禁止任何 Z 串口运动（测高/点动/Home/绝对到位一律丢弃）。 */
    [[nodiscard]] bool z_hold_blocks_motion() const noexcept;
    void send_z_goto_abs(float target_mm);
    /**
     * @brief 准备阶段用户终止：Z 绝对到 −10 mm，只发指令不等到位
     *        （TerminateDone 已先回 UI）。不是 Manual Top 的 1.00 mm。
     */
    void return_z_home_on_prep_terminate(const char* reason);
    /**
     * @brief Ready 前用户终止公共收尾：红光+舱灯+清会话+TerminateDone+Z −10。
     * @param red_reason apply_red_laser_mode 日志标签
     * @param z_reason return_z_home_on_prep_terminate 日志标签
     */
    void finish_prep_user_terminate(const char* red_reason, const char* z_reason);
    /**
     * @brief 启动起印前半段线程（Z/外门/内门/读码）；立即返回，结果经 pending 回灌。
     * @return 恒为 true（异步挂起；调用方须 return）
     */
    bool begin_early_print_prep(EarlyPrintPrepPhase phase);
    /** @brief 准备线程体内：按 phase 串行跑阻塞步骤，不碰 abort_print_start。 */
    EarlyPrintPrepResult run_early_print_prep(EarlyPrintPrepPhase phase);
    /** @brief 准备线程结果回灌 worker：续跑光路/测高，或 abort / 保持 UI 挂起。 */
    void on_early_print_prep_done();
    /**
     * @brief 关内门后读码闸门（可在准备线程调用；失败不 abort）。
     */
    EarlyPrintPrepResult begin_plate_present_gate();
    /** @brief 跑一轮 Plate Present；回包/超时。 */
    middleware::PlatePresentResult run_plate_present_round();
    /** @brief 无板 Confirm 后再检：外门已关 → 内门决策 → 再读码。 */
    void continue_print_after_plate_place_confirm();
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
    /** @brief 起印前规划本趟实际环境检查步数（跳过的不计）。 */
    void reset_print_prep_plan();
    /**
     * @brief 推送起印环境检查进度。进入新 step 时 step_index 自增；
     *        同一步内（光路点位）只更新 sub_*。Ready 把 index 拉满。
     */
    void publish_print_prep_progress(middleware::PrintPrepStep step,
                                     uint8_t sub_index = 0,
                                     uint8_t sub_total = 0);
    /** @brief 本趟是否会跑起印氧门（与 McuLink::soc_o2_print_gate_enabled 一致）。 */
    [[nodiscard]] bool print_prep_needs_oxygen() const;
    /** @brief 本趟是否会跑首层视觉（含参考图）。 */
    [[nodiscard]] bool print_prep_needs_first_layer() const;
    /**
     * @brief 外门/内门/光路/测高结束后进入开扫后半段。
     *        若后面还有氧门/首层则不发 Ready；否则发 Ready 再 continue。
     */
    void continue_after_static_print_prep();
    /** @brief 氧门通过后：有首层则推 FirstLayer，否则 Ready。 */
    void on_print_prep_oxygen_gate_passed();
    /** @brief 首层视觉通过后推 Ready（失败重铺粉不推）。 */
    void on_print_prep_first_layer_passed();
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
    void send_door_rsp_to(uint32_t target_id, middleware::MsgType rsp_type, bool ok);
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
    /** @brief 向 Xfr 推送 GLAVO_Print_State_Ntf（开始打印/打印结束状态边沿） */
    void publish_print_state_ntf(uint32_t mode);
    /** 云端 state：起印时按准备计划发第一个 printing 子状态。 */
    void cloud_begin_print(const std::string& path);
    /** 准备步或送层阶段切换云端子状态。结果页挂起后忽略。 */
    void cloud_set_substate(const char* substate);
    /** 送层中层号或进度跨过 5% 时刷新 job。 */
    void cloud_note_layer(uint32_t done_layers, uint32_t total_layers, int progress_pct);
    /** 正式送层之后的暂停/恢复。准备阶段不改子状态。 */
    void cloud_sync_run_pause(bool paused);
    /** 作业结束但结果页还在：finished / canceled / aborted。 */
    void cloud_hold_result(const char* substate);
    /** 结果页收掉，回到 idle ready 或 not_ready。 */
    void cloud_release_to_idle();
    void cloud_publish();
    [[nodiscard]] const char* cloud_first_substate() const;
    void handle_cloud_result_dismiss(const middleware::Message& msg);
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
    float pm25_clean_threshold_ = 50.0f;
    uint32_t timelapse_video_fps_ = 0; /**< 0=Media 默认；来自 cal.csv TIMELAPSE_VIDEO_FPS */
    uint32_t print_h264_bitrate_ = 0; /**< 0=Media 默认中等；来自 cal.csv PRINT_H264_BITRATE */
    std::thread clean_worker_;
    std::atomic<bool> clean_running_{false};
    std::thread pm25_sample_worker_;
    std::atomic<bool> pm25_sample_busy_{false};
    /** 真实开门：传感器轮询放独立线程，避免堵 worker 最长 GLAVO_DOOR_OPEN_TIMEOUT_MS。 */
    std::thread door_open_worker_;
    std::atomic<bool> door_open_busy_{false};
    /** 转运盒一键演示（Z→外门，确认后再开内门+转运串口），不堵 worker。 */
    std::thread transfer_demo_worker_;
    std::atomic<bool> transfer_demo_busy_{false};
    /**
     * 起印前半段（Z settle / 挡门视觉 / 读码视觉）独立线程，仿光路/测高：
     * 线程侧只置 early_print_prep_done_pending_，abort/续跑一律回 worker。
     */
    std::thread early_print_prep_thread_;
    std::atomic<bool> early_print_prep_active_{false};
    std::atomic<bool> early_print_prep_done_pending_{false};
    std::atomic<uint8_t> early_print_prep_result_{
        static_cast<uint8_t>(EarlyPrintPrepResult::Aborted)};
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
    bool optical_path_check_on_ = true; /**< 光路检测开关（仅本次运行，默认开；调试页 Skip Optical Path Chk 按下后关） */
    bool plate_height_adj_on_ = true; /**< 打印前测高调 Z（仅本次运行，默认开） */
    /** 测高收尾后的目标高度 mm。相对回零停点 1mm 再向上补差值。默认 1：不额外移动。 */
    float plate_work_z_mm_ = 1.0f;
    /** 准备阶段保持当前 Z（仅本次运行，默认关）。开则不起印降到 −10、内门前不降 Z、终止/失败收尾也不改 Z；基板测高仍动 Z。 */
    bool hold_prep_z_on_ = false;
    bool inner_door_occlusion_on_ = true; /**< 内门视觉挡门（仅本次运行，默认开；调试页 Skip Inner Door Occl 按下后关） */
    bool plate_present_on_ = true; /**< 基板在位读码（仅本次运行，默认开；调试页 Skip Plate Present 按下后关） */
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
    bool skip_outer_door_on_ = false; /**< 跳过外门检测+关内门套件（仅本次运行，默认关=要做） */
    bool outer_door_confirm_waiting_ = false; /**< 已发 Confirm_Req，等 UI Rsp */
    bool plate_place_confirm_waiting_ = false; /**< 无基板弹窗等 UI Rsp */
    std::mutex inner_door_seq_mtx_; /**< 内门开/关套件互斥（手动 / 起印 / 开→关边沿） */
    std::atomic<bool> inner_door_kit_busy_{false}; /**< 套件进行中；供 UI 禁钮 + 入队去重 */
    /** 外门补关 Z 正在下降到 −100 或回到下降前高度；Manual Z 键置灰。 */
    std::atomic<bool> manual_z_lock_{false};
    /** 外门补关 Z 运动中收到 1204：等待立刻失败，且不再自动回位重新上锁。 */
    std::atomic<bool> clearance_z_fault_{false};
    /** 外门补关套件整段（降 Z → 关内门 → 回 Z）进行中；推给 UI 盖监控页遮罩。
     *  与 manual_z_lock_ 不同：关门那 2 s 也保持 1。 */
    std::atomic<bool> clearance_kit_active_{false};
    /** Manual/调试开或关已入队未处理完；防止连点排队第二次套件 */
    std::atomic<bool> inner_door_cmd_queued_{false};
    /** 外门已关但当时不能跑关内门（套件忙 / 发送中 / 预检）；稍后补做 */
    std::atomic<bool> inner_door_close_pending_outer_{false};
    /** 用户终止：IPC 线程先置位，打断 worker 上阻塞的关内门套件 / wait_z */
    std::atomic<bool> print_prep_abort_{false};
    /* ---- 舱检挡门：worker 等 Rsp，IPC 回调直接 notify（勿入 worker 队列） ---- */
    // Visual orders both check types in the same cabin session stream.
    std::atomic<uint32_t> cabin_session_seq_{1};
    CabinVisualExchange occlusion_exchange_;
    std::atomic<uint32_t> occlusion_request_seq_{1};
    /* ---- 舱检读码：同挡门，Rsp 在 IPC 回调直投 ---- */
    CabinVisualExchange plate_present_exchange_;
    std::atomic<uint32_t> plate_present_request_seq_{1};
    /** 起印环境检查进度：本趟总步数 / 当前已进入步序号 / 上一次推送的 step */
    uint8_t prep_step_total_ = 0;
    uint8_t prep_step_index_ = 0;
    uint8_t prep_last_step_ = 0xff;
    bool ignore_powder_low_for_current_print_ = false; /**< 本次打印是否忽略缺粉检测（Cancel 后置位） */
    bool o2_auto_paused_ = false; /**< 本次因 SOC 氧过高由本层自动暂停（恢复时只清这类暂停） */
    /* 云端 state。mode 只在 idle/printing 之间切；子状态见 CloudStateNtfPayload。 */
    char cloud_mode_[16] = "idle";
    char cloud_substate_[32] = "not_ready";
    char cloud_job_id_[64] = {};
    bool cloud_job_valid_ = false;
    bool cloud_result_hold_ = false;
    /** UI 已离开打印占用，但终态还没到：到了就直接回 idle，不再挂结果页。 */
    bool cloud_dismiss_pending_ = false;
    uint32_t cloud_layer_ = 0;
    uint32_t cloud_total_layers_ = 0;
    int32_t cloud_progress_pct_ = 0;
    int32_t cloud_sent_pct_ = -1;
    uint32_t cloud_sent_layer_ = 0xffffffffu;
    uint64_t cloud_since_ms_ = 0;
    uint64_t cloud_started_ms_ = 0;
    std::string cloud_fp_;
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
