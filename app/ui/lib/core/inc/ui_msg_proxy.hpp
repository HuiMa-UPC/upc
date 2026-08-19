#ifndef UI_MSG_PROXY_HPP
#define UI_MSG_PROXY_HPP

#include "msg_port.hpp"
#include <functional>
#include <vector>
#include <cstring>

namespace ui {

/**
 * @file ui_msg_proxy.hpp
 * @brief UI 与消息总线之间的唯一门面（Facade，经 MsgPort）
 */

using EnvUpdateCb      = std::function<void(const middleware::EnvSnapshotPayload&)>;
using Pm25ResultCb     = std::function<void(const middleware::Pm25ResultPayload&)>;
using DoorResultCb     = std::function<void(const middleware::DoorResultPayload&)>;
using PurifyProgressCb = std::function<void(int progress)>;
using PurifyDoneCb     = std::function<void()>;
using CleanDoneCb      = std::function<void()>;
using CleanStopDoneCb  = std::function<void()>;
using CleanTestStartRspCb = std::function<void(bool accepted)>;
using CleanTestDoneCb = std::function<void(const middleware::CleanTestDonePayload&)>;
using CleanTestCloseRspCb = std::function<void()>;
using TransportResultCb = std::function<void(uint8_t result)>;
using FsDeleteRspCb     = std::function<void(const middleware::FsDeleteRspPayload&)>;
using FsDirCb          = std::function<void(const middleware::FileEntryPayload* files,
                                            uint32_t count)>;
using PrintProgressCb  = std::function<void(const middleware::PrintProgressPayload&)>;
using PrintTerminateDoneCb = std::function<void()>;
using PrintPauseDoneCb = std::function<void(bool paused)>;
using PrintPowderLowCb = std::function<void()>;
using SupplyStatusCb   = std::function<void(const middleware::SupplyStatusPayload&)>;
using PrintPrepareDoneCb = std::function<void(bool ok)>;
using McuOtaStatusCb   = std::function<void(const middleware::McuOtaStatusPayload&)>;
/* 网络 / WiFi（前端预留，待后端对齐） */
using WifiStateCb      = std::function<void(const middleware::WifiStatePayload&)>;
using WifiListCb       = std::function<void(const middleware::WifiEntryPayload* items,
                                            uint32_t count)>;
using WifiConnectResultCb = std::function<void(bool ok, const char* ssid)>;
using XfrPairPinCb     = std::function<void(const middleware::XfrPairPinPayload&)>;
using XfrFileReceivedCb = std::function<void(const middleware::XfrFileReceivedPayload&)>;
using XfrTransferProgressCb = std::function<void(const middleware::XfrTransferProgressPayload&)>;
using UsbHotplugCb     = std::function<void(const middleware::UsbHotplugPayload&)>;
using McuVersionCb     = std::function<void(const middleware::McuVersionPayload&)>;
using GalvoVersionCb   = std::function<void(const middleware::GalvoVersionPayload&)>;
using OtaNotifyCb      = std::function<void(const middleware::OtaNotifyPayload&)>;
using OtaProgressCb    = std::function<void(const middleware::OtaProgressPayload&)>;
using DeviceEventCb    = std::function<void(const middleware::DeviceEventPayload&)>;
using AlarmReportCb    = std::function<void(const middleware::AlarmReportPayload&)>;
using AlarmSnapshotCb  = std::function<void(const middleware::AlarmSnapshotPayload&)>;

class UiMsgProxy {
public:
    static UiMsgProxy& instance();

    void init();

    void register_env_update(EnvUpdateCb cb);

    void send_pm25_check();
    void register_pm25_result(Pm25ResultCb cb);

    void send_open_door();
    void register_open_door_result(DoorResultCb cb);

    void send_close_door_check();
    void register_close_door_result(DoorResultCb cb);

    void send_purify();
    void register_purify_progress(PurifyProgressCb cb);
    void register_purify_done(PurifyDoneCb cb);

    /** 开门前清粉（PM2.5≥50 触发）。完成回执经 register_clean_start_result。 */
    void send_clean_start();
    void register_clean_start_result(CleanDoneCb cb);
    /** 清粉中断（按清粉页 X）：请求后端停风机，回执经 register_clean_stop_result。 */
    void send_clean_stop();
    void register_clean_stop_result(CleanStopDoneCb cb);
    /** 打印结果页 Purify：触发 clean_test_start（新增契约，不复用 debug 消息） */
    void send_clean_test_start(const char* job_name, uint32_t width, uint32_t height, uint32_t fps);
    void register_clean_test_start_rsp(CleanTestStartRspCb cb);
    void register_clean_test_done(CleanTestDoneCb cb);
    /** 返回首页时强制 clean_test_close。 */
    void send_clean_test_close();
    /** 结果页退出后触发媒体统一合成 timelapse.mp4。 */
    void send_print_video_finalize(const char* frames_dir);
    void register_clean_test_close_rsp(CleanTestCloseRspCb cb);

    /** 转运盒 Ready：发自动转运请求；MCU TLV 0x04 结果经 register_transport_result。 */
    void send_transport_request();
    void register_transport_result(TransportResultCb cb);

    /** 列目录：当前 UI 仅对 USB 调用；Local 由 control 直读 config.json */
    void send_fs_dir_open(middleware::PrintFileSource source);

    void send_supply_status();
    void register_supply_status(SupplyStatusCb cb);

    void send_print_prepare(uint32_t file_index, middleware::PrintFileSource source);
    void register_print_prepare_done(PrintPrepareDoneCb cb);

    void send_print_start(uint32_t file_index, middleware::PrintFileSource source,
                          uint16_t plate_thickness_um);
    void register_print_progress(PrintProgressCb cb);

    void send_print_terminate();
    void register_print_terminate_done(PrintTerminateDoneCb cb);

    /** 暂停/恢复打印。回执经 register_print_pause_done（仅打印进行中有效）。 */
    void send_print_pause();
    void send_print_resume();
    void send_print_powder_low_ignore();
    void register_print_pause_done(PrintPauseDoneCb cb);

    /**
     * 打印中缺粉（粉量位 bit0/bit1 同时为 0）。后端已自行暂停并另发一条
     * APP_Print_Pause_Rsp(paused=1)，本回调只负责弹补粉弹窗。
     */
    void register_print_powder_low(PrintPowderLowCb cb);

    /** D1 X / 二次确认 Cancel：退出解析线程并清空 RingBuffer */
    void send_parser_exit();

    /** Local 详情页删除确认：通知 Glavo 删文件 + 更新 config.json */
    void send_fs_delete_ntf(uint32_t file_index, middleware::PrintFileSource source);
    void register_fs_delete_rsp(FsDeleteRspCb cb);

    void register_fs_dir(FsDirCb cb);

    /** MCU OTA 测试（Settings） */
    void send_mcu_ota_start();
    void send_mcu_ota_cancel();
    void register_mcu_ota_status(McuOtaStatusCb cb);
    void send_serial_mcu_ota_start();
    void send_serial_mcu_ota_cancel();
    void register_serial_mcu_ota_status(McuOtaStatusCb cb);

    /** MCU 固件版本查询（Settings/About 进页触发一次） */
    void send_mcu_version_query();
    void register_mcu_version(McuVersionCb cb);   /**< GLAVO_Mcu_Version_Ntf */

    /** 振镜控制板固件版本（Settings/About：读 Glavo 侧 TCP 0x0F 首包缓存） */
    void send_galvo_version_query();
    void register_galvo_version(GalvoVersionCb cb); /**< GLAVO_Galvo_Version_Ntf */
    /** 云端 OTA 通知弹窗：收到 MQTT_Ota_Notify_Ntf 后由 UI 决定继续/取消。 */
    void send_ota_user_decision(const char* task_id, bool approve);
    void register_ota_notify(OtaNotifyCb cb);       /**< MQTT_Ota_Notify_Ntf */
    void register_ota_progress(OtaProgressCb cb);   /**< APP_Mqtt_Ota_Progress_Ntf */

    /** GLAVO_Device_Event_Ntf：设备事件/告警，喂消息中心。
     *  Glavo 侧还没实现发布，接好放着——后端上线后 UI 不用改。 */
    void register_device_event(DeviceEventCb cb);

    /** GLAVO_Alarm_Report_Ntf：MCU 单条报警/EVENT 上报（0xA3/0x20 原样透传），
     *  由 ui_alarm_state 消费。 */
    void register_alarm_report(AlarmReportCb cb);
    /** GLAVO_Alarm_Snapshot_Ntf：报警快照的一帧（0xA3/0x81 原样透传）。
     *  分片到达，攒帧与全量对齐由 ui_alarm_state 负责。 */
    void register_alarm_snapshot(AlarmSnapshotCb cb);
    /** UI_Alarm_Reset_Req：请求复位某条 CONDITION 报警。ACK 不代表成功，
     *  最终状态以后续 GLAVO_Alarm_Report_Ntf 为准。 */
    void send_alarm_reset(uint16_t alarm_code);

    /* ---- 网络 / WiFi（前端预留，后端接口到位后即可用；未上线时无人响应，不影响运行） ---- */
    void send_wifi_enable(bool enable);
    void send_wifi_scan();
    void send_wifi_connect(const char* ssid, const char* password);
    void send_wifi_forget(const char* ssid);
    void send_wifi_disconnect(const char* ssid);   /**< 断开当前连接 */
    void send_wifi_info();
    void send_wifi_saved_query();   /**< 请求已保存网络列表 → GLAVO_Wifi_Saved_Rsp */
    /** GLAVO_Wifi_State_Ntf：多路监听（顶栏图标、设置页各注册一次，互不覆盖）。 */
    void add_wifi_state_listener(WifiStateCb cb);
    void register_wifi_scan(WifiListCb cb);     /**< GLAVO_Wifi_Scan_Rsp */
    void register_wifi_saved(WifiListCb cb);    /**< GLAVO_Wifi_Saved_Rsp */
    void register_wifi_connect_result(WifiConnectResultCb cb); /**< GLAVO_Wifi_Connect_Rsp */
    void register_xfr_pair_pin(XfrPairPinCb cb);
    void register_xfr_file_received(XfrFileReceivedCb cb);
    void register_xfr_transfer_progress(XfrTransferProgressCb cb);
    void register_usb_hotplug(UsbHotplugCb cb);

private:
    UiMsgProxy() = default;

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
    void publish_pod(middleware::MsgType type, middleware::ModuleId target, const T& pod) {
        middleware::Message msg(type, middleware::ModuleId::UI, target);
        msg.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&pod), sizeof(T));
        middleware::MsgPort::instance().publish_async(msg);
    }

    /** 解析 WiFi 列表消息（[WifiListHeader][WifiEntryPayload×N]）并回调 */
    void dispatch_wifi_list(const middleware::Message& msg, const WifiListCb& cb);

    std::vector<middleware::Subscription> subs_;

    EnvUpdateCb      env_update_cb_;
    Pm25ResultCb     pm25_result_cb_;
    DoorResultCb     open_door_cb_;
    DoorResultCb     close_door_cb_;
    PurifyProgressCb purify_progress_cb_;
    PurifyDoneCb     purify_done_cb_;
    CleanDoneCb      clean_done_cb_;
    CleanStopDoneCb  clean_stop_done_cb_;
    CleanTestStartRspCb clean_test_start_rsp_cb_;
    CleanTestDoneCb clean_test_done_cb_;
    CleanTestCloseRspCb clean_test_close_rsp_cb_;
    TransportResultCb transport_result_cb_;
    FsDeleteRspCb     fs_delete_rsp_cb_;
    FsDirCb          fs_dir_cb_;
    PrintProgressCb  print_progress_cb_;
    PrintTerminateDoneCb print_terminate_done_cb_;
    PrintPauseDoneCb print_pause_done_cb_;
    PrintPowderLowCb print_powder_low_cb_;
    SupplyStatusCb   supply_status_cb_;
    PrintPrepareDoneCb print_prepare_done_cb_;
    McuOtaStatusCb   mcu_ota_status_cb_;
    McuOtaStatusCb   serial_mcu_ota_status_cb_;
    std::vector<WifiStateCb> wifi_state_cbs_;
    WifiListCb       wifi_scan_cb_;
    WifiListCb       wifi_saved_cb_;
    WifiConnectResultCb wifi_connect_result_cb_;
    XfrPairPinCb xfr_pair_pin_cb_;
    XfrFileReceivedCb xfr_file_received_cb_;
    XfrTransferProgressCb xfr_transfer_progress_cb_;
    UsbHotplugCb usb_hotplug_cb_;
    McuVersionCb mcu_version_cb_;
    GalvoVersionCb galvo_version_cb_;
    OtaNotifyCb ota_notify_cb_;
    OtaProgressCb ota_progress_cb_;
    DeviceEventCb device_event_cb_;
    AlarmReportCb alarm_report_cb_;
    AlarmSnapshotCb alarm_snapshot_cb_;
};

} // namespace ui

#endif
