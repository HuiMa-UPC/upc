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
using FsDirCb          = std::function<void(const middleware::FileEntryPayload* files,
                                            uint32_t count)>;
using PrintProgressCb  = std::function<void(const middleware::PrintProgressPayload&)>;
using PrintTerminateDoneCb = std::function<void()>;
using SupplyStatusCb   = std::function<void(const middleware::SupplyStatusPayload&)>;
using PrintPrepareDoneCb = std::function<void(bool ok)>;

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

    /** path 为空或 Local 默认路径时打开 /home/Gcode（config.json 列表） */
    void send_fs_dir_open(const char* path = nullptr);

    void send_supply_status();
    void register_supply_status(SupplyStatusCb cb);

    void send_print_prepare(uint32_t file_index, middleware::PrintFileSource source);
    void register_print_prepare_done(PrintPrepareDoneCb cb);

    void send_print_start(uint32_t file_index, middleware::PrintFileSource source);
    void register_print_progress(PrintProgressCb cb);

    void send_print_terminate();
    void register_print_terminate_done(PrintTerminateDoneCb cb);

    /** D1 X / 二次确认 Cancel：退出解析线程并清空 RingBuffer */
    void send_parser_exit();

    /** Local 详情页删除确认：通知 Glavo 删文件 + 更新 config.json */
    void send_fs_delete_ntf(uint32_t file_index, middleware::PrintFileSource source);

    void register_fs_dir(FsDirCb cb);

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

    std::vector<middleware::Subscription> subs_;

    EnvUpdateCb      env_update_cb_;
    Pm25ResultCb     pm25_result_cb_;
    DoorResultCb     open_door_cb_;
    DoorResultCb     close_door_cb_;
    PurifyProgressCb purify_progress_cb_;
    PurifyDoneCb     purify_done_cb_;
    FsDirCb          fs_dir_cb_;
    PrintProgressCb  print_progress_cb_;
    PrintTerminateDoneCb print_terminate_done_cb_;
    SupplyStatusCb   supply_status_cb_;
    PrintPrepareDoneCb print_prepare_done_cb_;
};

} // namespace ui

#endif
