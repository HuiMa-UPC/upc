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
    auto& bus = middleware::MsgBus::instance();

    /* 首页环境快照 */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Update_Ntf, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::EnvSnapshotPayload p{};
            if (env_update_cb_ && read_inline_pod(msg, p)) {
                env_update_cb_(p);
            }
        }));

    /* MCU：PM2.5 / 开关门 / 净化 */
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

    /* GLAVO：目录列表（BufferRef；msg 析构时 MsgPayload RAII 自动 release） */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::Fs_DirOpen_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            if (!fs_dir_cb_) {
                return;
            }
            if (msg.payload.type != middleware::MsgPayload::Type::BufferRef) {
                fs_dir_cb_(nullptr, 0);
                return;
            }
            const auto& ref = msg.payload.data.buffer_ref;
            uint8_t* base = middleware::BufferPool::instance().get_ptr(ref.buf_id);
            if (!base) {
                fs_dir_cb_(nullptr, 0);
                return;
            }
            const auto* hdr = reinterpret_cast<const middleware::FsDirHeader*>(base);
            if (ref.length < sizeof(middleware::FsDirHeader)) {
                fs_dir_cb_(nullptr, 0);
                return;
            }
            const uint32_t max_count = static_cast<uint32_t>(
                (ref.length - sizeof(middleware::FsDirHeader)) /
                sizeof(middleware::FileEntryPayload));
            const uint32_t count = hdr->count <= max_count ? hdr->count : max_count;
            const auto* files = reinterpret_cast<const middleware::FileEntryPayload*>(
                base + sizeof(middleware::FsDirHeader));
            /* 回调内须同步拷贝数据，勿保存 files 裸指针（buffer 随 msg 析构释放） */
            fs_dir_cb_(files, count);
        }));

    /* GLAVO：文件详细信息响应 */
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_FileInfo_Rsp, middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::FileInfoPayload p{};
            if (file_info_cb_ && read_inline_pod(msg, p)) {
                file_info_cb_(p);
            }
        }));

    /* GLAVO：打印进度推送 */
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
}

void UiMsgProxy::register_env_update(EnvUpdateCb cb)
{
    env_update_cb_ = std::move(cb);
}

void UiMsgProxy::send_pm25_check()
{
    middleware::Message msg(middleware::MsgType::UI_PM25_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_MCU);
    middleware::MsgBus::instance().publish_async(msg);
}

void UiMsgProxy::register_pm25_result(Pm25ResultCb cb)
{
    pm25_result_cb_ = std::move(cb);
}

void UiMsgProxy::send_open_door()
{
    middleware::Message msg(middleware::MsgType::UI_OpenDoor_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_MCU);
    middleware::MsgBus::instance().publish_async(msg);
}

void UiMsgProxy::register_open_door_result(DoorResultCb cb)
{
    open_door_cb_ = std::move(cb);
}

void UiMsgProxy::send_close_door_check()
{
    middleware::Message msg(middleware::MsgType::UI_CLOSE_DOOR_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_MCU);
    middleware::MsgBus::instance().publish_async(msg);
}

void UiMsgProxy::register_close_door_result(DoorResultCb cb)
{
    close_door_cb_ = std::move(cb);
}

void UiMsgProxy::send_purify()
{
    middleware::Message msg(middleware::MsgType::UI_Purify_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_MCU);
    middleware::MsgBus::instance().publish_async(msg);
}

void UiMsgProxy::register_purify_progress(PurifyProgressCb cb)
{
    purify_progress_cb_ = std::move(cb);
}

void UiMsgProxy::register_purify_done(PurifyDoneCb cb)
{
    purify_done_cb_ = std::move(cb);
}

void UiMsgProxy::send_fs_dir_open()
{
    middleware::Message msg(middleware::MsgType::Fs_DirOpen_Req,
                            middleware::ModuleId::UI,
                            middleware::ModuleId::APP_GLAVO);
    middleware::MsgBus::instance().publish_async(msg);
}

void UiMsgProxy::register_fs_dir(FsDirCb cb)
{
    fs_dir_cb_ = std::move(cb);
}

void UiMsgProxy::send_file_info_req(uint32_t file_index)
{
    const middleware::PrintStartReqPayload req{file_index};
    publish_pod(middleware::MsgType::UI_FileInfo_Req,
                middleware::ModuleId::APP_GLAVO, req);
}

void UiMsgProxy::register_file_info(FileInfoCb cb)
{
    file_info_cb_ = std::move(cb);
}

void UiMsgProxy::send_print_start(uint32_t file_index)
{
    const middleware::PrintStartReqPayload req{file_index};
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
    middleware::MsgBus::instance().publish_async(msg);
}

} // namespace ui
