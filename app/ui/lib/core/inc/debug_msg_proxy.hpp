#ifndef DEBUG_MSG_PROXY_HPP
#define DEBUG_MSG_PROXY_HPP

#include "msg_port.hpp"
#include <functional>
#include <vector>
#include <cstring>

namespace ui {

/**
 * @file debug_msg_proxy.hpp
 * @brief 调试页（手动控制/标定）与消息总线之间的门面，独立于 UiMsgProxy。
 *
 * send_*：UI → APP_GLAVO（命令）。后端未实现时为无害发送。
 *         少数开关直连其它进程（如本地 RTSP Server → APP_MEDIA），由 target 参数指定。
 * register_*：后端 → UI 状态回读回调（按模块拆分，见 upc_contract.hpp）。
 */

using DebugSensorCb  = std::function<void(const middleware::DebugSensorPayload&)>;
using DebugAxisCb    = std::function<void(const middleware::DebugAxisStatePayload&)>;
using DebugDeviceCb  = std::function<void(const middleware::DebugDeviceStatePayload&)>;
using DebugGalvoCb   = std::function<void(const middleware::DebugGalvoStatePayload&)>;
using DebugLaserCb   = std::function<void(const middleware::DebugLaserStatePayload&)>;
using DebugPrintH264RecordCb = std::function<void(bool)>;
using DebugCleanRecordCb = std::function<void(bool)>;
using DebugEnvSimCb = std::function<void(bool)>;
using DebugEnvSimPowderCb = std::function<void(bool)>;
using DebugSkipO2GateCb = std::function<void(bool)>;
using DebugO2CalibCb =
    std::function<void(const middleware::DebugO2CalibRspPayload&)>;
using DebugFirstLayerVisualCheckCb = std::function<void(bool)>;
using DebugOpticalPathCheckCb = std::function<void(bool)>;
using DebugPlateHeightAdjCb = std::function<void(bool)>;
using DebugSocLedCb = std::function<void(uint8_t mask)>;
using DebugCabinPmValveCb = std::function<void(bool)>;
using DebugStatusLedCb =
    std::function<void(const middleware::DebugStatusLedPayload&)>;
using DebugFanStateCb =
    std::function<void(const middleware::DebugFanStatePayload&)>;
using DebugTestImageCb = std::function<void(bool)>;
using DebugGalvoPointTestCb = std::function<void(bool)>;
using DebugLocalRtspCb = std::function<void(bool)>;
using DebugHeightCalibProgressCb =
    std::function<void(const middleware::HeightCalibProgressPayload&)>;

class DebugMsgProxy {
public:
    static DebugMsgProxy& instance();

    void init();

    /* ---- Tab1 移动控制 ---- */
    void send_axis_jog(middleware::DebugAxis axis, middleware::DebugJogMode mode,
                       int dir, float value);
    void send_axis_hold_stop(middleware::DebugAxis axis);
    void send_axis_set_zero(middleware::DebugAxis axis);
    void send_axis_home(middleware::DebugAxis axis);
    void send_galvo_red_light(bool on);
    /** 红光常亮：TCP 0x0A DATA[3]=3（点发，不回读） */
    void send_galvo_red_always();
    void send_galvo_param_save(float scale, float step_mm,
                               float center_offset_x, float center_offset_y);
    void send_galvo_calib();
    /** @brief 振镜绝对坐标移动：发 UI_Debug_Galvo_AbsMove_Ntf。
     *  坐标系与切片一致，原点(0,0)为振镜中心，单位 mm；speed<=0 用默认。 */
    void send_galvo_abs_move(float x_mm, float y_mm,
                             float speed_mm_min, int laser_on);
    /** Other 页：中心点 + 四角红光驻留测试（一次性，不提供取消）。 */
    void start_galvo_point_test();
    void request_galvo_point_test_state();
    void send_device(middleware::DebugDeviceTarget target, bool on);

    /* ---- Tab2 其他控制 ---- */
    void send_powder_bucket(uint8_t bucket_id, bool on);
    void send_sieve(middleware::DebugMotorAction action);   /**< 筛粉电机：停止/运行/反转倒渣 */
    void send_inner_door(middleware::DebugMotorAction action);   /**< 内门：停止/开/关 */
    void send_fan(bool on);               /**< 风扇开/关 */
    void send_fan_duty(float duty);       /**< 仅设占空比 */
    void send_switch(middleware::DebugSwitchTarget target, bool on);
    void send_clean_test_start();
    void send_clean_test_close();
    void send_print_layer_param(uint16_t layer_um, uint16_t balance_x1000);

    /* ---- Laser 激光器 ---- */
    void send_laser_power_set(uint8_t percent);
    void send_laser_power_reset();
    void send_laser_alarm_reset();
    void send_print_h264_record_set(bool on);
    void request_print_h264_record_state();
    void send_clean_record_set(bool on);
    void request_clean_record_state();
    void send_env_sim_set(bool on);
    void request_env_sim_state();
    void send_env_sim_powder_set(bool on);
    void request_env_sim_powder_state();
    void send_skip_o2_gate_set(bool on);
    void request_skip_o2_gate_state();
    /** 空气单点氧标定；ref_o2_percent≤0 用后端默认 20.9 */
    void send_o2_calib(float ref_o2_percent = 0.f);
    void request_o2_calib_state();
    void send_first_layer_visual_check_set(bool on);
    void request_first_layer_visual_check_state();
    void send_optical_path_check_set(bool on);
    void request_optical_path_check_state();
    void send_plate_height_adj_set(bool on);
    void request_plate_height_adj_state();
    /** 六路舱灯 / 舱内 PM 阀：sysfs 直控，不看 MCU 版本 */
    void send_soc_led_set(uint8_t channel, bool on);
    void request_soc_led_state();
    void send_cabin_pm_valve_set(bool on);
    void request_cabin_pm_valve_state();
    /** SPI 灯板验色：force=false 释放接管回自动；r/g/b 仅 color=Raw 时生效 */
    void send_status_led_set(bool force, uint8_t color,
                             uint8_t r = 0, uint8_t g = 0, uint8_t b = 0);
    void request_status_led_state();
    void send_height_calib_start(float start_mm, float end_mm, float step_mm,
                                 const char* lut_path);
    /** 调试页 Test image：红光循环打印 /userdata/test/test.gcode */
    void send_test_image_set(bool on);
    void request_test_image_state();
    /** 本地局域网 RTSP Server 开关：直发 APP_MEDIA，实际状态以其回报为准。 */
    void send_local_rtsp_set(bool on);
    void request_local_rtsp_state();

    /* ---- 状态回读 ---- */
    void register_sensor(DebugSensorCb cb);
    void register_axis(DebugAxisCb cb);
    void register_device(DebugDeviceCb cb);
    void register_galvo(DebugGalvoCb cb);
    void register_laser(DebugLaserCb cb);
    void register_print_h264_record(DebugPrintH264RecordCb cb);
    void register_clean_record(DebugCleanRecordCb cb);
    void register_env_sim(DebugEnvSimCb cb);
    void register_env_sim_powder(DebugEnvSimPowderCb cb);
    void register_skip_o2_gate(DebugSkipO2GateCb cb);
    void register_o2_calib(DebugO2CalibCb cb);
    void register_first_layer_visual_check(DebugFirstLayerVisualCheckCb cb);
    void register_optical_path_check(DebugOpticalPathCheckCb cb);
    void register_plate_height_adj(DebugPlateHeightAdjCb cb);
    void register_soc_led(DebugSocLedCb cb);
    void register_cabin_pm_valve(DebugCabinPmValveCb cb);
    void register_status_led(DebugStatusLedCb cb);
    /** 风机实测状态：MCU V4 EXT 帧周期上报，无查询语义（不配 request_） */
    void register_fan_state(DebugFanStateCb cb);
    void register_test_image(DebugTestImageCb cb);
    void register_galvo_point_test(DebugGalvoPointTestCb cb);
    void register_local_rtsp(DebugLocalRtspCb cb);
    void register_height_calib_progress(DebugHeightCalibProgressCb cb);

    /** Confirm Print 互斥：Glavo 回报的测试图实际运行态 */
    [[nodiscard]] bool test_image_active() const noexcept { return test_image_active_; }
    /** Confirm Print 互斥：Glavo 回报的红光点位测试运行态 */
    [[nodiscard]] bool galvo_point_test_active() const noexcept {
        return galvo_point_test_active_;
    }

private:
    DebugMsgProxy() = default;

    template <typename T>
    static bool read_inline_pod(const middleware::Message& msg, T& out) {
        if (msg.payload.type != middleware::MsgPayload::Type::Inline ||
            msg.payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(T));
        return true;
    }

    /** 默认目标 APP_GLAVO；直连其它进程的开关显式传 target。 */
    template <typename T>
    void publish_pod(middleware::MsgType type, const T& pod,
                     middleware::ModuleId target = middleware::ModuleId::APP_GLAVO) {
        middleware::Message msg(type, middleware::ModuleId::UI, target);
        msg.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&pod), sizeof(T));
        middleware::MsgPort::instance().publish_async(msg);
    }

    void publish_empty(middleware::MsgType type,
                       middleware::ModuleId target = middleware::ModuleId::APP_GLAVO) {
        middleware::Message msg(type, middleware::ModuleId::UI, target);
        middleware::MsgPort::instance().publish_async(msg);
    }

    std::vector<middleware::Subscription> subs_;
    DebugSensorCb sensor_cb_;
    DebugAxisCb   axis_cb_;
    DebugDeviceCb device_cb_;
    DebugGalvoCb  galvo_cb_;
    DebugLaserCb  laser_cb_;
    DebugPrintH264RecordCb print_h264_record_cb_;
    DebugCleanRecordCb clean_record_cb_;
    DebugEnvSimCb env_sim_cb_;
    DebugEnvSimPowderCb env_sim_powder_cb_;
    DebugSkipO2GateCb skip_o2_gate_cb_;
    DebugO2CalibCb o2_calib_cb_;
    DebugFirstLayerVisualCheckCb first_layer_visual_check_cb_;
    DebugOpticalPathCheckCb optical_path_check_cb_;
    DebugPlateHeightAdjCb plate_height_adj_cb_;
    DebugSocLedCb soc_led_cb_;
    DebugCabinPmValveCb cabin_pm_valve_cb_;
    DebugStatusLedCb status_led_cb_;
    DebugFanStateCb fan_state_cb_;
    DebugTestImageCb test_image_cb_;
    DebugGalvoPointTestCb galvo_point_test_cb_;
    DebugLocalRtspCb local_rtsp_cb_;
    DebugHeightCalibProgressCb height_calib_progress_cb_;
    bool test_image_active_ = false;
    bool galvo_point_test_active_ = false;
};

} // namespace ui

#endif
