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
using FsDirCb          = std::function<void(const middleware::FileEntryPayload* files,
                                            uint32_t count)>;
using PrintProgressCb  = std::function<void(const middleware::PrintProgressPayload&)>;
using PrintTerminateDoneCb = std::function<void()>;
using PrintPauseDoneCb = std::function<void(bool paused)>;
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
using UsbHotplugCb     = std::function<void(const middleware::UsbHotplugPayload&)>;

                                            

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
    void register_clean_test_close_rsp(CleanTestCloseRspCb cb);

    /** 列目录：当前 UI 仅对 USB 调用；Local 由 control 直读 config.json */
    void send_fs_dir_open(middleware::PrintFileSource source);

    void send_supply_status();
    void register_supply_status(SupplyStatusCb cb);

    void send_print_prepare(uint32_t file_index, middleware::PrintFileSource source);
    void register_print_prepare_done(PrintPrepareDoneCb cb);

    void send_print_start(uint32_t file_index, middleware::PrintFileSource source);
    void register_print_progress(PrintProgressCb cb);

    void send_print_terminate();
    void register_print_terminate_done(PrintTerminateDoneCb cb);

    /** 暂停/恢复打印。回执经 register_print_pause_done（仅打印进行中有效）。 */
    void send_print_pause();
    void send_print_resume();
    void register_print_pause_done(PrintPauseDoneCb cb);

    /** D1 X / 二次确认 Cancel：退出解析线程并清空 RingBuffer */
    void send_parser_exit();

    /** Local 详情页删除确认：通知 Glavo 删文件 + 更新 config.json */
    void send_fs_delete_ntf(uint32_t file_index, middleware::PrintFileSource source);

    void register_fs_dir(FsDirCb cb);

    /** MCU OTA 测试（Settings） */
    void send_mcu_ota_start();
    void send_mcu_ota_cancel();
    void register_mcu_ota_status(McuOtaStatusCb cb);
    void send_serial_mcu_ota_start();
    void send_serial_mcu_ota_cancel();
    void register_serial_mcu_ota_status(McuOtaStatusCb cb);

    /* ---- 网络 / WiFi（前端预留，后端接口到位后即可用；未上线时无人响应，不影响运行） ---- */
    void send_wifi_enable(bool enable);
    void send_wifi_scan();
    void send_wifi_connect(const char* ssid, const char* password);
    void send_wifi_forget(const char* ssid);
    void send_wifi_disconnect(const char* ssid);   /**< 断开当前连接 */
    void send_wifi_info();
    void send_wifi_saved_query();   /**< 请求已保存网络列表 → GLAVO_Wifi_Saved_Rsp */
    void register_wifi_state(WifiStateCb cb);   /**< GLAVO_Wifi_State_Ntf */
    void register_wifi_scan(WifiListCb cb);     /**< GLAVO_Wifi_Scan_Rsp */
    void register_wifi_saved(WifiListCb cb);    /**< GLAVO_Wifi_Saved_Rsp */
    void register_wifi_connect_result(WifiConnectResultCb cb); /**< GLAVO_Wifi_Connect_Rsp */
    void register_xfr_pair_pin(XfrPairPinCb cb);
    void register_xfr_file_received(XfrFileReceivedCb cb);
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
    FsDirCb          fs_dir_cb_;
    PrintProgressCb  print_progress_cb_;
    PrintTerminateDoneCb print_terminate_done_cb_;
    PrintPauseDoneCb print_pause_done_cb_;
    SupplyStatusCb   supply_status_cb_;
    PrintPrepareDoneCb print_prepare_done_cb_;
    McuOtaStatusCb   mcu_ota_status_cb_;
    McuOtaStatusCb   serial_mcu_ota_status_cb_;
    WifiStateCb      wifi_state_cb_;
    WifiListCb       wifi_scan_cb_;
    WifiListCb       wifi_saved_cb_;
    WifiConnectResultCb wifi_connect_result_cb_;
    XfrPairPinCb xfr_pair_pin_cb_;
    XfrFileReceivedCb xfr_file_received_cb_;
    UsbHotplugCb usb_hotplug_cb_;
};

} // namespace ui

#endif
