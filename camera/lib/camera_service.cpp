#include "camera_service.hpp"

#include "device_config.hpp"
#include "ipc_shm_region.hpp"
#include "ipc_shm_types.hpp"
#include "msg_port.hpp"
#include "msg_types.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {

uint64_t monotonic_us()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now().time_since_epoch())
            .count());
}

const char* env_or_default(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

} // namespace

namespace upc::camera {

CameraService::~CameraService()
{
    shutdown();
}

bool CameraService::init()
{
    stream_fps_ = read_fps_from_env();

    const char* ntf_env = std::getenv(CAMERA_ENV_FRAME_NTF);
    if(ntf_env && ntf_env[0]) {
        frame_ntf_enabled_ = (ntf_env[0] != '0');
    }

    subscribe_messages();
    UPC_LOG_CAMERA_SERVICE(Info,
        "init device=%s %ux%u@%u iq=%s/%s shm=%s frame_ntf=%d",
        env_or_default(CAMERA_ENV_DEVICE, CAMERA_V4L2_DEVICE_DEFAULT),
        stream_width_, stream_height_, stream_fps_,
        env_or_default(CAMERA_ENV_IQ_DIR, CAMERA_IQ_DIR_DEFAULT),
        env_or_default(CAMERA_ENV_IQ_FILE, CAMERA_IQ_FILE_DEFAULT),
        CAMERA_SHM_NAME_DEFAULT, frame_ntf_enabled_ ? 1 : 0);
    return true;
}

void CameraService::shutdown()
{
    stop_streaming();
    subs_.clear();
}

ViCaptureConfig CameraService::build_capture_config()
{
    ViCaptureConfig cfg;
    cfg.video_dev = env_or_default(CAMERA_ENV_DEVICE, CAMERA_V4L2_DEVICE_DEFAULT);
    cfg.iq_dir    = env_or_default(CAMERA_ENV_IQ_DIR, CAMERA_IQ_DIR_DEFAULT);
    cfg.iq_file   = env_or_default(CAMERA_ENV_IQ_FILE, CAMERA_IQ_FILE_DEFAULT);
    cfg.width     = stream_width_;
    cfg.height    = stream_height_;
    cfg.fps       = stream_fps_;
    cfg.buf_count = CAMERA_V4L2_BUF_COUNT;
    return cfg;
}

uint32_t CameraService::read_fps_from_env()
{
    const char* env = std::getenv(CAMERA_ENV_FPS);
    if(!env || !env[0]) {
        return CAMERA_FPS_DEFAULT;
    }
    const long v = std::strtol(env, nullptr, 10);
    if(v <= 0 || v > 120) {
        return CAMERA_FPS_DEFAULT;
    }
    return static_cast<uint32_t>(v);
}

void CameraService::subscribe_messages()
{
    auto& port = middleware::MsgPort::instance();

    subs_.push_back(port.subscribe(
        middleware::MsgType::UI_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::UI_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::APP_Print_Video_Session_Start_Ntf,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(port.subscribe(
        middleware::MsgType::APP_Print_Layer_Snap_Ntf,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(port.subscribe(
        middleware::MsgType::APP_Print_Video_Session_Finish_Ntf,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(port.subscribe(
        middleware::MsgType::APP_Print_Video_Session_Abort_Ntf,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
}

void CameraService::handle_message(const middleware::Message& msg)
{
    switch(msg.type) {
    case middleware::MsgType::UI_Camera_Stream_Start_Req:
        handle_stream_start(msg);
        break;
    case middleware::MsgType::UI_Camera_Stream_Stop_Req:
        handle_stream_stop(msg);
        break;
    case middleware::MsgType::APP_Print_Video_Session_Start_Ntf:
    case middleware::MsgType::APP_Print_Layer_Snap_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Finish_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Abort_Ntf:
        handle_print_video(msg);
        break;
    default:
        break;
    }
}

void CameraService::handle_print_video(const middleware::Message& msg)
{
    switch(msg.type) {
    case middleware::MsgType::APP_Print_Video_Session_Start_Ntf: {
        middleware::PrintVideoSessionPayload info{};
        if(!read_inline_pod(msg.payload, info)) {
            return;
        }
        print_video_.set_stream_geometry(stream_width_, stream_height_);
        print_video_.on_session_start(info);
        break;
    }
    case middleware::MsgType::APP_Print_Layer_Snap_Ntf: {
        middleware::PrintVideoLayerSnapPayload snap{};
        if(!read_inline_pod(msg.payload, snap)) {
            return;
        }
        print_video_.on_layer_snap(snap);
        break;
    }
    case middleware::MsgType::APP_Print_Video_Session_Finish_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Abort_Ntf: {
        middleware::PrintVideoSessionPayload info{};
        if(!read_inline_pod(msg.payload, info)) {
            return;
        }
        if(msg.type == middleware::MsgType::APP_Print_Video_Session_Finish_Ntf) {
            print_video_.on_session_finish(info);
        } else {
            print_video_.on_session_abort(info);
        }
        break;
    }
    default:
        break;
    }
}

void CameraService::handle_stream_start(const middleware::Message& msg)
{
    if(stream_running_.load()) {
        stop_streaming();
    }

    const bool ok = start_streaming();
    send_stream_start_rsp(msg, ok);

    if(!ok) {
        UPC_LOG_CAMERA_SERVICE(Error, "stream start failed");
    } else {
        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps shm=%s stride_y=%u",
            stream_width_, stream_height_, stream_fps_,
            CAMERA_SHM_NAME_DEFAULT, shm_stride_y_);
    }
}

void CameraService::handle_stream_stop(const middleware::Message& msg)
{
    stop_streaming();
    send_stream_stop_done(msg);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stopped");
}

bool CameraService::start_streaming()
{
    const ViCaptureConfig cfg = build_capture_config();
    if(!capture_.open(cfg)) {
        UPC_LOG_CAMERA_SERVICE(Error, "ViCapture open failed");
        return false;
    }

    /* SHM 存紧凑 NV12（行 stride = width）；勿用 V4L2 对齐 stride */
    shm_stride_y_ = stream_width_;
    const size_t frame_bytes = capture_.frame_bytes();

    producer_.destroy();

    const upc_ipc::IpcShmRingCreateParams params{
        CAMERA_SHM_NAME_DEFAULT,
        stream_width_,
        stream_height_,
        shm_stride_y_,
        CAMERA_SHM_SLOT_COUNT,
        upc_ipc::IpcShmPixelFormat::YUV420SP,
    };

    upc_ipc::IpcShmError err = producer_.create(params);
    if(err == upc_ipc::IpcShmError::AlreadyExists) {
        upc_ipc::IpcShmRegion tmp;
        if(tmp.open(CAMERA_SHM_NAME_DEFAULT, false) == upc_ipc::IpcShmError::Ok) {
            tmp.unlink_name();
            tmp.close();
        }
        err = producer_.create(params);
    }
    if(err != upc_ipc::IpcShmError::Ok) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "shm create failed err=%d",
                               static_cast<int>(err));
        return false;
    }

    if(const upc_ipc::IpcShmGlobalHeader* hdr = producer_.header()) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "SHM created name=%s %ux%u stride_y=%u slots=%u slot_stride=%u "
            "frame_bytes=%zu",
            CAMERA_SHM_NAME_DEFAULT, hdr->width, hdr->height, hdr->stride_y,
            hdr->slot_count, hdr->slot_stride_bytes, frame_bytes);
    }

    stop_stream_.store(false);
    stream_running_.store(true);
    print_video_.set_stream_geometry(stream_width_, stream_height_);
    stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);
    return true;
}

void CameraService::stop_streaming()
{
    stop_stream_.store(true);
    if(stream_thread_.joinable()) {
        stream_thread_.join();
    }
    stream_running_.store(false);
    producer_.destroy();
    capture_.close();
}

void CameraService::stream_thread_fn()
{
    const size_t compact_bytes =
        static_cast<size_t>(stream_width_) * stream_height_ * 3u / 2u;
    const auto frame_period =
        std::chrono::microseconds(1000000u / (stream_fps_ > 0 ? stream_fps_ : 1u));

    using clock = std::chrono::steady_clock;
    auto stats_since   = clock::now();
    uint32_t frames_1s = 0;
    uint32_t last_seq  = 0;
    uint32_t last_slot = 0;
    bool logged_first_commit = false;

    while(!stop_stream_.load(std::memory_order_relaxed)) {
        const auto now = clock::now();
        if(now - stats_since >= std::chrono::seconds(1)) {
            UPC_LOG_CAMERA_SERVICE(Debug,
                "stream 1s: committed=%u last_seq=%u last_slot=%u",
                frames_1s, last_seq, last_slot);
            frames_1s   = 0;
            stats_since = now;
        }

        const auto tick_start = clock::now();

        std::optional<uint32_t> slot = producer_.acquire_write_slot();
        if(!slot) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        uint8_t* payload = producer_.slot_payload(*slot);
        if(!payload ||
           !capture_.capture_frame(payload, compact_bytes, 2000)) {
            producer_.cancel_write_slot(*slot);
            if(stop_stream_.load()) {
                break;
            }
            continue;
        }

        const uint32_t seq =
            producer_.commit_slot(*slot, monotonic_us());
        if(!logged_first_commit && payload) {
            const size_t y_size =
                static_cast<size_t>(stream_width_) * stream_height_;
            UPC_LOG_CAMERA_SERVICE(Info,
                "first frame committed slot=%u seq=%u "
                "Y[0]=%u Y[mid]=%u UV[0]=%u UV[1]=%u bytes=%zu",
                *slot, seq,
                payload[0],
                payload[y_size / 2u + stream_width_ / 2u],
                payload[y_size], payload[y_size + 1u], compact_bytes);
            logged_first_commit = true;
        }
        if(frame_ntf_enabled_) {
            notify_frame(seq);
        }
        frames_1s++;
        last_seq  = seq;
        last_slot = *slot;

        const auto elapsed = clock::now() - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }
}

void CameraService::send_stream_start_rsp(const middleware::Message& req, bool ok)
{
    middleware::CameraStreamInfoPayload info{};
    std::snprintf(info.shm_name, sizeof(info.shm_name), "%s",
                  CAMERA_SHM_NAME_DEFAULT);
    info.width    = stream_width_;
    info.height   = stream_height_;
    info.stride_y = shm_stride_y_;
    info.format   = static_cast<uint32_t>(upc_ipc::IpcShmPixelFormat::YUV420SP);
    info.fps      = stream_fps_;
    info.ok       = ok ? 1 : 0;

    middleware::Message rsp(
        middleware::MsgType::APP_Camera_Stream_Start_Rsp,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&info), sizeof(info));
    middleware::MsgPort::instance().publish_async(rsp);
}

void CameraService::send_stream_stop_done(const middleware::Message& req)
{
    middleware::Message done(
        middleware::MsgType::APP_Camera_Stream_Stop_Done,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    middleware::MsgPort::instance().publish_async(done);
}

void CameraService::notify_frame(uint32_t seq)
{
    middleware::CameraFrameNtfPayload ntf{};
    ntf.seq = seq;

    middleware::Message msg(
        middleware::MsgType::APP_Camera_Frame_Ntf,
        middleware::ModuleId::APP_CAMERA,
        middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    middleware::MsgPort::instance().publish_async(msg);
}

} // namespace upc::camera
