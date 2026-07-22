#include "ui_msg_proxy.hpp"
#include "buffer_pool.hpp"

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
        middleware::MsgType::APP_CLOSE_DOOR_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::DoorResultPayload p{};
            if (close_door_cb_ && read_inline_pod(msg, p)) {
                close_door_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Purify_Progress, middleware::ModuleId::UI,
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
        middleware::MsgType::APP_Print_Terminate_Done, middleware::ModuleId::UI,
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
    
    /* ---- 网络 / WiFi（前端预留，待后端对齐） ---- */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Wifi_State_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::WifiStatePayload p{};
            if (wifi_state_cb_ && read_inline_pod(msg, p)) {
                wifi_state_cb_(p);
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

void UiMsgProxy::register_wifi_state(WifiStateCb cb)
{
    wifi_state_cb_ = std::move(cb);
}

void UiMsgProxy::register_wifi_scan(WifiListCb cb)
{
    wifi_scan_cb_ = std::move(cb);
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

void UiMsgProxy::send_close_door_check()
{
    middleware::Message msg(middleware::MsgType::UI_CLOSE_DOOR_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::register_close_door_result(DoorResultCb cb)
{
    close_door_cb_ = std::move(cb);
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
    const middleware::PrintStartReqPayload req{file_index, source, {}};
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

void UiMsgProxy::register_print_pause_done(PrintPauseDoneCb cb)
{
    print_pause_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_print_pause()
{
    middleware::Message msg(middleware::MsgType::UI_Print_Pause_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
}

void UiMsgProxy::send_print_resume()
{
    middleware::Message msg(middleware::MsgType::UI_Print_Resume_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgPort::instance().publish_async(msg);
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

} // namespace ui
