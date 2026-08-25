#include "camera_service.hpp"

#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "msg_runtime.hpp"
#include "rs_log.hpp"
#include "dma_heap_buffer.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <algorithm>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#if defined(UPC_CAMERA_HAS_LIBAV) && UPC_CAMERA_HAS_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace upc::camera {

namespace {

constexpr const char* kUiShmName = CAMERA_SHM_NAME_DEFAULT;
constexpr const char* kVisualShmName = "/upc_camera_preview_visual";
constexpr const char* kMediaShmName = "/upc_camera_preview_media";
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

std::string resolve_demo_video_path(const std::string& basename)
{
    const std::string root = "/home/test";
    if(!basename.empty()) {
        if(basename.front() == '/' && ::access(basename.c_str(), R_OK) == 0) {
            return basename;
        }
        const std::string candidate = root + "/" + basename;
        if(::access(candidate.c_str(), R_OK) == 0) {
            return candidate;
        }
    }
    const std::string fixed = root + "/demo.mp4";
    if(::access(fixed.c_str(), R_OK) == 0) {
        return fixed;
    }
    DIR* dir = ::opendir(root.c_str());
    if(!dir) {
        return {};
    }
    std::string first_mp4;
    while(dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if(!name || name[0] == '.') {
            continue;
        }
        const size_t len = std::strlen(name);
        if(len < 4) {
            continue;
        }
        const char* ext = name + len - 4;
        if(std::strcmp(ext, ".mp4") == 0) {
            first_mp4 = root + "/" + name;
            break;
        }
    }
    ::closedir(dir);
    return first_mp4;
}

void dma_buf_sync_end_write(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

#if defined(UPC_CAMERA_HAS_LIBAV) && UPC_CAMERA_HAS_LIBAV
class DemoVideoDecoder {
public:
    ~DemoVideoDecoder() { close(); }

    bool open(const char* path, uint32_t max_edge)
    {
        close();
        if(!path || !path[0]) {
            return false;
        }
        fmt_ = avformat_alloc_context();
        if(!fmt_) {
            return false;
        }
        if(avformat_open_input(&fmt_, path, nullptr, nullptr) < 0) {
            close();
            return false;
        }
        if(avformat_find_stream_info(fmt_, nullptr) < 0) {
            close();
            return false;
        }
        stream_index_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if(stream_index_ < 0) {
            close();
            return false;
        }
        stream_ = fmt_->streams[stream_index_];
        const AVCodec* codec_def = avcodec_find_decoder(stream_->codecpar->codec_id);
        if(!codec_def) {
            close();
            return false;
        }
        codec_ = avcodec_alloc_context3(codec_def);
        if(!codec_) {
            close();
            return false;
        }
        if(avcodec_parameters_to_context(codec_, stream_->codecpar) < 0) {
            close();
            return false;
        }
        if(avcodec_open2(codec_, codec_def, nullptr) < 0) {
            close();
            return false;
        }
        src_w_ = codec_->width;
        src_h_ = codec_->height;
        if(src_w_ <= 0 || src_h_ <= 0) {
            close();
            return false;
        }
        int tw = src_w_;
        int th = src_h_;
        const int max_src_edge = std::max(src_w_, src_h_);
        if(max_edge > 0 && max_src_edge > static_cast<int>(max_edge)) {
            tw = std::max(1, src_w_ * static_cast<int>(max_edge) / max_src_edge);
            th = std::max(1, src_h_ * static_cast<int>(max_edge) / max_src_edge);
        }
        target_w_ = static_cast<uint32_t>(tw);
        target_h_ = static_cast<uint32_t>(th);
        AVRational fr = stream_->avg_frame_rate;
        if(fr.num <= 0 || fr.den <= 0) {
            fr = stream_->r_frame_rate;
        }
        if(fr.num > 0 && fr.den > 0) {
            const int fps = static_cast<int>(av_q2d(fr) + 0.5);
            fps_ = static_cast<uint32_t>(std::clamp(fps, 1, 60));
        } else {
            fps_ = 25;
        }
        sws_ = sws_getContext(src_w_, src_h_, codec_->pix_fmt,
                              static_cast<int>(target_w_),
                              static_cast<int>(target_h_),
                              AV_PIX_FMT_NV12,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if(!sws_) {
            close();
            return false;
        }
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if(!packet_ || !frame_) {
            close();
            return false;
        }
        y_plane_.assign(static_cast<size_t>(target_w_) * target_h_, 0);
        uv_plane_.assign(static_cast<size_t>(target_w_) * target_h_ / 2u, 0);
        return true;
    }

    void close()
    {
        y_plane_.clear();
        uv_plane_.clear();
        if(packet_) {
            av_packet_free(&packet_);
        }
        if(frame_) {
            av_frame_free(&frame_);
        }
        if(sws_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if(codec_) {
            avcodec_free_context(&codec_);
        }
        if(fmt_) {
            avformat_close_input(&fmt_);
        }
        stream_ = nullptr;
        stream_index_ = -1;
        src_w_ = src_h_ = 0;
        target_w_ = target_h_ = 0;
        fps_ = 0;
    }

    uint32_t width() const { return target_w_; }
    uint32_t height() const { return target_h_; }
    uint32_t fps() const { return fps_ > 0 ? fps_ : 25; }

    bool decode_next(uint8_t* dst_y, uint8_t* dst_uv, uint32_t dst_stride)
    {
        if(!fmt_ || !codec_ || !stream_ || !packet_ || !frame_ || !sws_) {
            return false;
        }
        while(true) {
            if(av_read_frame(fmt_, packet_) < 0) {
                if(!rewind()) {
                    return false;
                }
                continue;
            }
            if(packet_->stream_index != stream_index_) {
                av_packet_unref(packet_);
                continue;
            }
            if(avcodec_send_packet(codec_, packet_) < 0) {
                av_packet_unref(packet_);
                continue;
            }
            av_packet_unref(packet_);
            while(true) {
                const int ret = avcodec_receive_frame(codec_, frame_);
                if(ret == AVERROR(EAGAIN)) {
                    break;
                }
                if(ret == AVERROR_EOF) {
                    if(!rewind()) {
                        return false;
                    }
                    break;
                }
                if(ret < 0) {
                    return false;
                }
                uint8_t* out_data[4] = {y_plane_.data(), uv_plane_.data(), nullptr, nullptr};
                int out_linesize[4] = {static_cast<int>(target_w_), static_cast<int>(target_w_), 0, 0};
                const int h = sws_scale(sws_, frame_->data, frame_->linesize, 0, src_h_,
                                        out_data, out_linesize);
                av_frame_unref(frame_);
                if(h != static_cast<int>(target_h_)) {
                    return false;
                }
                for(uint32_t r = 0; r < target_h_; ++r) {
                    std::memcpy(dst_y + static_cast<size_t>(r) * dst_stride,
                                y_plane_.data() + static_cast<size_t>(r) * target_w_,
                                target_w_);
                }
                const uint32_t uv_h = target_h_ / 2u;
                for(uint32_t r = 0; r < uv_h; ++r) {
                    std::memcpy(dst_uv + static_cast<size_t>(r) * dst_stride,
                                uv_plane_.data() + static_cast<size_t>(r) * target_w_,
                                target_w_);
                }
                return true;
            }
        }
    }

private:
    bool rewind()
    {
        if(!fmt_ || !codec_ || stream_index_ < 0) {
            return false;
        }
        if(av_seek_frame(fmt_, stream_index_, 0, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codec_);
        return true;
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVStream* stream_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_index_ = -1;
    int src_w_ = 0;
    int src_h_ = 0;
    uint32_t target_w_ = 0;
    uint32_t target_h_ = 0;
    uint32_t fps_ = 0;
    std::vector<uint8_t> y_plane_;
    std::vector<uint8_t> uv_plane_;
};
#endif

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
        middleware::MsgType::VISUAL_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::VISUAL_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::MEDIA_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(port.subscribe(
        middleware::MsgType::MEDIA_Camera_Stream_Stop_Req,
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
    case middleware::MsgType::MEDIA_Camera_Stream_Start_Req:
        handle_media_stream_start(msg);
        break;
    case middleware::MsgType::MEDIA_Camera_Stream_Stop_Req:
        handle_media_stream_stop(msg);
        break;
    default:
        break;
    }
}

void CameraService::handle_stream_start(const middleware::Message& msg)
{
    requested_video_basename_.clear();
    middleware::CameraStreamStartReqPayload req{};
    if(msg.payload.type == middleware::MsgPayload::Type::Inline &&
       msg.payload.length >= sizeof(req)) {
        std::memcpy(&req, msg.payload.data.bytes.data(), sizeof(req));
        req.video_name[sizeof(req.video_name) - 1] = '\0';
        if(req.video_name[0]) {
            requested_video_basename_ = req.video_name;
        }
    }
    const bool wants_demo_video = !requested_video_basename_.empty();
    std::string desired_demo_path;
    if(wants_demo_video) {
        desired_demo_path = resolve_demo_video_path(requested_video_basename_);
        if(desired_demo_path.empty()) {
            UPC_LOG_CAMERA_SERVICE(Error, "requested demo video not found: %s",
                                   requested_video_basename_.c_str());
            send_stream_start_rsp(msg, false);
            return;
        }
    }

    bool need_source_switch = false;
    if(stream_running_.load()) {
        if(wants_demo_video) {
            need_source_switch =
                (stream_mode_ != StreamSourceMode::DemoVideo) ||
                (active_video_path_ != desired_demo_path);
        } else {
            need_source_switch = (stream_mode_ != StreamSourceMode::Live);
        }
    }
    if(need_source_switch) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "switch stream source: mode=%s -> %s path=%s",
            stream_mode_ == StreamSourceMode::DemoVideo ? "demo_video" : "live",
            wants_demo_video ? "demo_video" : "live",
            wants_demo_video ? desired_demo_path.c_str() : "(none)");
        stop_streaming();
    }

    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp(msg, false);
            UPC_LOG_CAMERA_SERVICE(Error, "stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming %s %ux%u @ %u fps stride_y=%u aligned=%d",
            stream_mode_ == StreamSourceMode::DemoVideo ? "demo_video" : "live",
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
    requested_video_basename_.clear();
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
        msg, middleware::MsgType::APP_Visual_Camera_Stream_Stop_Rsp);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop visual sender=%u",
                           static_cast<unsigned>(msg.sender_id));
}

void CameraService::handle_media_stream_start(const middleware::Message& msg)
{
    requested_video_basename_.clear();
    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp_with_type(
                msg, false, middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp,
                kMediaShmName);
            UPC_LOG_CAMERA_SERVICE(Error, "media stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps stride_y=%u aligned=%d",
            stream_width_, stream_height_, stream_fps_,
            dma_source_.dma_stride_y(),
            dma_source_.stride_aligned() ? 1 : 0);
    }

    const bool ok = ensure_stream_peer(
        msg, kMediaShmName, middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp);
    send_stream_start_rsp_with_type(
        msg, ok, middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp,
        kMediaShmName);
}

void CameraService::handle_media_stream_stop(const middleware::Message& msg)
{
    remove_stream_peer(msg.sender_id);
    if(!has_stream_peers()) {
        stop_streaming();
    }
    send_stream_stop_done_with_type(
        msg, middleware::MsgType::APP_Media_Camera_Stream_Stop_Rsp);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop media sender=%u",
                           static_cast<unsigned>(msg.sender_id));
}

bool CameraService::start_streaming()
{
    if(requested_video_basename_.empty()) {
        return start_live_streaming();
    }
    const std::string demo_path = resolve_demo_video_path(requested_video_basename_);
    if(!demo_path.empty()) {
        return start_demo_video_streaming(demo_path);
    }
    UPC_LOG_CAMERA_SERVICE(Warn, "requested demo video not found: %s",
                           requested_video_basename_.c_str());
    return start_live_streaming();
}

bool CameraService::start_live_streaming()
{
    stream_mode_ = StreamSourceMode::Live;
    active_video_path_.clear();
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

bool CameraService::start_demo_video_streaming(const std::string& video_path)
{
#if !defined(UPC_CAMERA_HAS_LIBAV) || !UPC_CAMERA_HAS_LIBAV
    (void)video_path;
    UPC_LOG_CAMERA_SERVICE(Error, "demo video stream requested but libav disabled");
    return false;
#else
    stream_mode_ = StreamSourceMode::DemoVideo;
    active_video_path_ = video_path;
    stop_demo_video_streaming();

    constexpr uint32_t kMaxEdge = 960;
    DemoVideoDecoder decoder;
    if(!decoder.open(video_path.c_str(), kMaxEdge)) {
        UPC_LOG_CAMERA_SERVICE(Error, "demo video open failed path=%s", video_path.c_str());
        return false;
    }
    stream_width_ = decoder.width();
    stream_height_ = decoder.height();
    stream_fps_ = decoder.fps();
    const uint32_t stride = ((stream_width_ + CAMERA_STRIDE_ALIGN - 1u) /
                             CAMERA_STRIDE_ALIGN) * CAMERA_STRIDE_ALIGN;
    const size_t bytes = static_cast<size_t>(stride) * stream_height_ * 3u / 2u;
    demo_video_bufs_.resize(CAMERA_V4L2_BUF_COUNT);
    std::vector<int> dma_fds;
    dma_fds.reserve(demo_video_bufs_.size());
    for(auto& b : demo_video_bufs_) {
        upc_ipc::DmaHeapBuffer hb{};
        if(!upc_ipc::dma_heap_alloc(bytes, hb)) {
            UPC_LOG_CAMERA_SERVICE(Error, "demo video dma alloc failed size=%zu", bytes);
            stop_demo_video_streaming();
            return false;
        }
        b.fd = hb.fd;
        b.va = hb.va;
        b.size = hb.size;
        dma_fds.push_back(hb.fd);
    }
    if(!dma_source_.prepare(stream_width_, stream_height_, stride, dma_fds)) {
        UPC_LOG_CAMERA_SERVICE(Error, "demo video dma source prepare failed");
        stop_demo_video_streaming();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        stream_peers_.clear();
    }
    UPC_LOG_CAMERA_SERVICE(Info, "demo video source ready path=%s %ux%u@%u",
                           video_path.c_str(), stream_width_, stream_height_, stream_fps_);
    return true;
#endif
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
    if(stream_mode_ == StreamSourceMode::Live) {
        capture_.close();
    } else {
        stop_demo_video_streaming();
    }
    stream_mode_ = StreamSourceMode::Live;
    active_video_path_.clear();
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
    peer->rsp_type = rsp_type;
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
    if(stream_mode_ == StreamSourceMode::DemoVideo) {
        stream_thread_demo_video_fn();
        return;
    }
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
    uint64_t last_frame_ok_ms = monotonic_ms();
    uint64_t last_watchdog_restart_ms = 0;

    if(warmup_left > 0) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "AE warmup: discarding first %u frames before SHM commit",
            warmup_left);
    }

    while(!stop_stream_.load(std::memory_order_relaxed)) {
        const auto now = clock::now();
        if(now - stats_since >= std::chrono::seconds(5)) {
            /* 流诊断受 UPC_MOD_CAMERA_PREVIEW 控制，5s 聚合输出 */
            UPC_LOG_CAMERA_PREVIEW(Debug,
                "stream 5s: committed=%u last_seq=%u last_slot=%u "
                "slot_fail=%u dequeue_fail=%u release_timeout=%u",
                frames_1s, last_seq, last_slot,
                slot_fail_1s, dequeue_fail_1s, timeout_1s);
            frames_1s = slot_fail_1s = dequeue_fail_1s = timeout_1s = 0;
            stats_since = now;
        }

        const auto tick_start = clock::now();

        auto try_watchdog_restart = [&](const char* why) -> bool {
#if CAMERA_STREAM_WATCHDOG_STALL_MS == 0
            (void)why;
            return false;
#else
            const uint64_t now_ms = monotonic_ms();
            if(now_ms - last_frame_ok_ms < CAMERA_STREAM_WATCHDOG_STALL_MS) {
                return false;
            }
            if(last_watchdog_restart_ms != 0 &&
               now_ms - last_watchdog_restart_ms <
                   CAMERA_STREAM_WATCHDOG_COOLDOWN_MS) {
                return false;
            }
            UPC_LOG_CAMERA_SERVICE(Warn,
                "stream watchdog trigger why=%s stall_ms=%llu",
                why ? why : "?",
                static_cast<unsigned long long>(now_ms - last_frame_ok_ms));
            last_watchdog_restart_ms = now_ms;
            if(!restart_capture_keep_peers()) {
                last_frame_ok_ms = monotonic_ms();
                return false;
            }
            warmup_left = stream_warmup_frames_;
            logged_first_commit = false;
            last_frame_ok_ms = monotonic_ms();
            if(warmup_left > 0) {
                UPC_LOG_CAMERA_SERVICE(Info,
                    "stream watchdog: AE warmup %u frames after restart",
                    warmup_left);
            }
            return true;
#endif
        };

        /* AE 预热：只 dequeue/queue 喂 ISP，不占 SHM slot、不 commit。 */
        if(warmup_left > 0) {
            std::optional<uint32_t> buf_idx = capture_.dequeue_frame(2000);
            if(!buf_idx) {
                ++dequeue_fail_1s;
                if(stop_stream_.load()) {
                    break;
                }
                (void)try_watchdog_restart("warmup_dequeue");
                continue;
            }
            last_frame_ok_ms = monotonic_ms();
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
            (void)try_watchdog_restart("dequeue");
            continue;
        }
        last_frame_ok_ms = monotonic_ms();

        uint32_t slot_buf_idx = *buf_idx;
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

void CameraService::stream_thread_demo_video_fn()
{
#if !defined(UPC_CAMERA_HAS_LIBAV) || !UPC_CAMERA_HAS_LIBAV
    UPC_LOG_CAMERA_SERVICE(Error, "demo video thread started without libav");
    while(!stop_stream_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return;
#else
    constexpr uint32_t kMaxEdge = 960;
    DemoVideoDecoder decoder;
    if(!decoder.open(active_video_path_.c_str(), kMaxEdge)) {
        UPC_LOG_CAMERA_SERVICE(Error, "demo video thread open failed path=%s",
                               active_video_path_.c_str());
        return;
    }
    const auto frame_period =
        std::chrono::microseconds(1000000u / (stream_fps_ > 0 ? stream_fps_ : 1u));
    uint32_t buf_cursor = 0;
    bool logged_first_commit = false;
    while(!stop_stream_.load(std::memory_order_relaxed)) {
        const auto tick_start = std::chrono::steady_clock::now();
        if(demo_video_bufs_.empty()) {
            break;
        }
        DemoVideoBuffer& b = demo_video_bufs_[buf_cursor % demo_video_bufs_.size()];
        if(b.fd < 0 || !b.va) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const uint32_t stride = dma_source_.dma_stride_y();
        uint8_t* y = static_cast<uint8_t*>(b.va);
        uint8_t* uv = y + static_cast<size_t>(stride) * stream_height_;
        if(!decoder.decode_next(y, uv, stride)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        dma_buf_sync_end_write(b.fd);
        const uint32_t slot_buf_idx = buf_cursor % demo_video_bufs_.size();
        buf_cursor++;

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
            for(const auto& peer : peers) {
                std::optional<uint32_t> slot = peer->producer.acquire_write_slot();
                if(!slot) {
                    continue;
                }
                const uint32_t seq =
                    peer->producer.commit_slot(*slot, monotonic_us(), slot_buf_idx);
                if(seq == 0) {
                    peer->producer.cancel_write_slot(*slot);
                    continue;
                }
                commits.push_back(PeerCommit{peer, *slot, seq});
            }
        }
        if(!logged_first_commit && !commits.empty()) {
            UPC_LOG_CAMERA_SERVICE(Info,
                "demo first frame committed peers=%zu seq=%u buf_idx=%u",
                commits.size(), commits[0].seq, slot_buf_idx);
            logged_first_commit = true;
        }
        if(frame_ntf_enabled_ && !commits.empty()) {
            notify_frame(commits[0].seq);
        }
        for(const auto& c : commits) {
            (void)c.peer->producer.wait_slot_empty(c.slot_idx, kPeerWaitSlotEmptyMs);
        }
        const auto elapsed = std::chrono::steady_clock::now() - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }
#endif
}

void CameraService::stop_demo_video_streaming()
{
    for(auto& b : demo_video_bufs_) {
        upc_ipc::DmaHeapBuffer hb{};
        hb.fd = b.fd;
        hb.va = b.va;
        hb.size = b.size;
        upc_ipc::dma_heap_free(hb);
        b = {};
    }
    demo_video_bufs_.clear();
}

void CameraService::send_stream_start_rsp(const middleware::Message& req, bool ok)
{
    send_stream_start_rsp_with_type(req, ok,
        middleware::MsgType::APP_Camera_Stream_Start_Rsp, kUiShmName);
}

void CameraService::publish_stream_start_to_peer(middleware::ModuleId peer_id,
                                                  middleware::MsgType rsp_type,
                                                  const char* shm_name,
                                                  bool ok)
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
        peer_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&info), sizeof(info));

    auto& port = middleware::MsgPort::instance();
    if(ok && !dma_source_.ipc_fds().empty()) {
        if(!port.publish_to_peer_with_fds(rsp, dma_source_.ipc_fds())) {
            UPC_LOG_CAMERA_SERVICE(Error,
                "stream start rsp with fds failed peer=%u type=%u",
                static_cast<unsigned>(peer_id),
                static_cast<unsigned>(rsp_type));
        }
    } else {
        port.publish_async(rsp);
    }
}

void CameraService::send_stream_start_rsp_with_type(
    const middleware::Message& req, bool ok,
    middleware::MsgType rsp_type, const char* shm_name)
{
    publish_stream_start_to_peer(req.sender_id, rsp_type, shm_name, ok);
}

bool CameraService::restart_capture_keep_peers()
{
    UPC_LOG_CAMERA_SERVICE(Warn,
        "stream watchdog: restarting ISP/V4L2 (keep peers)");

    const uint32_t old_width    = stream_width_;
    const uint32_t old_height   = stream_height_;
    const uint32_t old_stride_y = dma_source_.dma_stride_y();

    dma_source_.shutdown();
    capture_.close();

    if(stop_stream_.load(std::memory_order_relaxed)) {
        return false;
    }

    const ViCaptureConfig cfg = build_capture_config();
    if(!capture_.open(cfg)) {
        UPC_LOG_CAMERA_SERVICE(Error, "stream watchdog: ViCapture reopen failed");
        return false;
    }
    stream_width_  = capture_.width();
    stream_height_ = capture_.height();

    const std::vector<int>& capture_fds = capture_.dma_fds();
    if(capture_fds.empty()) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "stream watchdog: no dma fds after reopen");
        return false;
    }
    if(!dma_source_.prepare(stream_width_, stream_height_, capture_.stride_y(),
                            capture_fds)) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "stream watchdog: dma prepare failed");
        return false;
    }
    const uint32_t new_stride_y = dma_source_.dma_stride_y();
    const bool peer_shm_layout_changed =
        (old_width != stream_width_) ||
        (old_height != stream_height_) ||
        (old_stride_y != new_stride_y);

    struct PeerResync {
        middleware::ModuleId peer_id;
        middleware::MsgType rsp_type;
        std::string shm_name;
    };
    std::vector<PeerResync> peers;
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        for(auto& kv : stream_peers_) {
            if(!kv.second) {
                continue;
            }
            auto& peer = *kv.second;
            if(peer_shm_layout_changed) {
                peer.producer.destroy();
                const upc_ipc::IpcShmError err = peer.producer.create(
                    peer.shm_name.c_str(),
                    stream_width_,
                    stream_height_,
                    new_stride_y,
                    upc_ipc::kDmaFrameFormatNv12,
                    CAMERA_SHM_SLOT_COUNT);
                if(err != upc_ipc::IpcShmError::OK) {
                    UPC_LOG_CAMERA_SERVICE(Error,
                        "stream watchdog: peer shm recreate fail sender=%u name=%s err=%d",
                        kv.first, peer.shm_name.c_str(), static_cast<int>(err));
                    continue;
                }
            }
            peers.push_back(PeerResync{
                peer.peer_id, peer.rsp_type, peer.shm_name});
        }
    }

    for(const auto& p : peers) {
        publish_stream_start_to_peer(p.peer_id, p.rsp_type, p.shm_name.c_str(),
                                     true);
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "stream watchdog: capture restarted peers=%zu %ux%u stride_y=%u "
        "peer_shm_recreate=%d",
        peers.size(), stream_width_, stream_height_,
        new_stride_y, peer_shm_layout_changed ? 1 : 0);
    return true;
}

uint64_t CameraService::monotonic_ms()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch())
            .count());
}

void CameraService::send_stream_stop_done(const middleware::Message& req)
{
    send_stream_stop_done_with_type(
        req, middleware::MsgType::APP_Camera_Stream_Stop_Rsp);
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
