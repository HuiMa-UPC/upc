#include "live_rtsp_pusher.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#endif

namespace upc::media {

namespace {
/** 云端 ack reason：已有推流会话，拒绝并发 start。 */
constexpr const char* kReasonPushAlreadyActive = "push_already_active";
/** 云端 ack reason：编码器/ffmpeg/几何等启动失败。 */
constexpr const char* kReasonEncoderError = "encoder_error";
/** 云端 ack reason：stream_type 非 live 等非法状态。 */
constexpr const char* kReasonInvalidState = "invalid_state";
} // namespace

/**
 * 析构：停会话、关 ffmpeg/编码器，再 join worker。
 * 顺序：锁内停资源 → notify → join，避免 worker 卡在 wait 上。
 */
LiveRtspPusher::~LiveRtspPusher()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        active_ = false;
        worker_stop_ = true;
        stop_ffmpeg_locked();
        close_encoder_locked();
    }
    cv_.notify_all();
    stop_worker();
}

/**
 * 更新推流几何参数。
 * 仅接受非零值，避免开流前的 0 覆盖默认分辨率/帧率。
 */
void LiveRtspPusher::set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(width > 0) width_ = width;
    if(height > 0) height_ = height;
    if(fps > 0) fps_ = fps;
}

/** 返回当前是否处于推流会话。 */
bool LiveRtspPusher::active() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return active_;
}

/** 墙钟毫秒时间戳，填入 RtspPushRspPayload::push_started_at。 */
uint64_t LiveRtspPusher::unix_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/**
 * 将带 stride 的 NV12 平面拷贝为紧凑 NV12（Y 后接 UV，stride==width）。
 * stride==width 时整块 memcpy；否则逐行拷贝去掉行尾 padding。
 */
void LiveRtspPusher::copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                     const uint8_t* src_uv, uint32_t src_stride_y,
                                     uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || src_stride_y == 0 || width == 0 || height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
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

/**
 * 启动 live 推流会话。
 *
 * 校验顺序：stream_type → push_url → 未在推 → 几何已知 → RKMPP 可用。
 * 成功路径：开编码器 → fork ffmpeg → 按需启动 worker → active_=true。
 * ffmpeg 启动失败会回滚已打开的编码器。
 */
middleware::RtspPushRspPayload LiveRtspPusher::start(
    const middleware::RtspPushStartReqPayload& req)
{
    middleware::RtspPushRspPayload rsp{};
    std::snprintf(rsp.req_id, sizeof(rsp.req_id), "%s", req.req_id);
    std::snprintf(rsp.stream_type, sizeof(rsp.stream_type), "%s",
                  req.stream_type[0] ? req.stream_type : "live");

    const std::string stream_type =
        req.stream_type[0] ? std::string(req.stream_type) : "live";
    /* 一期仅 live；其它类型直接 Rejected，避免误开推流。 */
    if(stream_type != "live") {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Rejected);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonInvalidState);
        return rsp;
    }
    if(!req.push_url[0]) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    if(active_) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Rejected);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonPushAlreadyActive);
        return rsp;
    }
    if(width_ == 0 || height_ == 0) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp start fail: stream geometry unknown");
        return rsp;
    }

#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
    std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
    UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp RKMPP disabled in this build");
    return rsp;
#else
    /* 云端 max_bitrate_kbps→bps；0 表示使用板端默认中等码率。 */
    bitrate_bps_ = req.max_bitrate_kbps > 0
        ? req.max_bitrate_kbps * 1000u
        : CAMERA_PRINT_H264_BITRATE;

    close_encoder_locked();
    stop_ffmpeg_locked();
    has_pending_ = false;
    pending_nv12_.clear();
    dropped_ = 0;

    if(!open_encoder_locked()) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }
    if(!start_ffmpeg_locked(req.push_url)) {
        close_encoder_locked();
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }

    /* worker 进程生命周期内只建一次，stop 不销毁线程，便于反复 start/stop。 */
    if(!worker_started_) {
        worker_stop_ = false;
        worker_ = std::thread(&LiveRtspPusher::worker_fn, this);
        worker_started_ = true;
    }
    active_ = true;
    rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Accepted);
    rsp.reason[0] = '\0';
    rsp.push_started_at = unix_ms();
    UPC_LOG_MEDIA_SERVICE(Info,
        "live_rtsp started %ux%u@%u bps=%u url=%s",
        width_, height_, fps_, bitrate_bps_, req.push_url);
    return rsp;
#endif
}

/**
 * 停止 live 推流。
 * 清 pending、关 ffmpeg、关编码器；worker 线程保留，仅不再取帧。
 * 未在推流时仍返回 Accepted（幂等）。
 */
middleware::RtspPushRspPayload LiveRtspPusher::stop(
    const middleware::RtspPushStopReqPayload& req)
{
    middleware::RtspPushRspPayload rsp{};
    std::snprintf(rsp.req_id, sizeof(rsp.req_id), "%s", req.req_id);
    std::snprintf(rsp.stream_type, sizeof(rsp.stream_type), "%s",
                  req.stream_type[0] ? req.stream_type : "live");

    const std::string stream_type =
        req.stream_type[0] ? std::string(req.stream_type) : "live";
    if(stream_type != "live") {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Rejected);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonInvalidState);
        return rsp;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        active_ = false;
        has_pending_ = false;
        pending_nv12_.clear();
        stop_ffmpeg_locked();
        close_encoder_locked();
    }
    /* 唤醒可能卡在 wait 上的 worker，使其跳过已停会话。 */
    cv_.notify_all();

    rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Accepted);
    rsp.reason[0] = '\0';
    UPC_LOG_MEDIA_SERVICE(Info, "live_rtsp stopped req_id=%s", req.req_id);
    return rsp;
}

/**
 * DMA 消费线程帧回调：活跃时把最新 NV12 写入单槽 pending。
 *
 * latest-wins：若 worker 尚未取走上帧，覆盖并计入 dropped_，
 * 保证推流延迟优先于完整帧率（实时预览/直播场景）。
 */
void LiveRtspPusher::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv,
                                   uint32_t stride_y)
{
    uint32_t w = 0;
    uint32_t h = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || !src_y || !src_uv || width_ == 0 || height_ == 0) {
            return;
        }
        w = width_;
        h = height_;
        if(stride_y == 0) stride_y = w;
        if(stride_y < w) return;

        if(has_pending_) {
            ++dropped_;
        }
        pending_nv12_.resize(static_cast<size_t>(w) * h * 3u / 2u);
        copy_nv12_tight(pending_nv12_.data(), src_y, src_uv, stride_y, w, h);
        pending_w_ = w;
        pending_h_ = h;
        has_pending_ = true;
    }
    cv_.notify_one();
}

/** join worker 并清除 started 标志；仅析构路径调用。 */
void LiveRtspPusher::stop_worker()
{
    if(worker_.joinable()) {
        worker_.join();
    }
    worker_started_ = false;
}

/**
 * worker 主循环：等待 pending 或停止信号。
 * 取帧后在锁外 encode_and_write；失败则自停会话（关 ffmpeg/编码器），
 * 避免持续向已断管道写入。
 */
void LiveRtspPusher::worker_fn()
{
    for(;;) {
        std::vector<uint8_t> nv12;
        uint32_t w = 0;
        uint32_t h = 0;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] {
                return worker_stop_ || (active_ && has_pending_);
            });
            if(worker_stop_) {
                break;
            }
            if(!active_ || !has_pending_) {
                continue;
            }
            nv12.swap(pending_nv12_);
            w = pending_w_;
            h = pending_h_;
            has_pending_ = false;
        }
        if(!encode_and_write(nv12, w, h)) {
            std::lock_guard<std::mutex> lock(mtx_);
            if(active_) {
                UPC_LOG_MEDIA_SERVICE(Warn,
                    "live_rtsp encode/write fail — stopping session dropped=%u",
                    dropped_);
                active_ = false;
                stop_ffmpeg_locked();
                close_encoder_locked();
            }
        }
    }
}

/**
 * 打开 RKMPP H.264 编码器（CBR，NV12/YUV420SP）。
 * GOP≈1s（gop=fps），便于 App 中途 PLAY 快速等到 IDR。
 * 调用前必须持有 mtx_。
 */
bool LiveRtspPusher::open_encoder_locked()
{
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    return false;
#else
    if(mpp_ready_) return true;
    close_encoder_locked();

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppEncCfg cfg = nullptr;
    if(mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp mpp_create fail");
        return false;
    }
    if(mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp mpp_init fail");
        mpp_destroy(ctx);
        return false;
    }
    mpp_enc_cfg_init(&cfg);
    if(!cfg) {
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp mpp_enc_cfg_init fail");
        mpp_destroy(ctx);
        return false;
    }

    (void)mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
    const int fps = static_cast<int>(std::max(1u, fps_));
    const int bps = static_cast<int>(bitrate_bps_ > 0 ? bitrate_bps_
                                                      : CAMERA_PRINT_H264_BITRATE);
    mpp_enc_cfg_set_s32(cfg, "prep:width", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:height", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    /* ~1s IDR，便于 App 中途 PLAY */
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps);
    (void)mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

    mpp_ctx_ = ctx;
    mpp_mpi_ = mpi;
    mpp_cfg_ = cfg;
    mpp_ready_ = true;
    return true;
#endif
}

/**
 * 关闭 RKMPP：发 EOS 帧、短暂排空 packet（直播侧丢弃，不写文件），
 * 再 deinit cfg / destroy ctx。调用前必须持有 mtx_。
 */
void LiveRtspPusher::close_encoder_locked()
{
#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
    if(mpp_ready_) {
        auto* mpi = reinterpret_cast<MppApi*>(mpp_mpi_);
        auto ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
        MppFrame eos = nullptr;
        if(mpi && ctx && mpp_frame_init(&eos) == MPP_OK && eos) {
            mpp_frame_set_eos(eos, 1);
            (void)mpi->encode_put_frame(ctx, eos);
            mpp_frame_deinit(&eos);
            /* 直播停止不落盘，仅排空内部队列，最多约 50 次 get。 */
            for(int i = 0; i < 50; ++i) {
                MppPacket pkt = nullptr;
                if(mpi->encode_get_packet(ctx, &pkt) != MPP_OK || !pkt) {
                    break;
                }
                const bool eos_hit = (mpp_packet_get_eos(pkt) != 0);
                mpp_packet_deinit(&pkt);
                if(eos_hit) break;
            }
        }
    }
    if(mpp_cfg_) {
        mpp_enc_cfg_deinit(reinterpret_cast<MppEncCfg>(mpp_cfg_));
        mpp_cfg_ = nullptr;
    }
    if(mpp_ctx_) {
        mpp_destroy(reinterpret_cast<MppCtx>(mpp_ctx_));
        mpp_ctx_ = nullptr;
        mpp_mpi_ = nullptr;
    }
    mpp_ready_ = false;
#else
    mpp_ctx_ = nullptr;
    mpp_mpi_ = nullptr;
    mpp_cfg_ = nullptr;
    mpp_ready_ = false;
#endif
}

/**
 * 启动 ffmpeg 子进程做 RTSP 复用推流。
 *
 * 管道：父写 Annex-B → 子 stdin；子进程：
 *   -f h264 -i pipe:0 -c copy -f rtsp -rtsp_transport tcp <push_url>
 * stdout/stderr 重定向到 /dev/null，避免刷屏占满父进程无关 fd。
 * 调用前必须持有 mtx_。
 */
bool LiveRtspPusher::start_ffmpeg_locked(const char* push_url)
{
    if(!push_url || !push_url[0]) {
        return false;
    }
    stop_ffmpeg_locked();

    int pipefd[2] = {-1, -1};
    if(::pipe(pipefd) != 0) {
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp pipe fail err=%s",
                              std::strerror(errno));
        return false;
    }

    const pid_t pid = ::fork();
    if(pid < 0) {
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp fork fail err=%s",
                              std::strerror(errno));
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }
    if(pid == 0) {
        ::close(pipefd[1]);
        if(::dup2(pipefd[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        ::close(pipefd[0]);
        /* 丢掉 ffmpeg 噪声，避免占满父进程无关 fd */
        int null_fd = ::open("/dev/null", O_WRONLY);
        if(null_fd >= 0) {
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }
        ::execl(CAMERA_FFMPEG_BIN, CAMERA_FFMPEG_BIN,
                "-loglevel", "error",
                "-fflags", "+genpts",
                "-f", "h264",
                "-i", "pipe:0",
                "-c", "copy",
                "-f", "rtsp",
                "-rtsp_transport", "tcp",
                push_url,
                static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(pipefd[0]);
    ffmpeg_stdin_fd_ = pipefd[1];
    ffmpeg_pid_ = pid;
    UPC_LOG_MEDIA_SERVICE(Info, "live_rtsp ffmpeg pid=%d", static_cast<int>(pid));
    return true;
}

/**
 * 停止 ffmpeg：先关 stdin（促使子进程读 EOF），再 SIGTERM 等待，
 * 超时 SIGKILL。调用前必须持有 mtx_。
 */
void LiveRtspPusher::stop_ffmpeg_locked()
{
    if(ffmpeg_stdin_fd_ >= 0) {
        ::close(ffmpeg_stdin_fd_);
        ffmpeg_stdin_fd_ = -1;
    }
    if(ffmpeg_pid_ > 0) {
        (void)::kill(ffmpeg_pid_, SIGTERM);
        int status = 0;
        for(int i = 0; i < 50; ++i) {
            const pid_t w = ::waitpid(ffmpeg_pid_, &status, WNOHANG);
            if(w == ffmpeg_pid_ || (w < 0 && errno == ECHILD)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(::waitpid(ffmpeg_pid_, &status, WNOHANG) == 0) {
            (void)::kill(ffmpeg_pid_, SIGKILL);
            (void)::waitpid(ffmpeg_pid_, &status, 0);
        }
        ffmpeg_pid_ = -1;
    }
}

/**
 * 编码一帧并写入 ffmpeg stdin。
 *
 * 流程：锁内校验会话并拷贝句柄 → 锁外申请 MPP buffer、put_frame、
 * 循环 get_packet 并 write_all 到 stdin。
 * write 失败（管道断开等）返回 false，由 worker 自停会话。
 */
bool LiveRtspPusher::encode_and_write(const std::vector<uint8_t>& nv12,
                                      uint32_t w, uint32_t h)
{
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    (void)nv12;
    (void)w;
    (void)h;
    return false;
#else
    void* ctx = nullptr;
    void* mpi = nullptr;
    int out_fd = -1;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || !mpp_ready_ || ffmpeg_stdin_fd_ < 0) {
            return false;
        }
        ctx = mpp_ctx_;
        mpi = mpp_mpi_;
        out_fd = ffmpeg_stdin_fd_;
    }

    const size_t need = static_cast<size_t>(w) * h * 3u / 2u;
    if(nv12.size() < need) {
        return false;
    }

    auto mpp_ctx = reinterpret_cast<MppCtx>(ctx);
    auto* mpp_mpi = reinterpret_cast<MppApi*>(mpi);
    MppBuffer buf = nullptr;
    if(mpp_buffer_get(nullptr, &buf, need) != MPP_OK || !buf) {
        return false;
    }
    void* ptr = mpp_buffer_get_ptr(buf);
    if(!ptr) {
        mpp_buffer_put(buf);
        return false;
    }
    std::memcpy(ptr, nv12.data(), need);

    MppFrame frame = nullptr;
    if(mpp_frame_init(&frame) != MPP_OK || !frame) {
        mpp_buffer_put(buf);
        return false;
    }
    mpp_frame_set_width(frame, w);
    mpp_frame_set_height(frame, h);
    mpp_frame_set_hor_stride(frame, w);
    mpp_frame_set_ver_stride(frame, h);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buf);

    bool ok = (mpp_mpi->encode_put_frame(mpp_ctx, frame) == MPP_OK);
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buf);
    if(!ok) {
        return false;
    }

    /* 取出本帧产生的全部 packet（可能含多 NALU），写入 ffmpeg stdin。 */
    for(;;) {
        MppPacket pkt = nullptr;
        if(mpp_mpi->encode_get_packet(mpp_ctx, &pkt) != MPP_OK || !pkt) {
            break;
        }
        const void* p = mpp_packet_get_pos(pkt);
        const size_t n = static_cast<size_t>(mpp_packet_get_length(pkt));
        if(p && n > 0) {
            const uint8_t* bytes = static_cast<const uint8_t*>(p);
            size_t off = 0;
            while(off < n) {
                const ssize_t wr = ::write(out_fd, bytes + off, n - off);
                if(wr < 0) {
                    if(errno == EINTR) continue;
                    mpp_packet_deinit(&pkt);
                    return false;
                }
                off += static_cast<size_t>(wr);
            }
        }
        mpp_packet_deinit(&pkt);
    }
    return true;
#endif
}

} // namespace upc::media
