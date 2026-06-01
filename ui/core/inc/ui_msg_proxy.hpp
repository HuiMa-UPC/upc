#ifndef UI_MSG_PROXY_HPP
#define UI_MSG_PROXY_HPP

#include "msg_bus.hpp"
#include <functional>
#include <vector>
#include <cstring>

namespace ui {

/**
 * @file ui_msg_proxy.hpp
 * @brief UI 与 MsgBus 之间的唯一门面（Facade）
 *
 * 职责划分：
 *   - init()：向 MsgBus 订阅所有「后端 → UI」的固定消息类型（订阅句柄保存在 subs_）
 *   - send_*：UI 发起的请求（Empty / Inline POD），异步 publish
 *   - register_*：由页面或 control 在运行时挂载的业务回调（单槽，后注册覆盖前次）
 *
 * 线程：register_* 收到的回调均在 UI 线程（MsgBus::process）中执行，可安全操作 LVGL。
 *
 * 使用者：
 *   - page_user_home：register_env_update / send_open_door（首页环境、滑动开门）
 *   - control：打印流程相关的 send_* / register_*（见 control.cpp）
 */

using EnvUpdateCb      = std::function<void(const middleware::EnvSnapshotPayload&)>;
using Pm25ResultCb     = std::function<void(const middleware::Pm25ResultPayload&)>;
using DoorResultCb     = std::function<void(const middleware::DoorResultPayload&)>;
using PurifyProgressCb = std::function<void(int progress)>;
using PurifyDoneCb     = std::function<void()>;
using FsDirCb          = std::function<void(const middleware::FileEntryPayload* files,
                                            uint32_t count)>;
using PrintProgressCb  = std::function<void(const middleware::PrintProgressPayload&)>;

class UiMsgProxy {
public:
    static UiMsgProxy& instance();

    /**
     * 建立 MsgBus 订阅（仅消息分发，不绑定业务逻辑）。
     * 须在 app_init 中、任何 register_* 之前调用一次。
     */
    void init();

    /* ---- 首页环境推送（MCU 周期 APP_Update_Ntf）---- */
    void register_env_update(EnvUpdateCb cb);

    /* ---- 打印流程：MCU ---- */
    void send_pm25_check();
    void register_pm25_result(Pm25ResultCb cb);

    void send_open_door();
    void register_open_door_result(DoorResultCb cb);

    void send_close_door_check();
    void register_close_door_result(DoorResultCb cb);

    void send_purify();
    void register_purify_progress(PurifyProgressCb cb);
    void register_purify_done(PurifyDoneCb cb);

    /* ---- 打印流程：振镜 / 文件 / 作业 ---- */
    void send_fs_dir_open();
    void register_fs_dir(FsDirCb cb);

    void send_print_start(uint32_t file_index);
    void register_print_progress(PrintProgressCb cb);
    void send_print_terminate();

private:
    UiMsgProxy() = default;

    /** 从 Inline payload 解析 POD；长度不足时返回 false */
    template <typename T>
    static bool read_inline_pod(const middleware::Message& msg, T& out) {
        if (msg.payload.type != middleware::MsgPayload::Type::Inline ||
            msg.payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(T));
        return true;
    }

    /** 发送带 Inline POD 的请求 */
    template <typename T>
    void publish_pod(middleware::MsgType type, middleware::ModuleId target, const T& pod) {
        middleware::Message msg(type, middleware::ModuleId::UI, target);
        msg.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&pod), sizeof(T));
        middleware::MsgBus::instance().publish_async(msg);
    }

    /** init() 时创建的订阅，生命周期与 UiMsgProxy 单例一致 */
    std::vector<middleware::Subscription> subs_;

    /* 业务回调单槽（由 register_* 赋值，init 里的 lambda 转调） */
    EnvUpdateCb      env_update_cb_;
    Pm25ResultCb     pm25_result_cb_;
    DoorResultCb     open_door_cb_;
    DoorResultCb     close_door_cb_;
    PurifyProgressCb purify_progress_cb_;
    PurifyDoneCb     purify_done_cb_;
    FsDirCb          fs_dir_cb_;
    PrintProgressCb  print_progress_cb_;
};

} // namespace ui

#endif
