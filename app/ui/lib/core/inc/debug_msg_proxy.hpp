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
 * register_*：APP_GLAVO → UI 状态回读回调（按模块拆分，见 upc_contract.hpp）。
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
    void send_galvo_move(middleware::DebugGalvoAxis axis, int dir, float step_mm);
    void send_galvo_home();
    void send_galvo_red_light(bool on);
    void send_galvo_param_save(float scale, float step_mm);
    void send_galvo_calib();
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

    template <typename T>
    void publish_pod(middleware::MsgType type, const T& pod) {
        middleware::Message msg(type, middleware::ModuleId::UI,
                                middleware::ModuleId::APP_GLAVO);
        msg.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&pod), sizeof(T));
        middleware::MsgPort::instance().publish_async(msg);
    }

    void publish_empty(middleware::MsgType type) {
        middleware::Message msg(type, middleware::ModuleId::UI,
                                middleware::ModuleId::APP_GLAVO);
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
};

} // namespace ui

#endif
