#include "camera_service.hpp"

#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "msg_runtime.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace upc::camera {

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
        return;
    }

    stop_stream_.store(false);
    stream_running_.store(true);
    print_video_.set_stream_geometry(stream_width_, stream_height_);
    print_video_.set_dma_fd_lookup(
        [this](uint32_t idx) { return dma_source_.fd_for_buffer_idx(idx); });
    stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

    UPC_LOG_CAMERA_SERVICE(Info,
        "streaming live %ux%u @ %u fps shm=%s stride_y=%u aligned=%d",
        stream_width_, stream_height_, stream_fps_,
        CAMERA_SHM_NAME_DEFAULT, dma_source_.dma_stride_y(),
        dma_source_.stride_aligned() ? 1 : 0);
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

    stream_width_  = capture_.width();
    stream_height_ = capture_.height();

    const std::vector<int>& capture_fds = capture_.dma_fds();
    if(capture_fds.empty()) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "no V4L2 dma fds exported");
        return false;
    }

    if(!dma_source_.prepare(stream_width_, stream_height_, capture_.stride_y(),
                            capture_fds)) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "dma frame source prepare failed");
        return false;
    }

    producer_.destroy();

    const upc_ipc::IpcShmError err = producer_.create(
        CAMERA_SHM_NAME_DEFAULT,
        stream_width_,
        stream_height_,
        dma_source_.dma_stride_y(),
        upc_ipc::kDmaFrameFormatNv12,
        CAMERA_SHM_SLOT_COUNT);

    if(err != upc_ipc::IpcShmError::OK) {
        dma_source_.shutdown();
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "dma frame ring create failed err=%d",
                               static_cast<int>(err));
        return false;
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "SHM created name=%s %ux%u stride_y=%u slots=%u dma_buf_count=%u aligned=%d",
        CAMERA_SHM_NAME_DEFAULT, stream_width_, stream_height_,
        dma_source_.dma_stride_y(), CAMERA_SHM_SLOT_COUNT,
        dma_source_.buffer_count(),
        dma_source_.stride_aligned() ? 1 : 0);

    return true;
}

void CameraService::stop_streaming()
{
    stop_stream_.store(true);
    if(stream_thread_.joinable()) {
        stream_thread_.join();
    }
    stream_running_.store(false);

    dma_source_.shutdown();
    producer_.destroy();
    capture_.close();
}

void CameraService::stream_thread_fn()
{
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

        std::optional<uint32_t> buf_idx = capture_.dequeue_frame(2000);
        if(!buf_idx) {
            producer_.cancel_write_slot(*slot);
            if(stop_stream_.load()) {
                break;
            }
            continue;
        }

        uint32_t slot_buf_idx = *buf_idx;

        if(!dma_source_.stride_aligned()) {
            const int padded_idx = dma_source_.acquire_padded();
            if(padded_idx < 0) {
                capture_.queue_frame(*buf_idx);
                producer_.cancel_write_slot(*slot);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            dma_source_.copy_padded_frame(
                static_cast<uint32_t>(padded_idx),
                capture_.buffer_y(*buf_idx),
                capture_.buffer_uv(*buf_idx),
                capture_.stride_y(),
                stream_width_,
                stream_height_);
            capture_.queue_frame(*buf_idx);
            slot_buf_idx = static_cast<uint32_t>(padded_idx);
        }

        const uint32_t seq =
            producer_.commit_slot(*slot, monotonic_us(), slot_buf_idx);

        if(!logged_first_commit) {
            UPC_LOG_CAMERA_SERVICE(Info,
                "first frame committed slot=%u seq=%u buf_idx=%u aligned=%d",
                *slot, seq, slot_buf_idx,
                dma_source_.stride_aligned() ? 1 : 0);
            logged_first_commit = true;
        }
        if(frame_ntf_enabled_) {
            notify_frame(seq);
        }
        frames_1s++;
        last_seq  = seq;
        last_slot = *slot;

        const bool slot_released =
            producer_.wait_slot_empty(*slot, 1000);
        if(!slot_released) {
            UPC_LOG_CAMERA_SERVICE(Warn,
                "slot release timeout slot=%u seq=%u buf_idx=%u (backpressure)",
                *slot, seq, slot_buf_idx);
        }

        // 即使 UI 释放超时，也要把底层 buffer 还回去，避免 V4L2 / padding pool 泄漏。
        // slot 本身仍保持 READY/READING，等 Consumer 最终 release_slot 后恢复 EMPTY。
        if(dma_source_.stride_aligned()) {
            capture_.queue_frame(slot_buf_idx);
        } else {
            dma_source_.release_padded(slot_buf_idx);
        }

        if(!slot_released) {
            continue;
        }

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
    info.width         = stream_width_;
    info.height        = stream_height_;
    info.stride_y      = dma_source_.dma_stride_y();
    info.format        = upc_ipc::kDmaFrameFormatNv12;
    info.fps           = stream_fps_;
    info.dma_buf_count = ok ? dma_source_.buffer_count() : 0;
    info.dma_mode      = ok ? 1 : 0;
    info.ok            = ok ? 1 : 0;

    middleware::Message rsp(
        middleware::MsgType::APP_Camera_Stream_Start_Rsp,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&info), sizeof(info));

    auto& port = middleware::MsgPort::instance();
    if(ok && !dma_source_.ipc_fds().empty()) {
        if(!port.publish_to_peer_with_fds(rsp, dma_source_.ipc_fds())) {
            UPC_LOG_CAMERA_SERVICE(Error,
                "stream start rsp with fds failed (IPC SCM_RIGHTS)");
        }
    } else {
        port.publish_async(rsp);
    }
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
