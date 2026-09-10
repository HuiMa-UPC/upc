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

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Print_H264_Record_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if (print_h264_record_cb_ && read_inline_pod(msg, p)) {
                print_h264_record_cb_(p.on != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Clean_Record_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if (clean_record_cb_ && read_inline_pod(msg, p)) {
                clean_record_cb_(p.on != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Env_Sim_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if (env_sim_cb_ && read_inline_pod(msg, p)) {
                env_sim_cb_(p.on != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Env_Sim_Powder_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if (env_sim_powder_cb_ && read_inline_pod(msg, p)) {
                env_sim_powder_cb_(p.on != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_FirstLayer_Visual_Check_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if(first_layer_visual_check_cb_ && read_inline_pod(msg, p)) {
                first_layer_visual_check_cb_(p.on != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_OpticalPath_Check_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if(optical_path_check_cb_ && read_inline_pod(msg, p)) {
                optical_path_check_cb_(p.on != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_TestImage_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if(!read_inline_pod(msg, p)) {
                return;
            }
            test_image_active_ = (p.on != 0);
            if(test_image_cb_) {
                test_image_cb_(test_image_active_);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Galvo_PointTest_Ntf,
        middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if(!read_inline_pod(msg, p)) {
                return;
            }
            galvo_point_test_active_ = (p.on != 0);
            if(galvo_point_test_cb_) {
                galvo_point_test_cb_(galvo_point_test_active_);
            }
        }));

    /* 来自 APP_MEDIA：开关的实际状态（开启失败时为 0，用于把 UI 开关弹回）。 */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Media_Local_Rtsp_State_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DebugOnOffPayload p{};
            if(local_rtsp_cb_ && read_inline_pod(msg, p)) {
                local_rtsp_cb_(p.on != 0);
            }
        }));

    /* Confirm Print 互斥依赖此状态；进调试页前也可能需要。 */
    request_test_image_state();
    request_galvo_point_test_state();
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
    publish_pod(middleware::MsgType::UI_Debug_Axis_Jog_Ntf, p);
}

void DebugMsgProxy::send_axis_hold_stop(middleware::DebugAxis axis)
{
    middleware::DebugAxisPayload p{};
    p.axis = static_cast<uint8_t>(axis);
    publish_pod(middleware::MsgType::UI_Debug_Axis_Hold_Stop_Ntf, p);
}

void DebugMsgProxy::send_axis_set_zero(middleware::DebugAxis axis)
{
    middleware::DebugAxisPayload p{};
    p.axis = static_cast<uint8_t>(axis);
    publish_pod(middleware::MsgType::UI_Debug_Axis_SetZero_Ntf, p);
}

void DebugMsgProxy::send_axis_home(middleware::DebugAxis axis)
{
    middleware::DebugAxisPayload p{};
    p.axis = static_cast<uint8_t>(axis);
    publish_pod(middleware::MsgType::UI_Debug_Axis_Home_Ntf, p);
}

void DebugMsgProxy::send_galvo_red_light(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Galvo_RedLight_Ntf, p);
}

void DebugMsgProxy::send_galvo_red_always()
{
    publish_empty(middleware::MsgType::UI_Debug_Galvo_RedAlways_Ntf);
}

void DebugMsgProxy::send_galvo_param_save(float scale, float step_mm,
                                          float center_offset_x,
                                          float center_offset_y)
{
    middleware::DebugGalvoParamPayload p{};
    p.scale = scale;
    p.step_mm = step_mm;
    p.center_offset_x = center_offset_x;
    p.center_offset_y = center_offset_y;
    publish_pod(middleware::MsgType::UI_Debug_Galvo_Param_Save_Ntf, p);
}

void DebugMsgProxy::send_galvo_calib()
{
    publish_empty(middleware::MsgType::UI_Debug_Galvo_Calib_Ntf);
}

void DebugMsgProxy::start_galvo_point_test()
{
    publish_empty(middleware::MsgType::UI_Debug_Galvo_PointTest_Start_Ntf);
}

void DebugMsgProxy::request_galvo_point_test_state()
{
    publish_empty(middleware::MsgType::UI_Debug_Galvo_PointTest_Get_Ntf);
}

void DebugMsgProxy::send_device(middleware::DebugDeviceTarget target, bool on)
{
    middleware::DebugDevicePayload p{};
    p.target = static_cast<uint8_t>(target);
    p.on     = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Device_Ntf, p);
}

/* ---- Tab2 ---- */
void DebugMsgProxy::send_powder_bucket(uint8_t bucket_id, bool on)
{
    middleware::DebugBucketPayload p{};
    p.bucket_id = bucket_id;
    p.on        = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Powder_Bucket_Ntf, p);
}

void DebugMsgProxy::send_sieve(middleware::DebugMotorAction action)
{
    middleware::DebugMotorPayload p{};
    p.action = static_cast<uint8_t>(action);
    publish_pod(middleware::MsgType::UI_Debug_Pusher_Ntf, p);
}

void DebugMsgProxy::send_inner_door(middleware::DebugMotorAction action)
{
    middleware::DebugMotorPayload p{};
    p.action = static_cast<uint8_t>(action);
    publish_pod(middleware::MsgType::UI_Debug_Recycle_Motor_Ntf, p);
}

void DebugMsgProxy::send_fan(bool on)
{
    middleware::DebugFanPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Fan_Ntf, p);
}

void DebugMsgProxy::send_fan_duty(float duty)
{
    middleware::DebugFanPayload p{};
    p.duty = duty;
    publish_pod(middleware::MsgType::UI_Debug_Fan_Duty_Ntf, p);
}

void DebugMsgProxy::send_switch(middleware::DebugSwitchTarget target, bool on)
{
    middleware::DebugSwitchPayload p{};
    p.target = static_cast<uint8_t>(target);
    p.on     = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Switch_Ntf, p);
}

void DebugMsgProxy::send_clean_test_start()
{
    publish_empty(middleware::MsgType::UI_Debug_CleanTest_Start_Ntf);
}

void DebugMsgProxy::send_clean_test_close()
{
    publish_empty(middleware::MsgType::UI_Debug_CleanTest_Close_Ntf);
}

void DebugMsgProxy::send_print_layer_param(uint16_t layer_um, uint16_t balance_x1000)
{
    middleware::DebugPrintParamPayload p{};
    p.layer_um      = layer_um;
    p.balance_x1000 = balance_x1000;
    publish_pod(middleware::MsgType::UI_Debug_PrintParam_Set_Ntf, p);
}

void DebugMsgProxy::send_laser_power_set(uint8_t percent)
{
    middleware::DebugLaserPowerPayload p{};
    p.percent = percent > 100 ? 100 : percent;
    publish_pod(middleware::MsgType::UI_Debug_Laser_Power_Set_Ntf, p);
}

void DebugMsgProxy::send_laser_power_reset()
{
    publish_empty(middleware::MsgType::UI_Debug_Laser_Power_Reset_Ntf);
}

void DebugMsgProxy::send_laser_alarm_reset()
{
    publish_empty(middleware::MsgType::UI_Debug_Laser_AlarmReset_Ntf);
}

void DebugMsgProxy::send_print_h264_record_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Print_H264_Record_Set_Ntf, p);
}

void DebugMsgProxy::request_print_h264_record_state()
{
    publish_empty(middleware::MsgType::UI_Debug_Print_H264_Record_Get_Ntf);
}

void DebugMsgProxy::send_clean_record_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Clean_Record_Set_Ntf, p);
}

void DebugMsgProxy::request_clean_record_state()
{
    publish_empty(middleware::MsgType::UI_Debug_Clean_Record_Get_Ntf);
}

void DebugMsgProxy::send_env_sim_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Env_Sim_Set_Ntf, p);
}

void DebugMsgProxy::request_env_sim_state()
{
    publish_empty(middleware::MsgType::UI_Debug_Env_Sim_Get_Ntf);
}

void DebugMsgProxy::send_env_sim_powder_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Env_Sim_Powder_Set_Ntf, p);
}

void DebugMsgProxy::request_env_sim_powder_state()
{
    publish_empty(middleware::MsgType::UI_Debug_Env_Sim_Powder_Get_Ntf);
}

void DebugMsgProxy::send_first_layer_visual_check_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_FirstLayer_Visual_Check_Set_Ntf, p);
}

void DebugMsgProxy::request_first_layer_visual_check_state()
{
    publish_empty(middleware::MsgType::UI_Debug_FirstLayer_Visual_Check_Get_Ntf);
}

void DebugMsgProxy::send_optical_path_check_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_OpticalPath_Check_Set_Ntf, p);
}

void DebugMsgProxy::request_optical_path_check_state()
{
    publish_empty(middleware::MsgType::UI_Debug_OpticalPath_Check_Get_Ntf);
}

void DebugMsgProxy::send_test_image_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_TestImage_Set_Ntf, p);
}

void DebugMsgProxy::request_test_image_state()
{
    publish_empty(middleware::MsgType::UI_Debug_TestImage_Get_Ntf);
}

/* 本地 RTSP Server 由 Media 进程持有，直发 APP_MEDIA 而非 APP_GLAVO。 */
void DebugMsgProxy::send_local_rtsp_set(bool on)
{
    middleware::DebugOnOffPayload p{};
    p.on = on ? 1 : 0;
    publish_pod(middleware::MsgType::UI_Debug_Local_Rtsp_Set_Ntf, p,
                middleware::ModuleId::APP_MEDIA);
}

void DebugMsgProxy::request_local_rtsp_state()
{
    publish_empty(middleware::MsgType::UI_Debug_Local_Rtsp_Get_Ntf,
                  middleware::ModuleId::APP_MEDIA);
}

/* ---- 回读注册 ---- */
void DebugMsgProxy::register_sensor(DebugSensorCb cb) { sensor_cb_ = std::move(cb); }
void DebugMsgProxy::register_axis(DebugAxisCb cb)     { axis_cb_   = std::move(cb); }
void DebugMsgProxy::register_device(DebugDeviceCb cb) { device_cb_ = std::move(cb); }
void DebugMsgProxy::register_galvo(DebugGalvoCb cb)   { galvo_cb_  = std::move(cb); }
void DebugMsgProxy::register_laser(DebugLaserCb cb)   { laser_cb_  = std::move(cb); }
void DebugMsgProxy::register_print_h264_record(DebugPrintH264RecordCb cb)
{
    print_h264_record_cb_ = std::move(cb);
}

void DebugMsgProxy::register_clean_record(DebugCleanRecordCb cb)
{
    clean_record_cb_ = std::move(cb);
}

void DebugMsgProxy::register_env_sim(DebugEnvSimCb cb)
{
    env_sim_cb_ = std::move(cb);
}

void DebugMsgProxy::register_env_sim_powder(DebugEnvSimPowderCb cb)
{
    env_sim_powder_cb_ = std::move(cb);
}

void DebugMsgProxy::register_first_layer_visual_check(DebugFirstLayerVisualCheckCb cb)
{
    first_layer_visual_check_cb_ = std::move(cb);
}

void DebugMsgProxy::register_optical_path_check(DebugOpticalPathCheckCb cb)
{
    optical_path_check_cb_ = std::move(cb);
}

void DebugMsgProxy::register_test_image(DebugTestImageCb cb)
{
    test_image_cb_ = std::move(cb);
}

void DebugMsgProxy::register_galvo_point_test(DebugGalvoPointTestCb cb)
{
    galvo_point_test_cb_ = std::move(cb);
}

void DebugMsgProxy::register_local_rtsp(DebugLocalRtspCb cb)
{
    local_rtsp_cb_ = std::move(cb);
}

} // namespace ui
