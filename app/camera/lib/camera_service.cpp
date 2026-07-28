#include "camera_service.hpp"

#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "msg_runtime.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace upc::camera {

namespace {

constexpr const char* kPhotoOutDir = "/home/photo";
constexpr const char* kUiShmName = CAMERA_SHM_NAME_DEFAULT;
constexpr const char* kVisualShmName = "/upc_camera_preview_visual";
constexpr int kPeerWaitSlotEmptyMs = 1000;

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

std::string format_stamp_local()
{
    const auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    if(localtime_r(&t, &tm_buf) == nullptr) {
        return "19700101_000000_000";
    }
    const uint32_t ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d_%03u",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
    return buf;
}

void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y, const uint8_t* src_uv,
                     uint32_t src_stride_y, uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || src_stride_y == 0 || width == 0 ||
       height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    uint8_t* dst_uv = dst + y_size;

    if(src_stride_y == width) {
        /* 注意：mplane 场景下 Y/UV 可能不是连续映射，不能从 src_y 一次性连拷。 */
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }

    for(uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * width,
                    src_y + static_cast<size_t>(row) * src_stride_y,
                    width);
    }
    for(uint32_t row = 0; row < height / 2u; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * width,
                    src_uv + static_cast<size_t>(row) * src_stride_y,
                    width);
    }
}

int run_shell_cmd(const char* cmd, const char* log_tag)
{
    if(!cmd || !cmd[0]) {
        return -1;
    }
    UPC_LOG_CAMERA_SERVICE(Info, "snapshot %s: %s",
                           log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_CAMERA_SERVICE(Error, "snapshot %s failed rc=%d",
                               log_tag ? log_tag : "cmd", rc);
    }
    return rc;
}

} // namespace

CameraService::~CameraService()
{
    shutdown();
}

bool CameraService::init()
{
    stream_fps_            = read_fps_from_env();
    stream_warmup_frames_  = read_warmup_frames_from_env();

    const char* ntf_env = std::getenv(CAMERA_ENV_FRAME_NTF);
    if(ntf_env && ntf_env[0]) {
        frame_ntf_enabled_ = (ntf_env[0] != '0');
    }

    subscribe_messages();
    print_video_.set_nv12_snapshot_provider(
        [this](std::vector<uint8_t>& out) {
            return copy_latest_nv12_snapshot(out);
        });
    UPC_LOG_CAMERA_SERVICE(Info,
        "init device=%s %ux%u@%u warmup_frames=%u iq=%s/%s shm=%s frame_ntf=%d",
        env_or_default(CAMERA_ENV_DEVICE, CAMERA_V4L2_DEVICE_DEFAULT),
        stream_width_, stream_height_, stream_fps_, stream_warmup_frames_,
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

uint32_t CameraService::read_warmup_frames_from_env()
{
    const char* env = std::getenv(CAMERA_ENV_WARMUP_FRAMES);
    if(!env || !env[0]) {
        return CAMERA_WARMUP_FRAMES_DEFAULT;
    }
    const long v = std::strtol(env, nullptr, 10);
    if(v < 0 || v > 120) {
        return CAMERA_WARMUP_FRAMES_DEFAULT;
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
        middleware::MsgType::UI_Camera_Snapshot_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::VISUAL_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::VISUAL_Camera_Stream_Stop_Req,
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
    subs_.push_back(port.subscribe(
        middleware::MsgType::APP_Clean_Video_Session_Start_Ntf,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(port.subscribe(
        middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf,
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
    case middleware::MsgType::VISUAL_Camera_Stream_Start_Req:
        handle_visual_stream_start(msg);
        break;
    case middleware::MsgType::VISUAL_Camera_Stream_Stop_Req:
        handle_visual_stream_stop(msg);
        break;
    case middleware::MsgType::UI_Camera_Snapshot_Req:
        handle_snapshot_req(msg);
        break;
    case middleware::MsgType::APP_Print_Video_Session_Start_Ntf:
    case middleware::MsgType::APP_Print_Layer_Snap_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Finish_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Abort_Ntf:
        handle_print_video(msg);
        break;
    case middleware::MsgType::APP_Clean_Video_Session_Start_Ntf:
    case middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf:
        handle_clean_video(msg);
        break;
    default:
        break;
    }
}

void CameraService::handle_snapshot_req(const middleware::Message& msg)
{
    std::string out_path;
    const bool ok = save_latest_png_snapshot(out_path);
    send_snapshot_rsp(msg, ok, ok ? out_path.c_str() : "");
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

void CameraService::handle_clean_video(const middleware::Message& msg)
{
    if(msg.type == middleware::MsgType::APP_Clean_Video_Session_Start_Ntf) {
        middleware::CleanVideoSessionStartPayload req{};
        if(!read_inline_pod(msg.payload, req)) {
            return;
        }
        clean_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
        clean_video_.on_session_start(req);
        return;
    }
    if(msg.type == middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf) {
        middleware::CleanVideoSessionStopPayload req{};
        if(!read_inline_pod(msg.payload, req)) {
            return;
        }
        clean_video_.on_session_stop(req);
    }
}

void CameraService::handle_stream_start(const middleware::Message& msg)
{
    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp(msg, false);
            UPC_LOG_CAMERA_SERVICE(Error, "stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        print_video_.set_stream_geometry(stream_width_, stream_height_);
        clean_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps stride_y=%u aligned=%d",
            stream_width_, stream_height_, stream_fps_,
            dma_source_.dma_stride_y(),
            dma_source_.stride_aligned() ? 1 : 0);
    }

    const bool ok = ensure_stream_peer(
        msg, kUiShmName, middleware::MsgType::APP_Camera_Stream_Start_Rsp);
    if(!ok) {
        send_stream_start_rsp(msg, false);
    } else {
        send_stream_start_rsp(msg, true);
    }
}

void CameraService::handle_visual_stream_start(const middleware::Message& msg)
{
    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp_with_type(
                msg, false, middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp,
                kVisualShmName);
            UPC_LOG_CAMERA_SERVICE(Error, "visual stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        print_video_.set_stream_geometry(stream_width_, stream_height_);
        clean_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps stride_y=%u aligned=%d",
            stream_width_, stream_height_, stream_fps_,
            dma_source_.dma_stride_y(),
            dma_source_.stride_aligned() ? 1 : 0);
    }

    const bool ok = ensure_stream_peer(
        msg, kVisualShmName, middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp);
    send_stream_start_rsp_with_type(
        msg, ok, middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp,
        kVisualShmName);
}

void CameraService::handle_stream_stop(const middleware::Message& msg)
{
    remove_stream_peer(msg.sender_id);
    if(!has_stream_peers()) {
        stop_streaming();
    }
    send_stream_stop_done(msg);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop ui sender=%u",
                           static_cast<unsigned>(msg.sender_id));
}

void CameraService::handle_visual_stream_stop(const middleware::Message& msg)
{
    remove_stream_peer(msg.sender_id);
    if(!has_stream_peers()) {
        stop_streaming();
    }
    send_stream_stop_done_with_type(
        msg, middleware::MsgType::APP_Visual_Camera_Stream_Stop_Done);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop visual sender=%u",
                           static_cast<unsigned>(msg.sender_id));
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

    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        stream_peers_.clear();
    }

    return true;
}

void CameraService::stop_streaming()
{
    stop_stream_.store(true);
    if(stream_thread_.joinable()) {
        stream_thread_.join();
    }
    stream_running_.store(false);
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        for(auto& kv : stream_peers_) {
            if(kv.second) {
                kv.second->producer.destroy();
            }
        }
        stream_peers_.clear();
    }
    dma_source_.shutdown();
    capture_.close();
    {
        std::lock_guard<std::mutex> lock(latest_nv12_mtx_);
        latest_nv12_snapshot_.clear();
        latest_nv12_valid_ = false;
    }
}

void CameraService::update_latest_nv12_snapshot(const uint8_t* src_y,
                                                const uint8_t* src_uv,
                                                uint32_t src_stride_y,
                                                uint32_t width,
                                                uint32_t height)
{
    if(!src_y || !src_uv || width == 0 || height == 0) {
        return;
    }
    const size_t need =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
    std::lock_guard<std::mutex> lock(latest_nv12_mtx_);
    latest_nv12_snapshot_.resize(need);
    copy_nv12_tight(latest_nv12_snapshot_.data(), src_y, src_uv, src_stride_y,
                    width, height);
    latest_nv12_valid_ = true;
}

bool CameraService::copy_latest_nv12_snapshot(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lock(latest_nv12_mtx_);
    if(!latest_nv12_valid_ || latest_nv12_snapshot_.empty()) {
        return false;
    }
    out = latest_nv12_snapshot_;
    return true;
}

bool CameraService::save_latest_png_snapshot(std::string& out_path) const
{
    out_path.clear();
    std::vector<uint8_t> nv12;
    if(!copy_latest_nv12_snapshot(nv12) || nv12.empty()) {
        return false;
    }

    if(mkdir(kPhotoOutDir, 0755) != 0 && errno != EEXIST) {
        UPC_LOG_CAMERA_SERVICE(Error, "mkdir %s failed: %s",
                               kPhotoOutDir, std::strerror(errno));
        return false;
    }

    const std::string stamp = format_stamp_local();
    const std::string name = "photo_" + stamp + "_" +
                             std::to_string(stream_width_) + "x" +
                             std::to_string(stream_height_) + ".png";
    const std::string out_png_path = std::string(kPhotoOutDir) + "/" + name;
    const std::string tmp_nv12_path =
        "/tmp/camera_snapshot_" + stamp + "_" + std::to_string(monotonic_us()) + ".nv12";

    FILE* fp = std::fopen(tmp_nv12_path.c_str(), "wb");
    if(!fp) {
        UPC_LOG_CAMERA_SERVICE(Error, "open temp nv12 failed %s: %s",
                               tmp_nv12_path.c_str(), std::strerror(errno));
        return false;
    }
    const bool ok = std::fwrite(nv12.data(), 1, nv12.size(), fp) == nv12.size();
    if(std::fclose(fp) != 0 || !ok) {
        UPC_LOG_CAMERA_SERVICE(Error, "write temp nv12 failed %s", tmp_nv12_path.c_str());
        (void)std::remove(tmp_nv12_path.c_str());
        return false;
    }

    char cmd[1024];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -i '%s' "
        "-frames:v 1 -c:v png '%s'",
        CAMERA_FFMPEG_BIN, stream_width_, stream_height_,
        tmp_nv12_path.c_str(), out_png_path.c_str());
    const bool png_ok = (run_shell_cmd(cmd, "ffmpeg png") == 0);
    (void)std::remove(tmp_nv12_path.c_str());
    if(!png_ok) {
        return false;
    }

    out_path = out_png_path;
    UPC_LOG_CAMERA_SERVICE(Info, "snapshot saved path=%s src_nv12_bytes=%zu",
                           out_path.c_str(), nv12.size());
    return true;
}

bool CameraService::ensure_stream_peer(const middleware::Message& req,
                                       const char* shm_name,
                                       middleware::MsgType rsp_type)
{
    if(!shm_name || !shm_name[0]) {
        return false;
    }
    const uint32_t key = static_cast<uint32_t>(req.sender_id);
    std::lock_guard<std::mutex> lock(stream_peers_mtx_);
    auto it = stream_peers_.find(key);
    if(it != stream_peers_.end() && it->second) {
        return true;
    }

    auto peer = std::make_shared<StreamPeerSession>();
    peer->peer_id = req.sender_id;
    peer->shm_name = shm_name;
    const upc_ipc::IpcShmError err = peer->producer.create(
        peer->shm_name.c_str(),
        stream_width_,
        stream_height_,
        dma_source_.dma_stride_y(),
        upc_ipc::kDmaFrameFormatNv12,
        CAMERA_SHM_SLOT_COUNT);
    if(err != upc_ipc::IpcShmError::OK) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "peer shm create fail type=%u sender=%u name=%s err=%d",
            static_cast<unsigned>(rsp_type),
            key,
            peer->shm_name.c_str(),
            static_cast<int>(err));
        return false;
    }
    stream_peers_[key] = peer;
    UPC_LOG_CAMERA_SERVICE(Info,
        "peer shm ready sender=%u name=%s %ux%u stride_y=%u slots=%u",
        key, peer->shm_name.c_str(), stream_width_, stream_height_,
        dma_source_.dma_stride_y(), CAMERA_SHM_SLOT_COUNT);
    return true;
}

void CameraService::remove_stream_peer(middleware::ModuleId peer)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        const uint32_t key = static_cast<uint32_t>(peer);
        auto it = stream_peers_.find(key);
        if(it != stream_peers_.end()) {
            stream_peers_.erase(it);
            removed = true;
        }
    }
    if(removed) {
        UPC_LOG_CAMERA_SERVICE(Info, "peer shm removed sender=%u",
                               static_cast<unsigned>(peer));
    }
}

bool CameraService::has_stream_peers() const
{
    std::lock_guard<std::mutex> lock(stream_peers_mtx_);
    return !stream_peers_.empty();
}

void CameraService::stream_thread_fn()
{
    const auto frame_period =
        std::chrono::microseconds(1000000u / (stream_fps_ > 0 ? stream_fps_ : 1u));

    using clock = std::chrono::steady_clock;
    auto stats_since   = clock::now();
    uint32_t frames_1s       = 0;
    uint32_t last_seq        = 0;
    uint32_t last_slot       = 0;
    uint32_t slot_fail_1s    = 0;
    uint32_t dequeue_fail_1s = 0;
    uint32_t timeout_1s      = 0;
    bool logged_first_commit = false;
    uint32_t warmup_left = stream_warmup_frames_;

    if(warmup_left > 0) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "AE warmup: discarding first %u frames before SHM commit",
            warmup_left);
    }

    while(!stop_stream_.load(std::memory_order_relaxed)) {
        const auto now = clock::now();
        if(now - stats_since >= std::chrono::seconds(1)) {
            /* 流诊断受 UPC_MOD_CAMERA_PREVIEW 控制，便于与 UI decode 1s 对照 */
            UPC_LOG_CAMERA_PREVIEW(Debug,
                "stream 1s: committed=%u last_seq=%u last_slot=%u "
                "slot_fail=%u dequeue_fail=%u release_timeout=%u",
                frames_1s, last_seq, last_slot,
                slot_fail_1s, dequeue_fail_1s, timeout_1s);
            frames_1s = slot_fail_1s = dequeue_fail_1s = timeout_1s = 0;
            stats_since = now;
        }

        const auto tick_start = clock::now();

        /* AE 预热：只 dequeue/queue 喂 ISP，不占 SHM slot、不 commit。 */
        if(warmup_left > 0) {
            std::optional<uint32_t> buf_idx = capture_.dequeue_frame(2000);
            if(!buf_idx) {
                if(stop_stream_.load()) {
                    break;
                }
                continue;
            }
            capture_.queue_frame(*buf_idx);
            --warmup_left;
            if(warmup_left == 0) {
                UPC_LOG_CAMERA_SERVICE(Info,
                    "AE warmup done, committing frames to SHM");
            }
            const auto elapsed = clock::now() - tick_start;
            if(elapsed < frame_period) {
                std::this_thread::sleep_for(frame_period - elapsed);
            }
            continue;
        }

        std::optional<uint32_t> buf_idx = capture_.dequeue_frame(2000);
        if(!buf_idx) {
            ++dequeue_fail_1s;
            if(stop_stream_.load()) {
                break;
            }
            continue;
        }

        uint32_t slot_buf_idx = *buf_idx;
        const uint8_t* src_y = capture_.buffer_y(*buf_idx);
        const uint8_t* src_uv = capture_.buffer_uv(*buf_idx);
        if(src_y && src_uv) {
            update_latest_nv12_snapshot(src_y, src_uv, capture_.stride_y(),
                                        stream_width_, stream_height_);
            clean_video_.on_frame_nv12(src_y, src_uv, capture_.stride_y());
        }

        if(!dma_source_.stride_aligned()) {
            const int padded_idx = dma_source_.acquire_padded();
            if(padded_idx < 0) {
                capture_.queue_frame(*buf_idx);
                ++slot_fail_1s;
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

        struct PeerCommit {
            std::shared_ptr<StreamPeerSession> peer;
            uint32_t slot_idx;
            uint32_t seq;
        };
        std::vector<PeerCommit> commits;
        {
            std::vector<std::shared_ptr<StreamPeerSession>> peers;
            {
                std::lock_guard<std::mutex> lock(stream_peers_mtx_);
                peers.reserve(stream_peers_.size());
                for(auto& kv : stream_peers_) {
                    if(kv.second) {
                        peers.push_back(kv.second);
                    }
                }
            }
            if(peers.empty()) {
                ++slot_fail_1s;
            } else {
                for(const auto& peer : peers) {
                    std::optional<uint32_t> slot = peer->producer.acquire_write_slot();
                    if(!slot) {
                        ++slot_fail_1s;
                        continue;
                    }
                    const uint32_t seq =
                        peer->producer.commit_slot(*slot, monotonic_us(), slot_buf_idx);
                    if(seq == 0) {
                        peer->producer.cancel_write_slot(*slot);
                        ++slot_fail_1s;
                        continue;
                    }
                    commits.push_back(PeerCommit{peer, *slot, seq});
                }
            }
        }

        if(!logged_first_commit && !commits.empty()) {
            UPC_LOG_CAMERA_SERVICE(Info,
                "first frame committed peers=%zu seq=%u buf_idx=%u aligned=%d",
                commits.size(), commits[0].seq, slot_buf_idx,
                dma_source_.stride_aligned() ? 1 : 0);
            logged_first_commit = true;
        }
        if(frame_ntf_enabled_ && !commits.empty()) {
            notify_frame(commits[0].seq);
        }
        if(!commits.empty()) {
            frames_1s++;
            last_seq  = commits[0].seq;
            last_slot = commits[0].slot_idx;
        }

        bool all_released = true;
        for(const auto& c : commits) {
            const bool slot_released =
                c.peer->producer.wait_slot_empty(c.slot_idx, kPeerWaitSlotEmptyMs);
            if(!slot_released) {
                all_released = false;
                ++timeout_1s;
                UPC_LOG_CAMERA_SERVICE(Warn,
                    "slot release timeout sender=%u slot=%u seq=%u buf_idx=%u (backpressure)",
                    static_cast<unsigned>(c.peer->peer_id),
                    c.slot_idx, c.seq, slot_buf_idx);
            }
        }

        // 即使 UI 释放超时，也要把底层 buffer 还回去，避免 V4L2 / padding pool 泄漏。
        // slot 本身仍保持 READY/READING，等 Consumer 最终 release_slot 后恢复 EMPTY。
        if(dma_source_.stride_aligned()) {
            capture_.queue_frame(slot_buf_idx);
        } else {
            dma_source_.release_padded(slot_buf_idx);
        }

        if(!all_released) {
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
    send_stream_start_rsp_with_type(req, ok,
        middleware::MsgType::APP_Camera_Stream_Start_Rsp, kUiShmName);
}

void CameraService::send_stream_start_rsp_with_type(
    const middleware::Message& req, bool ok,
    middleware::MsgType rsp_type, const char* shm_name)
{
    middleware::CameraStreamInfoPayload info{};
    std::snprintf(info.shm_name, sizeof(info.shm_name), "%s",
                  shm_name ? shm_name : "");
    info.width         = stream_width_;
    info.height        = stream_height_;
    info.stride_y      = dma_source_.dma_stride_y();
    info.format        = upc_ipc::kDmaFrameFormatNv12;
    info.fps           = stream_fps_;
    info.dma_buf_count = ok ? dma_source_.buffer_count() : 0;
    info.dma_mode      = ok ? 1 : 0;
    info.ok            = ok ? 1 : 0;

    middleware::Message rsp(
        rsp_type,
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
    send_stream_stop_done_with_type(
        req, middleware::MsgType::APP_Camera_Stream_Stop_Done);
}

void CameraService::send_stream_stop_done_with_type(
    const middleware::Message& req, middleware::MsgType done_type)
{
    middleware::Message done(
        done_type,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    middleware::MsgPort::instance().publish_async(done);
}

void CameraService::send_snapshot_rsp(const middleware::Message& req, bool ok,
                                      const char* path)
{
    middleware::CameraSnapshotRspPayload rsp{};
    rsp.ok = ok ? 1 : 0;
    if(path && path[0]) {
        std::snprintf(rsp.path, sizeof(rsp.path), "%s", path);
    }
    middleware::Message msg(
        middleware::MsgType::APP_Camera_Snapshot_Rsp,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    middleware::MsgPort::instance().publish_async(msg);
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
