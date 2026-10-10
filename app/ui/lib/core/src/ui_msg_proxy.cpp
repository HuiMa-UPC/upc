#include "ui_msg_proxy.hpp"
#include "buffer_pool.hpp"
#include <cstdio>

namespace ui {

UiMsgProxy& UiMsgProxy::instance()
{
    static UiMsgProxy inst;
    return inst;
}

void UiMsgProxy::init()
{
    auto& bus = middleware::MsgPort::instance();

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Update_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::EnvSnapshotPayload p{};
            if (env_update_cb_ && read_inline_pod(msg, p)) {
                env_update_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_PM25_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::Pm25ResultPayload p{};
            if (pm25_result_cb_ && read_inline_pod(msg, p)) {
                pm25_result_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_OpenDoor_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DoorResultPayload p{};
            if (open_door_cb_ && read_inline_pod(msg, p)) {
                open_door_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Purify_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            if (purify_progress_cb_ &&
                msg.payload.type == middleware::MsgPayload::Type::Value) {
                purify_progress_cb_(static_cast<int>(msg.payload.data.value));
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Purify_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if (purify_done_cb_) {
                purify_done_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Start_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if (clean_done_cb_) {
                clean_done_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Stop_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if (clean_stop_done_cb_) {
                clean_stop_done_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Test_Start_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::CleanTestStartRspPayload p{};
            if (clean_test_start_rsp_cb_ && read_inline_pod(msg, p)) {
                clean_test_start_rsp_cb_(p.accepted != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Test_Result_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::CleanTestDonePayload p{};
            if (clean_test_done_cb_ && read_inline_pod(msg, p)) {
                clean_test_done_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Test_Close_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if (clean_test_close_rsp_cb_) {
                clean_test_close_rsp_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Transport_Result_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::TransportResultPayload p{};
            if (transport_result_cb_ && read_inline_pod(msg, p)) {
                transport_result_cb_(p.result);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Transfer_Demo_Prep_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DoorResultPayload p{};
            if (transfer_demo_prep_cb_ && read_inline_pod(msg, p)) {
                transfer_demo_prep_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Transfer_Demo_Confirm_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DoorResultPayload p{};
            if (transfer_demo_confirm_cb_ && read_inline_pod(msg, p)) {
                transfer_demo_confirm_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Fs_Delete_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::FsDeleteRspPayload p{};
            if (fs_delete_rsp_cb_ && read_inline_pod(msg, p)) {
                fs_delete_rsp_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::Fs_DirOpen_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            if (!fs_dir_cb_) {
                return;
            }
            const uint8_t* base = nullptr;
            size_t data_len = 0;
            if (msg.payload.type == middleware::MsgPayload::Type::BufferRef) {
                const auto& ref = msg.payload.data.buffer_ref;
                base = middleware::BufferPool::instance().get_ptr(ref.buf_id);
                data_len = ref.length;
            } else if (msg.payload.type == middleware::MsgPayload::Type::Inline) {
                base = msg.payload.data.bytes.data();
                data_len = msg.payload.length;
            } else {
                fs_dir_cb_(nullptr, 0);
                return;
            }
            if (!base || data_len < sizeof(middleware::FsDirHeader)) {
                fs_dir_cb_(nullptr, 0);
                return;
            }
            const auto* hdr = reinterpret_cast<const middleware::FsDirHeader*>(base);
            const uint32_t max_count = static_cast<uint32_t>(
                (data_len - sizeof(middleware::FsDirHeader)) /
                sizeof(middleware::FileEntryPayload));
            const uint32_t count = hdr->count <= max_count ? hdr->count : max_count;
            const auto* files = reinterpret_cast<const middleware::FileEntryPayload*>(
                base + sizeof(middleware::FsDirHeader));
            fs_dir_cb_(files, count);
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Progress_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintProgressPayload p{};
            if (print_progress_cb_ && read_inline_pod(msg, p)) {
                print_progress_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Terminate_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if (print_terminate_done_cb_) {
                print_terminate_done_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Pause_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintPauseStatePayload p{};
            if (print_pause_done_cb_ && read_inline_pod(msg, p)) {
                print_pause_done_cb_(p.paused != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Powder_Low_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if (print_powder_low_cb_) {
                print_powder_low_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_OpticalPath_Progress_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::OpticalPathProgressPayload p{};
            if(optical_path_progress_cb_ && read_inline_pod(msg, p)) {
                optical_path_progress_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Prep_Progress_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintPrepProgressPayload p{};
            if(print_prep_progress_cb_ && read_inline_pod(msg, p)) {
                print_prep_progress_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Start_Rejected_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintStartRejectedPayload p{};
            if(print_start_rejected_cb_ && read_inline_pod(msg, p)) {
                print_start_rejected_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Print_State_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintStateNtfPayload p{};
            if(print_state_cb_ && read_inline_pod(msg, p)) {
                print_state_cb_(p.mode);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Outer_Door_Confirm_Req, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if(outer_door_confirm_req_cb_) {
                outer_door_confirm_req_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Plate_Place_Confirm_Req, middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if(plate_place_confirm_req_cb_) {
                plate_place_confirm_req_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Supply_Status_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::SupplyStatusPayload p{};
            if (supply_status_cb_ && read_inline_pod(msg, p)) {
                supply_status_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Prepare_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintPrepareRspPayload p{};
            if (print_prepare_done_cb_ && read_inline_pod(msg, p)) {
                print_prepare_done_cb_(p.ok != 0);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mcu_Ota_Status_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::McuOtaStatusPayload p{};
            if (mcu_ota_status_cb_ && read_inline_pod(msg, p)) {
                mcu_ota_status_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Serial_Mcu_Ota_Status_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::McuOtaStatusPayload p{};
            if (serial_mcu_ota_status_cb_ && read_inline_pod(msg, p)) {
                serial_mcu_ota_status_cb_(p);
            }
        }));
    
    /* ---- 网络 / WiFi（前端预留，待后端对齐） ---- */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Wifi_State_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::WifiStatePayload p{};
            if (wifi_state_cbs_.empty() || !read_inline_pod(msg, p)) {
                return;
            }
            p.ssid[sizeof(p.ssid) - 1] = '\0';
            p.ip_addr[sizeof(p.ip_addr) - 1] = '\0';
            for (const auto& cb : wifi_state_cbs_) {
                cb(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Wifi_Scan_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            dispatch_wifi_list(msg, wifi_scan_cb_);
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Wifi_Saved_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            dispatch_wifi_list(msg, wifi_saved_cb_);
        }));
    
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Wifi_Connect_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::WifiConnectRspPayload p{};
            if (wifi_connect_result_cb_ && read_inline_pod(msg, p)) {
                p.ssid[sizeof(p.ssid) - 1] = '\0';
                wifi_connect_result_cb_(p.ok != 0, p.ssid);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::XFR_Pair_Pin_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::XfrPairPinPayload p{};
            if (xfr_pair_pin_cb_ && read_inline_pod(msg, p)) {
                p.pin[sizeof(p.pin) - 1] = '\0';
                xfr_pair_pin_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::XFR_File_Received_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::XfrFileReceivedPayload p{};
            if (xfr_file_received_cb_ && read_inline_pod(msg, p)) {
                p.file_name[sizeof(p.file_name) - 1] = '\0';
                xfr_file_received_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::XFR_Transfer_Progress_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::XfrTransferProgressPayload p{};
            if (xfr_transfer_progress_cb_ && read_inline_pod(msg, p)) {
                p.file_name[sizeof(p.file_name) - 1] = '\0';
                xfr_transfer_progress_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_USB_Hotplug_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::UsbHotplugPayload p{};
            if (!read_inline_pod(msg, p)) {
                return;
            }
            p.mount_path[sizeof(p.mount_path) - 1] = '\0';
            usb_mounted_ = (p.present != 0 && p.mounted != 0);
            for (const auto& cb : usb_hotplug_cbs_) {
                if (cb) {
                    cb(p);
                }
            }
        }));

    /* SD 卡热插拔：载荷与 USB 那条同形，但**是两个不同的卷**，回调各挂各的。
     * 后端未实现，这条订阅目前收不到任何东西。 */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_SdCard_Hotplug_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::UsbHotplugPayload p{};
            if (!read_inline_pod(msg, p)) {
                return;
            }
            p.mount_path[sizeof(p.mount_path) - 1] = '\0';
            sdcard_mounted_ = (p.present != 0 && p.mounted != 0);
            if (sdcard_hotplug_cb_) {
                sdcard_hotplug_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Storage_Usage_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::StorageUsagePayload p{};
            if (storage_usage_cb_ && read_inline_pod(msg, p)) {
                storage_usage_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Storage_Format_Progress_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::StorageFormatProgressPayload p{};
            if (storage_format_progress_cb_ && read_inline_pod(msg, p)) {
                storage_format_progress_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Storage_Format_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::StorageFormatRspPayload p{};
            if (storage_format_result_cb_ && read_inline_pod(msg, p)) {
                storage_format_result_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Mcu_Version_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::McuVersionPayload p{};
            if (mcu_version_cb_ && read_inline_pod(msg, p)) {
                mcu_version_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Galvo_Version_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::GalvoVersionPayload p{};
            if (galvo_version_cb_ && read_inline_pod(msg, p)) {
                galvo_version_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::MQTT_Ota_Notify_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::OtaNotifyPayload p{};
            if (ota_notify_cb_ && read_inline_pod(msg, p)) {
                p.task_id[sizeof(p.task_id) - 1] = '\0';
                p.release_version[sizeof(p.release_version) - 1] = '\0';
                ota_notify_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Ota_Progress_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::OtaProgressPayload p{};
            if (ota_progress_cb_ && read_inline_pod(msg, p)) {
                p.task_id[sizeof(p.task_id) - 1] = '\0';
                p.component[sizeof(p.component) - 1] = '\0';
                p.error_code[sizeof(p.error_code) - 1] = '\0';
                ota_progress_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Soc_Ota_Ready_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::SocOtaReadyPayload p{};
            if (soc_ota_ready_cb_ && read_inline_pod(msg, p)) {
                p.version[sizeof(p.version) - 1] = '\0';
                p.task_id[sizeof(p.task_id) - 1] = '\0';
                soc_ota_ready_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Soc_Ota_Apply_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::SocOtaApplyRspPayload p{};
            if (soc_ota_apply_rsp_cb_ && read_inline_pod(msg, p)) {
                p.reason[sizeof(p.reason) - 1] = '\0';
                soc_ota_apply_rsp_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Device_Event_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DeviceEventPayload p{};
            if (device_event_cb_ && read_inline_pod(msg, p)) {
                device_event_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Alarm_Report_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::AlarmReportPayload p{};
            if (alarm_report_cb_ && read_inline_pod(msg, p)) {
                alarm_report_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Alarm_Snapshot_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::AlarmSnapshotPayload p{};
            if (alarm_snapshot_cb_ && read_inline_pod(msg, p)) {
                alarm_snapshot_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Alarm_Reset_Result_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::AlarmResetResultPayload p{};
            if (alarm_reset_result_cb_ && read_inline_pod(msg, p)) {
                alarm_reset_result_cb_(p);
            }
        }));

}

/* 解析 [WifiListHeader][WifiEntryPayload × count]（BufferRef/Inline 皆可），仿 Fs_DirOpen_Rsp。 */
void UiMsgProxy::dispatch_wifi_list(const middleware::Message& msg, const WifiListCb& cb)
{
    if (!cb) {
        return;
    }
    const uint8_t* base = nullptr;
    size_t data_len = 0;
    if (msg.payload.type == middleware::MsgPayload::Type::BufferRef) {
        const auto& ref = msg.payload.data.buffer_ref;
        base = middleware::BufferPool::instance().get_ptr(ref.buf_id);
        data_len = ref.length;
    } else if (msg.payload.type == middleware::MsgPayload::Type::Inline) {
        base = msg.payload.data.bytes.data();
        data_len = msg.payload.length;
    } else {
        cb(nullptr, 0);
        return;
    }
    if (!base || data_len < sizeof(middleware::WifiListHeader)) {
        cb(nullptr, 0);
        return;
    }
    const auto* hdr = reinterpret_cast<const middleware::WifiListHeader*>(base);
    const uint32_t max_count = static_cast<uint32_t>(
        (data_len - sizeof(middleware::WifiListHeader)) /
        sizeof(middleware::WifiEntryPayload));
    const uint32_t count = hdr->count <= max_count ? hdr->count : max_count;
    const auto* items = reinterpret_cast<const middleware::WifiEntryPayload*>(
        base + sizeof(middleware::WifiListHeader));
    cb(items, count);

}

void UiMsgProxy::register_env_update(EnvUpdateCb cb)
{
    env_update_cb_ = std::move(cb);
}

void UiMsgProxy::register_wifi_connect_result(WifiConnectResultCb cb)
{
    wifi_connect_result_cb_ = std::move(cb);
}

/* ---- 网络 / WiFi（前端预留，待后端对齐） ---- */
void UiMsgProxy::send_wifi_enable(bool enable)
{
    const middleware::WifiEnableReqPayload payload{ enable ? uint8_t(1) : uint8_t(0) };
    publish_pod(middleware::MsgType::UI_Wifi_Enable_Req,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_wifi_scan()
{
    middleware::Message msg(middleware::MsgType::UI_Wifi_Scan_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_storage_usage(middleware::StorageVolume volume)
{
    const middleware::StorageUsageReqPayload payload{ static_cast<uint8_t>(volume), {} };
    publish_pod(middleware::MsgType::UI_Storage_Usage_Req,
                middleware::ModuleId::APP_MEDIA, payload);
}

void UiMsgProxy::send_storage_format(middleware::StorageVolume volume)
{
    const middleware::StorageFormatReqPayload payload{ static_cast<uint8_t>(volume), {} };
    publish_pod(middleware::MsgType::UI_Storage_Format_Req,
                middleware::ModuleId::APP_MEDIA, payload);
}

void UiMsgProxy::send_wifi_connect(const char* ssid, const char* password)
{
    middleware::WifiConnectReqPayload payload{};
    if (ssid) {
        std::strncpy(payload.ssid, ssid, sizeof(payload.ssid) - 1);
    }
    if (password) {
        std::strncpy(payload.password, password, sizeof(payload.password) - 1);
    }
    publish_pod(middleware::MsgType::UI_Wifi_Connect_Req,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_wifi_forget(const char* ssid)
{
    middleware::WifiForgetReqPayload payload{};
    if (ssid) {
        std::strncpy(payload.ssid, ssid, sizeof(payload.ssid) - 1);
    }
    publish_pod(middleware::MsgType::UI_Wifi_Forget_Req,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_wifi_disconnect(const char* ssid)
{
    middleware::WifiForgetReqPayload payload{};   /* 复用 ssid 载荷 */
    if (ssid) {
        std::strncpy(payload.ssid, ssid, sizeof(payload.ssid) - 1);
    }
    publish_pod(middleware::MsgType::UI_Wifi_Disconnect_Req,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_wifi_info()
{
    middleware::Message msg(middleware::MsgType::UI_Wifi_Info_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_wifi_saved_query()
{
    middleware::Message msg(middleware::MsgType::UI_Wifi_Saved_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::add_wifi_state_listener(WifiStateCb cb)
{
    if (cb) {
        wifi_state_cbs_.push_back(std::move(cb));
    }
}

void UiMsgProxy::register_wifi_scan(WifiListCb cb)
{
    wifi_scan_cb_ = std::move(cb);
}

void UiMsgProxy::register_sdcard_hotplug(UsbHotplugCb cb)
{
    sdcard_hotplug_cb_ = std::move(cb);
}

void UiMsgProxy::register_storage_usage(StorageUsageCb cb)
{
    storage_usage_cb_ = std::move(cb);
}

void UiMsgProxy::register_storage_format_progress(StorageFormatProgressCb cb)
{
    storage_format_progress_cb_ = std::move(cb);
}

void UiMsgProxy::register_storage_format_result(StorageFormatResultCb cb)
{
    storage_format_result_cb_ = std::move(cb);
}

void UiMsgProxy::register_wifi_saved(WifiListCb cb)
{
    wifi_saved_cb_ = std::move(cb);
}

void UiMsgProxy::register_xfr_pair_pin(XfrPairPinCb cb)
{
    xfr_pair_pin_cb_ = std::move(cb);
}

void UiMsgProxy::register_xfr_file_received(XfrFileReceivedCb cb)
{
    xfr_file_received_cb_ = std::move(cb);
}

void UiMsgProxy::register_xfr_transfer_progress(XfrTransferProgressCb cb)
{
    xfr_transfer_progress_cb_ = std::move(cb);
}

void UiMsgProxy::add_usb_hotplug_listener(UsbHotplugCb cb)
{
    if (cb) {
        usb_hotplug_cbs_.push_back(std::move(cb));
    }
}

void UiMsgProxy::send_mcu_version_query()
{
    middleware::Message msg(middleware::MsgType::UI_Mcu_Version_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_mcu_version(McuVersionCb cb)
{
    mcu_version_cb_ = std::move(cb);
}

void UiMsgProxy::send_galvo_version_query()
{
    middleware::Message msg(middleware::MsgType::UI_Galvo_Version_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_galvo_version(GalvoVersionCb cb)
{
    galvo_version_cb_ = std::move(cb);
}

void UiMsgProxy::send_ota_user_decision(const char* task_id, bool approve)
{
    middleware::OtaDecisionReqPayload payload{};
    if(task_id && task_id[0]) {
        std::snprintf(payload.task_id, sizeof(payload.task_id), "%s", task_id);
    }
    payload.approve = approve ? uint8_t(1) : uint8_t(0);
    publish_pod(middleware::MsgType::UI_Ota_User_Decision_Req,
                middleware::ModuleId::APP_MQTT, payload);
}

void UiMsgProxy::register_ota_notify(OtaNotifyCb cb)
{
    ota_notify_cb_ = std::move(cb);
}

void UiMsgProxy::register_ota_progress(OtaProgressCb cb)
{
    ota_progress_cb_ = std::move(cb);
}

void UiMsgProxy::send_soc_ota_apply(middleware::SocOtaApplyAction action)
{
    middleware::SocOtaApplyReqPayload payload{};
    payload.action = static_cast<uint8_t>(action);
    publish_pod(middleware::MsgType::UI_Soc_Ota_Apply_Req,
                middleware::ModuleId::APP_MQTT, payload);
}

void UiMsgProxy::register_soc_ota_ready(SocOtaReadyCb cb)
{
    soc_ota_ready_cb_ = std::move(cb);
}

void UiMsgProxy::register_soc_ota_apply_rsp(SocOtaApplyRspCb cb)
{
    soc_ota_apply_rsp_cb_ = std::move(cb);
}

void UiMsgProxy::register_device_event(DeviceEventCb cb)
{
    device_event_cb_ = std::move(cb);
}

void UiMsgProxy::register_alarm_report(AlarmReportCb cb)
{
    alarm_report_cb_ = std::move(cb);
}

void UiMsgProxy::register_alarm_snapshot(AlarmSnapshotCb cb)
{
    alarm_snapshot_cb_ = std::move(cb);
}

void UiMsgProxy::register_alarm_reset_result(AlarmResetResultCb cb)
{
    alarm_reset_result_cb_ = std::move(cb);
}

void UiMsgProxy::send_alarm_reset(uint16_t alarm_code)
{
    middleware::AlarmResetReqPayload payload{};
    payload.alarm_code = alarm_code;
    publish_pod(middleware::MsgType::UI_Alarm_Reset_Ntf,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_alarm_snapshot_req()
{
    middleware::Message msg(middleware::MsgType::UI_Alarm_Snapshot_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}


void UiMsgProxy::send_pm25_check()
{
    middleware::Message msg(middleware::MsgType::UI_PM25_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_pm25_result(Pm25ResultCb cb)
{
    pm25_result_cb_ = std::move(cb);
}

void UiMsgProxy::send_open_door()
{
    middleware::Message msg(middleware::MsgType::UI_OpenDoor_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_open_door_result(DoorResultCb cb)
{
    open_door_cb_ = std::move(cb);
}

void UiMsgProxy::send_purify()
{
    middleware::Message msg(middleware::MsgType::UI_Purify_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_purify_progress(PurifyProgressCb cb)
{
    purify_progress_cb_ = std::move(cb);
}

void UiMsgProxy::register_purify_done(PurifyDoneCb cb)
{
    purify_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_clean_start()
{
    middleware::Message msg(middleware::MsgType::UI_Clean_Start_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_clean_start_result(CleanDoneCb cb)
{
    clean_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_clean_stop()
{
    middleware::Message msg(middleware::MsgType::UI_Clean_Stop_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_clean_stop_result(CleanStopDoneCb cb)
{
    clean_stop_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_clean_test_start(const char* job_name, uint32_t width,
                                       uint32_t height, uint32_t fps,
                                       bool record_video, uint8_t record_kind)
{
    middleware::CleanTestStartReqPayload req{};
    if(job_name && job_name[0]) {
        std::snprintf(req.job_name, sizeof(req.job_name), "%s", job_name);
    }
    req.width = width;
    req.height = height;
    req.fps = fps;
    req.record_video = record_video ? 1 : 0;
    req.record_kind = record_kind;
    publish_pod(middleware::MsgType::UI_Clean_Test_Start_Req,
                middleware::ModuleId::APP_GLAVO, req);
}

void UiMsgProxy::register_clean_test_start_rsp(CleanTestStartRspCb cb)
{
    clean_test_start_rsp_cb_ = std::move(cb);
}

void UiMsgProxy::register_clean_test_done(CleanTestDoneCb cb)
{
    clean_test_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_clean_test_close()
{
    middleware::Message msg(middleware::MsgType::UI_Clean_Test_Close_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_print_video_finalize(const char* frames_dir)
{
    middleware::PrintVideoFinalizeReqPayload req{};
    if(frames_dir && frames_dir[0]) {
        std::snprintf(req.frames_dir, sizeof(req.frames_dir), "%s", frames_dir);
    }
    publish_pod(middleware::MsgType::UI_Print_Video_Finalize_Req,
                middleware::ModuleId::APP_MEDIA, req);
}

void UiMsgProxy::register_clean_test_close_rsp(CleanTestCloseRspCb cb)
{
    clean_test_close_rsp_cb_ = std::move(cb);
}

void UiMsgProxy::send_transport_request()
{
    middleware::Message msg(middleware::MsgType::UI_Transport_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_transport_result(TransportResultCb cb)
{
    transport_result_cb_ = std::move(cb);
}

void UiMsgProxy::send_transfer_demo_prep()
{
    middleware::Message msg(middleware::MsgType::UI_Transfer_Demo_Prep_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_transfer_demo_prep(DoorResultCb cb)
{
    transfer_demo_prep_cb_ = std::move(cb);
}

void UiMsgProxy::send_transfer_demo_confirm()
{
    middleware::Message msg(middleware::MsgType::UI_Transfer_Demo_Confirm_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_transfer_demo_confirm(DoorResultCb cb)
{
    transfer_demo_confirm_cb_ = std::move(cb);
}

void UiMsgProxy::send_fs_dir_open(middleware::PrintFileSource source)
{
    /* Local 列表由 UI 读 config.json；DirOpen 仅 USB */
    if(source != middleware::PrintFileSource::Usb) {
        return;
    }
    const middleware::FsDirOpenReqPayload payload{source, {}};
    publish_pod(middleware::MsgType::Fs_DirOpen_Req,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_fs_delete_ntf(uint32_t file_index, middleware::PrintFileSource source)
{
    const middleware::FsDeleteNtfPayload payload{file_index, source, {}};
    publish_pod(middleware::MsgType::Fs_Delete_Ntf,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::register_fs_delete_rsp(FsDeleteRspCb cb)
{
    fs_delete_rsp_cb_ = std::move(cb);
}

void UiMsgProxy::register_fs_dir(FsDirCb cb)
{
    fs_dir_cb_ = std::move(cb);
}

void UiMsgProxy::send_supply_status()
{
    middleware::Message msg(middleware::MsgType::UI_Supply_Status_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_supply_status(SupplyStatusCb cb)
{
    supply_status_cb_ = std::move(cb);
}

void UiMsgProxy::send_print_prepare(uint32_t file_index, middleware::PrintFileSource source)
{
    const middleware::PrintPrepareReqPayload req{file_index, source, {}};
    publish_pod(middleware::MsgType::UI_Print_Prepare_Req,
                middleware::ModuleId::APP_GLAVO, req);
}

void UiMsgProxy::register_print_prepare_done(PrintPrepareDoneCb cb)
{
    print_prepare_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_print_start(uint32_t file_index, middleware::PrintFileSource source)
{
    const middleware::PrintStartReqPayload req{file_index, source, {0, 0, 0}};
    publish_pod(middleware::MsgType::UI_Print_Start_Req,
                middleware::ModuleId::APP_GLAVO, req);
}

void UiMsgProxy::register_print_progress(PrintProgressCb cb)
{
    print_progress_cb_ = std::move(cb);
}

void UiMsgProxy::register_print_terminate_done(PrintTerminateDoneCb cb)
{
    print_terminate_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_print_terminate()
{
    middleware::Message msg(middleware::MsgType::UI_Print_Terminate_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_cloud_result_dismiss()
{
    middleware::Message msg(middleware::MsgType::UI_Cloud_Result_Dismiss_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_print_pause_done(PrintPauseDoneCb cb)
{
    print_pause_done_cb_ = std::move(cb);
}

bool UiMsgProxy::send_print_pause()
{
    middleware::Message msg(middleware::MsgType::UI_Print_Pause_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    return middleware::MsgPort::instance().publish_async(msg);
}

bool UiMsgProxy::send_print_resume()
{
    middleware::Message msg(middleware::MsgType::UI_Print_Resume_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    return middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_print_powder_low_ignore()
{
    middleware::Message msg(middleware::MsgType::UI_Print_Powder_Low_Ignore_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_print_powder_low(PrintPowderLowCb cb)
{
    print_powder_low_cb_ = std::move(cb);
}

void UiMsgProxy::register_optical_path_progress(OpticalPathProgressCb cb)
{
    optical_path_progress_cb_ = std::move(cb);
}

void UiMsgProxy::register_print_prep_progress(PrintPrepProgressCb cb)
{
    print_prep_progress_cb_ = std::move(cb);
}

void UiMsgProxy::register_print_start_rejected(PrintStartRejectedCb cb)
{
    print_start_rejected_cb_ = std::move(cb);
}

void UiMsgProxy::register_print_state(PrintStateCb cb)
{
    print_state_cb_ = std::move(cb);
}

void UiMsgProxy::register_outer_door_confirm_req(OuterDoorConfirmReqCb cb)
{
    outer_door_confirm_req_cb_ = std::move(cb);
}

void UiMsgProxy::send_outer_door_confirm(bool skip)
{
    middleware::OuterDoorConfirmRspPayload payload{};
    payload.skip = skip ? uint8_t(1) : uint8_t(0);
    publish_pod(middleware::MsgType::UI_Outer_Door_Confirm_Rsp,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::register_plate_place_confirm_req(PlatePlaceConfirmReqCb cb)
{
    plate_place_confirm_req_cb_ = std::move(cb);
}

void UiMsgProxy::send_plate_place_confirm(bool cancel)
{
    middleware::PlatePlaceConfirmRspPayload payload{};
    payload.cancel = cancel ? uint8_t(1) : uint8_t(0);
    publish_pod(middleware::MsgType::UI_Plate_Place_Confirm_Rsp,
                middleware::ModuleId::APP_GLAVO, payload);
}

void UiMsgProxy::send_parser_exit()
{
    middleware::Message msg(middleware::MsgType::UI_Parser_Exit_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_mcu_ota_start()
{
    middleware::Message msg(middleware::MsgType::UI_Mcu_Ota_Start_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_mcu_ota_cancel()
{
    middleware::Message msg(middleware::MsgType::UI_Mcu_Ota_Cancel_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_mcu_ota_status(McuOtaStatusCb cb)
{
    mcu_ota_status_cb_ = std::move(cb);
}

void UiMsgProxy::send_serial_mcu_ota_start()
{
    middleware::Message msg(middleware::MsgType::UI_Serial_Mcu_Ota_Start_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_serial_mcu_ota_cancel()
{
    middleware::Message msg(middleware::MsgType::UI_Serial_Mcu_Ota_Cancel_Ntf,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_serial_mcu_ota_status(McuOtaStatusCb cb)
{
    serial_mcu_ota_status_cb_ = std::move(cb);
}

} // namespace ui
