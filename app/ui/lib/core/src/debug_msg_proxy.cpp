#include "debug_msg_proxy.hpp"

namespace ui {

DebugMsgProxy& DebugMsgProxy::instance()
{
    static DebugMsgProxy inst;
    return inst;
}

void DebugMsgProxy::init()
{
    auto& bus = middleware::MsgPort::instance();

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Sensor_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugSensorPayload p{};
            if (sensor_cb_ && read_inline_pod(msg, p)) {
                sensor_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Axis_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugAxisStatePayload p{};
            if (axis_cb_ && read_inline_pod(msg, p)) {
                axis_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Device_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugDeviceStatePayload p{};
            if (device_cb_ && read_inline_pod(msg, p)) {
                device_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Galvo_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugGalvoStatePayload p{};
            if (galvo_cb_ && read_inline_pod(msg, p)) {
                galvo_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Laser_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugLaserStatePayload p{};
            if (laser_cb_ && read_inline_pod(msg, p)) {
                laser_cb_(p);
            }
        }));
}

/* ---- Tab1 ---- */
void DebugMsgProxy::send_axis_jog(middleware::DebugAxis axis,
                                  middleware::DebugJogMode mode,
                                  int dir, float value)
{
    middleware::DebugAxisJogPayload p{};
    p.axis  = static_cast<uint8_t>(axis);
    p.mode  = static_cast<uint8_t>(mode);
    p.dir   = static_cast<int8_t>(dir < 0 ? -1 : 1);
    p.value = value;
    publish_pod(middleware::MsgType::UI_Debug_Axis_Jog_Req, p);
}

void DebugMsgProxy::send_axis_hold_stop(middleware::DebugAxis axis)
{
    middleware::DebugAxisPayload p{};
    p.axis = static_cast<uint8_t>(axis);
    publish_pod(middleware::MsgType::UI_Debug_Axis_Hold_Stop_Req, p);
}

void DebugMsgProxy::send_axis_set_zero(middleware::DebugAxis axis)
{
    middleware::DebugAxisPayload p{};
    p.axis = static_cast<uint8_t>(axis);
    publish_pod(middleware::MsgType::UI_Debug_Axis_SetZero_Req, p);
}

void DebugMsgProxy::send_axis_home(middleware::DebugAxis axis)
{
    middleware::DebugAxisPayload p{};
    p.axis = static_cast<uint8_t>(axis);
    publish_pod(middleware::MsgType::UI_Debug_Axis_Home_Req, p);
}

void DebugMsgProxy::send_galvo_move(middleware::DebugGalvoAxis axis,
                                    int dir, float step_mm)
{
    middleware::DebugGalvoMovePayload p{};
    p.axis    = static_cast<uint8_t>(axis);
    p.dir     = static_cast<int8_t>(dir < 0 ? -1 : 1);
    p.step_mm = step_mm;
    publish_pod(middleware::MsgType::UI_Debug_Galvo_Move_Req, p);
}

void DebugMsgProxy::send_galvo_home()
{
    publish_empty(middleware::MsgType::UI_Debug_Galvo_Home_Req);
}

void DebugMsgProxy::send_galvo_red_light(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Galvo_RedLight_Req, p);
}

void DebugMsgProxy::send_galvo_param_save(float scale, float step_mm)
{
    middleware::DebugGalvoParamPayload p{scale, step_mm};
    publish_pod(middleware::MsgType::UI_Debug_Galvo_Param_Save_Req, p);
}

void DebugMsgProxy::send_galvo_calib()
{
    publish_empty(middleware::MsgType::UI_Debug_Galvo_Calib_Req);
}

void DebugMsgProxy::send_device(middleware::DebugDeviceTarget target, bool on)
{
    middleware::DebugDevicePayload p{};
    p.target = static_cast<uint8_t>(target);
    p.on     = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Device_Req, p);
}

/* ---- Tab2 ---- */
void DebugMsgProxy::send_powder_bucket(uint8_t bucket_id, bool on)
{
    middleware::DebugBucketPayload p{};
    p.bucket_id = bucket_id;
    p.on        = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Powder_Bucket_Req, p);
}

void DebugMsgProxy::send_pusher(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Pusher_Req, p);
}

void DebugMsgProxy::send_recycle_motor(middleware::DebugMotorAction action)
{
    middleware::DebugMotorPayload p{};
    p.action = static_cast<uint8_t>(action);
    publish_pod(middleware::MsgType::UI_Debug_Recycle_Motor_Req, p);
}

void DebugMsgProxy::send_fan(bool on)
{
    middleware::DebugFanPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Fan_Req, p);
}

void DebugMsgProxy::send_fan_duty(float duty)
{
    middleware::DebugFanPayload p{};
    p.duty = duty;
    publish_pod(middleware::MsgType::UI_Debug_Fan_Duty_Req, p);
}

void DebugMsgProxy::send_switch(middleware::DebugSwitchTarget target, bool on)
{
    middleware::DebugSwitchPayload p{};
    p.target = static_cast<uint8_t>(target);
    p.on     = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Switch_Req, p);
}

void DebugMsgProxy::send_clean_test_start()
{
    publish_empty(middleware::MsgType::UI_Debug_CleanTest_Start_Req);
}

void DebugMsgProxy::send_print_layer_param(uint16_t layer_um, uint16_t balance_x1000)
{
    middleware::DebugPrintParamPayload p{};
    p.layer_um      = layer_um;
    p.balance_x1000 = balance_x1000;
    publish_pod(middleware::MsgType::UI_Debug_PrintParam_Set_Req, p);
}

void DebugMsgProxy::send_laser_power_set(uint8_t percent)
{
    middleware::DebugLaserPowerPayload p{};
    p.percent = percent > 100 ? 100 : percent;
    publish_pod(middleware::MsgType::UI_Debug_Laser_Power_Set_Req, p);
}

void DebugMsgProxy::send_laser_power_reset()
{
    publish_empty(middleware::MsgType::UI_Debug_Laser_Power_Reset_Req);
}

void DebugMsgProxy::send_laser_alarm_reset()
{
    publish_empty(middleware::MsgType::UI_Debug_Laser_AlarmReset_Req);
}

/* ---- 回读注册 ---- */
void DebugMsgProxy::register_sensor(DebugSensorCb cb) { sensor_cb_ = std::move(cb); }
void DebugMsgProxy::register_axis(DebugAxisCb cb)     { axis_cb_   = std::move(cb); }
void DebugMsgProxy::register_device(DebugDeviceCb cb) { device_cb_ = std::move(cb); }
void DebugMsgProxy::register_galvo(DebugGalvoCb cb)   { galvo_cb_  = std::move(cb); }
void DebugMsgProxy::register_laser(DebugLaserCb cb)   { laser_cb_  = std::move(cb); }

} // namespace ui
