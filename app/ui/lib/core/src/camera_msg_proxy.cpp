#include "camera_msg_proxy.hpp"

namespace ui {

CameraMsgProxy& CameraMsgProxy::instance()
{
    static CameraMsgProxy inst;
    return inst;
}

void CameraMsgProxy::init()
{
    auto& bus = middleware::MsgPort::instance();

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Camera_Stream_Start_Rsp,
        middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::CameraStreamInfoPayload p{};
            if(start_rsp_cb_ && read_inline_pod(msg, p)) {
                start_rsp_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Camera_Stream_Stop_Done,
        middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if(stop_done_cb_) {
                stop_done_cb_();
            }
        }));
}

void CameraMsgProxy::send_stream_start(const char* video_basename)
{
    middleware::Message msg(
        middleware::MsgType::UI_Camera_Stream_Start_Req,
        middleware::ModuleId::UI,
        middleware::ModuleId::APP_CAMERA);

    if(video_basename && video_basename[0]) {
        middleware::CameraStreamStartReqPayload req{};
        std::snprintf(req.video_name, sizeof(req.video_name), "%s",
                      video_basename);
        msg.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    }

    middleware::MsgPort::instance().publish_async(msg);
}

void CameraMsgProxy::send_stream_stop()
{
    middleware::Message msg(
        middleware::MsgType::UI_Camera_Stream_Stop_Req,
        middleware::ModuleId::UI,
        middleware::ModuleId::APP_CAMERA);
    middleware::MsgPort::instance().publish_async(msg);
}

void CameraMsgProxy::register_stream_start_rsp(CameraStreamStartRspCb cb)
{
    start_rsp_cb_ = std::move(cb);
}

void CameraMsgProxy::register_stream_stop_done(CameraStreamStopDoneCb cb)
{
    stop_done_cb_ = std::move(cb);
}

} // namespace ui
