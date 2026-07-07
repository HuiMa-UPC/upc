#ifndef CAMERA_MSG_PROXY_HPP
#define CAMERA_MSG_PROXY_HPP

#include "msg_port.hpp"
#include <cstring>
#include <functional>
#include <vector>

namespace ui {

using CameraStreamStartRspCb = std::function<void(
    const middleware::CameraStreamInfoPayload&)>;
using CameraStreamStopDoneCb = std::function<void()>;

/**
 * @brief UI ↔ APP_CAMERA（`rongsu_camera`）控制面（像素走 SHM，不经 MsgBus）
 *
 * 不订阅 APP_Camera_Frame_Ntf；UI 侧自行 poll SHM。
 * 回调均在 MsgPort::process 线程执行，勿操作 LVGL。
 */
class CameraMsgProxy {
public:
    static CameraMsgProxy& instance();

    void init();

    void send_stream_start(const char* video_basename = nullptr);
    void send_stream_stop();

    void register_stream_start_rsp(CameraStreamStartRspCb cb);
    void register_stream_stop_done(CameraStreamStopDoneCb cb);

private:
    CameraMsgProxy() = default;

    template <typename T>
    static bool read_inline_pod(const middleware::Message& msg, T& out)
    {
        if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
           msg.payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(T));
        return true;
    }

    std::vector<middleware::Subscription> subs_;

    CameraStreamStartRspCb start_rsp_cb_;
    CameraStreamStopDoneCb stop_done_cb_;
};

} // namespace ui

#endif
