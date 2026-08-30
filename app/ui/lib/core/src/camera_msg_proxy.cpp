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
        middleware::MsgType::APP_Camera_Stream_Stop_Rsp,
        middleware::ModuleId::UI,
        [this](const middleware::Message&) {
            if(stop_done_cb_) {
                stop_done_cb_();
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Camera_Snapshot_Rsp,
        middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::CameraSnapshotRspPayload p{};
            if(snapshot_rsp_cb_ && read_inline_pod(msg, p)) {
                snapshot_rsp_cb_(p);
            }
        }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Video_Manifest_Ntf,
        middleware::ModuleId::UI,
        [this](const middleware::Message& msg) {
            middleware::PrintVideoManifestPayload p{};
            if(print_video_manifest_cb_ && read_inline_pod(msg, p)) {
                print_video_manifest_cb_(p);
            }
        }));
}

void CameraMsgProxy::send_stream_start(const char* video_basename)
{
    middleware::Message msg(
        middleware::MsgType::UI_Camera_Stream_Start_Req,
        middleware::ModuleId::UI,
        middleware::ModuleId::APP_CAMERA);
    msg.priority = middleware::MsgPriority::High;

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
    msg.priority = middleware::MsgPriority::High;
    middleware::MsgPort::instance().publish_async(msg);
}

void CameraMsgProxy::send_snapshot_req()
{
    middleware::Message msg(
        middleware::MsgType::UI_Camera_Snapshot_Req,
        middleware::ModuleId::UI,
        middleware::ModuleId::APP_MEDIA);
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

void CameraMsgProxy::register_snapshot_rsp(CameraSnapshotRspCb cb)
{
    snapshot_rsp_cb_ = std::move(cb);
}

void CameraMsgProxy::register_print_video_manifest(PrintVideoManifestCb cb)
{
    print_video_manifest_cb_ = std::move(cb);
}

} // namespace ui
