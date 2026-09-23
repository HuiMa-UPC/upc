#include "h264_stream_encoder.hpp"

#include "nv12_copy.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#endif

namespace upc::media {

namespace {
/** H.264 NAL 类型：IDR 图像片。 */
constexpr uint8_t kNalTypeIdr = 5;
/** H.264 NAL 类型：序列参数集。 */
constexpr uint8_t kNalTypeSps = 7;
/** H.264 NAL 类型：图像参数集。 */
constexpr uint8_t kNalTypePps = 8;

/**
 * 墙钟微秒。用作帧的 presentation time 基准：live555 的 RTCP SR 要拿它
 * 换算 NTP 时间戳，故必须是墙钟而非单调钟。
 */
int64_t now_us()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
} // namespace

/**
 * 析构：停 worker、摘掉全部 sink、关编码器。
 * 顺序：锁内置停止位并清资源 → notify → join，避免 worker 卡在 wait 上。
 */
H264StreamEncoder::~H264StreamEncoder()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        worker_stop_ = true;
        for(auto& s : sinks_) {
            s->alive.store(false);
        }
        sinks_.clear();
        close_encoder_locked();
    }
    cv_.notify_all();
    stop_worker();
}

/**
 * 更新编码几何。
 * 仅接受非零值，避免开流前的 0 覆盖默认分辨率/帧率。
 * 编码器已打开时不重开：相机看门狗重启不会改变几何，重开反而会打断在途会话。
 */
void H264StreamEncoder::set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(width > 0) width_ = width;
    if(height > 0) height_ = height;
    if(fps > 0) fps_ = fps;
}

/** 是否已有 sink（即编码会话是否活跃）。 */
bool H264StreamEncoder::active() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return !sinks_.empty();
}

/** 取最近一次 IDR 的参数集；首个 IDR 之前返回 false。 */
bool H264StreamEncoder::copy_sps_pps(std::vector<uint8_t>& sps,
                                     std::vector<uint8_t>& pps) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(sps_.empty() || pps_.empty()) {
        return false;
    }
    sps = sps_;
    pps = pps_;
    return true;
}

/**
 * 遍历 Annex-B 码流中的 NAL。
 * 起始码兼容 3 字节（00 00 01）与 4 字节（00 00 00 01）；
 * 回调收到的是不含起始码的裸 NAL。
 */
void H264StreamEncoder::for_each_nal(const uint8_t* annexb, size_t len,
                                     const std::function<void(const uint8_t*, size_t)>& fn)
{
    if(!annexb || len < 4 || !fn) {
        return;
    }
    size_t pos = 0;
    size_t nal_begin = 0;
    bool in_nal = false;

    while(pos + 2 < len) {
        if(annexb[pos] == 0x00 && annexb[pos + 1] == 0x00 && annexb[pos + 2] == 0x01) {
            if(in_nal) {
                /* 前一个 NAL 到本起始码为止；起始码可能是 4 字节，回退一个 0。 */
                size_t nal_end = pos;
                if(nal_end > nal_begin && annexb[nal_end - 1] == 0x00) {
                    --nal_end;
                }
                if(nal_end > nal_begin) {
                    fn(annexb + nal_begin, nal_end - nal_begin);
                }
            }
            pos += 3;
            nal_begin = pos;
            in_nal = true;
            continue;
        }
        ++pos;
    }
    if(in_nal && len > nal_begin) {
        fn(annexb + nal_begin, len - nal_begin);
    }
}

/**
 * 扫描一个编码包：缓存 SPS/PPS 供 SDP 使用，并判定是否含 IDR。
 * 调用前必须持有 mtx_（参数集缓存受其保护）。
 */
bool H264StreamEncoder::scan_packet(const uint8_t* annexb, size_t len)
{
    bool keyframe = false;
    for_each_nal(annexb, len, [&](const uint8_t* nal, size_t nal_len) {
        if(nal_len == 0) {
            return;
        }
        const uint8_t type = static_cast<uint8_t>(nal[0] & 0x1f);
        switch(type) {
        case kNalTypeSps:
            sps_.assign(nal, nal + nal_len);
            break;
        case kNalTypePps:
            pps_.assign(nal, nal + nal_len);
            break;
        case kNalTypeIdr:
            keyframe = true;
            break;
        default:
            break;
        }
    });
    return keyframe;
}

/**
 * 注册一路 sink。
 *
 * 首个 sink 打开 MPP，后续 sink 沿用同一路码流。
 * 码率固定取 CAMERA_PRINT_H264_BITRATE，不接受按 sink 指定：改码率需重开
 * MPP，会打断在途的另一路。
 */
int H264StreamEncoder::add_sink(PacketCb on_packet, ErrorCb on_error,
                                const SinkOptions& opt)
{
    if(!on_packet) {
        return -1;
    }

    std::unique_lock<std::mutex> lock(mtx_);
    if(width_ == 0 || height_ == 0) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "h264_enc add_sink fail: stream geometry unknown");
        return -1;
    }

    const bool first = sinks_.empty();
    if(first) {
        sps_.clear();
        pps_.clear();
        has_pending_ = false;
        pending_nv12_.clear();
        dropped_ = 0;
        if(!open_encoder_locked()) {
            return -1;
        }
        if(!worker_started_) {
            worker_stop_ = false;
            worker_ = std::thread(&H264StreamEncoder::worker_fn, this);
            worker_started_ = true;
        }
    }

    auto entry = std::make_shared<SinkEntry>();
    entry->id = next_sink_id_++;
    entry->on_packet = std::move(on_packet);
    entry->on_error = std::move(on_error);
    sinks_.push_back(entry);

    const int handle = entry->id;
    const size_t sink_count = sinks_.size();
    const uint32_t log_w = width_;
    const uint32_t log_h = height_;
    const uint32_t log_fps = fps_;
    const uint32_t log_bps = CAMERA_PRINT_H264_BITRATE;
    lock.unlock();

    if(opt.request_idr_on_join) {
        request_idr();
    }
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "h264_enc sink+ id=%d count=%zu %ux%u@%u bps=%u",
                          handle, sink_count, log_w, log_h, log_fps, log_bps);
    return handle;
}

/** 从 sinks_ 摘除并置 alive=false；调用前必须持有 mtx_。 */
std::shared_ptr<H264StreamEncoder::SinkEntry>
H264StreamEncoder::detach_sink_locked(int handle)
{
    auto it = std::find_if(sinks_.begin(), sinks_.end(),
                           [handle](const std::shared_ptr<SinkEntry>& s) {
                               return s->id == handle;
                           });
    if(it == sinks_.end()) {
        return nullptr;
    }
    auto entry = *it;
    entry->alive.store(false);
    sinks_.erase(it);
    return entry;
}

/** sink 清空后的收尾：关编码器、丢弃 pending；调用前必须持有 mtx_。 */
void H264StreamEncoder::shutdown_if_no_sink_locked()
{
    if(!sinks_.empty()) {
        return;
    }
    has_pending_ = false;
    pending_nv12_.clear();
    close_encoder_locked();
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "h264_enc idle — encoder closed dropped=%u", dropped_);
}

/**
 * 摘除一路 sink。
 * 等待本轮分发结束后再返回，保证调用方随后释放的资源（如 ffmpeg stdin）
 * 不会被 worker 回调碰到。禁止在 PacketCb 内调用。
 */
void H264StreamEncoder::remove_sink(int handle)
{
    std::unique_lock<std::mutex> lock(mtx_);
    auto entry = detach_sink_locked(handle);
    if(!entry) {
        return;
    }
    dispatch_cv_.wait(lock, [this] { return !dispatching_; });
    shutdown_if_no_sink_locked();
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "h264_enc sink- id=%d count=%zu",
                          handle, sinks_.size());
}

/**
 * DMA 消费线程帧回调：有 sink 时把最新 NV12 写入单槽 pending。
 *
 * latest-wins：若 worker 尚未取走上帧，覆盖并计入 dropped_，
 * 保证直播延迟优先于完整帧率。
 */
void H264StreamEncoder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv,
                                      uint32_t stride_y)
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(sinks_.empty() || !mpp_ready_ || !src_y || !src_uv ||
           width_ == 0 || height_ == 0) {
            return;
        }
        const uint32_t w = width_;
        const uint32_t h = height_;
        if(stride_y == 0) stride_y = w;
        if(stride_y < w) return;

        if(has_pending_) {
            ++dropped_;
        }
        pending_nv12_.resize(static_cast<size_t>(w) * h * 3u / 2u);
        upc::copy_nv12_tight(pending_nv12_.data(), src_y, src_uv, stride_y, w, h);
        pending_w_ = w;
        pending_h_ = h;
        /* 采帧时刻打戳，而非编码完成时刻：后者会把编码耗时的抖动直接
         * 灌进 presentation time，客户端为抹平抖动会加大缓冲、拉高延迟。 */
        pending_pts_us_ = now_us();
        has_pending_ = true;
        ++stat_in_frames_;
    }
    cv_.notify_one();
}

/**
 * 每 5 秒打一行运行期心跳。
 *
 * 这几个计数是排查"画面卡住"时的第一现场：in 不涨说明相机没送帧，
 * in 涨而 enc 不涨说明卡在 MPP 或某个 sink 的投递回调里，两者都涨
 * 则说明编码链路正常，问题在下游传输。调用前必须持有 mtx_。
 *
 * 由 UPC_MEDIA_LOG_STREAM_STAT 控制；关闭时本函数为空，计数照常累加。
 */
void H264StreamEncoder::maybe_log_stats_locked()
{
#if UPC_MEDIA_LOG_STREAM_STAT
    const int64_t now = now_us();
    if(stat_last_log_us_ == 0) {
        stat_last_log_us_ = now;
        return;
    }
    const int64_t elapsed = now - stat_last_log_us_;
    if(elapsed < kStatsPeriodUs) {
        return;
    }

    const uint64_t d_in = stat_in_frames_ - stat_last_in_;
    const uint64_t d_enc = stat_enc_frames_ - stat_last_enc_;
    const uint64_t d_bytes = stat_enc_bytes_ - stat_last_bytes_;
    const double secs = static_cast<double>(elapsed) / 1000000.0;
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info,
        "h264_enc stat sinks=%zu in=%llu(%.1f/s) enc=%llu(%.1f/s) "
        "kbps=%.0f overwritten=%u",
        sinks_.size(),
        static_cast<unsigned long long>(stat_in_frames_),
        static_cast<double>(d_in) / secs,
        static_cast<unsigned long long>(stat_enc_frames_),
        static_cast<double>(d_enc) / secs,
        static_cast<double>(d_bytes) * 8.0 / secs / 1000.0,
        dropped_);

    stat_last_log_us_ = now;
    stat_last_in_ = stat_in_frames_;
    stat_last_enc_ = stat_enc_frames_;
    stat_last_bytes_ = stat_enc_bytes_;
#endif /* UPC_MEDIA_LOG_STREAM_STAT */
}

/** join worker 并清除 started 标志；仅析构路径调用。 */
void H264StreamEncoder::stop_worker()
{
    if(worker_.joinable()) {
        worker_.join();
    }
    worker_started_ = false;
}

/**
 * worker 主循环：等待 pending 或停止信号。
 *
 * 取帧后在锁外编码并分发（MPP 与 sink 回调都可能阻塞）；
 * 分发期间置 dispatching_，remove_sink 会等待其落下再返回。
 * 投递失败的 sink 就地摘除并回调其 ErrorCb；MPP 自身失败则摘除全部 sink。
 */
void H264StreamEncoder::worker_fn()
{
    for(;;) {
        std::vector<uint8_t> nv12;
        uint32_t w = 0;
        uint32_t h = 0;
        int64_t pts_us = 0;
        std::vector<std::shared_ptr<SinkEntry>> sinks;
        void* ctx = nullptr;
        void* mpi = nullptr;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] {
                return worker_stop_ || (!sinks_.empty() && has_pending_);
            });
            if(worker_stop_) {
                break;
            }
            if(sinks_.empty() || !has_pending_ || !mpp_ready_) {
                has_pending_ = false;
                continue;
            }
            nv12.swap(pending_nv12_);
            w = pending_w_;
            h = pending_h_;
            pts_us = pending_pts_us_;
            has_pending_ = false;
            sinks = sinks_;
            ctx = mpp_ctx_;
            mpi = mpp_mpi_;
            dispatching_ = true;
        }

        std::vector<std::shared_ptr<SinkEntry>> failed;
        size_t enc_bytes = 0;
        const bool enc_ok = encode_and_dispatch(ctx, mpi, nv12, w, h, pts_us,
                                                sinks, failed, enc_bytes);

        {
            std::lock_guard<std::mutex> lock(mtx_);
            dispatching_ = false;
            if(enc_ok) {
                ++stat_enc_frames_;
                stat_enc_bytes_ += enc_bytes;
            }
            maybe_log_stats_locked();
            if(!enc_ok) {
                /* 编码器级故障：所有 sink 一起下线，避免持续空转。 */
                failed = sinks_;
                UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn,
                    "h264_enc encode fail — dropping all sinks dropped=%u", dropped_);
            }
            for(const auto& s : failed) {
                (void)detach_sink_locked(s->id);
            }
            shutdown_if_no_sink_locked();
        }
        dispatch_cv_.notify_all();

        /* ErrorCb 在锁外、dispatching_ 落下后调用：owner 可在其中安全清理资源。 */
        for(const auto& s : failed) {
            if(s->on_error) {
                s->on_error();
            }
        }
    }
}

/**
 * 打开 RKMPP H.264 编码器（CBR，NV12/YUV420SP）。
 * GOP≈1s（gop=fps）且每个 IDR 都带 SPS/PPS，保证客户端中途接入可解码。
 * 调用前必须持有 mtx_。
 */
bool H264StreamEncoder::open_encoder_locked()
{
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "h264_enc RKMPP disabled in this build");
    return false;
#else
    if(mpp_ready_) return true;
    close_encoder_locked();

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppEncCfg cfg = nullptr;
    if(mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "h264_enc mpp_create fail");
        return false;
    }
    if(mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "h264_enc mpp_init fail");
        mpp_destroy(ctx);
        return false;
    }
    mpp_enc_cfg_init(&cfg);
    if(!cfg) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "h264_enc mpp_enc_cfg_init fail");
        mpp_destroy(ctx);
        return false;
    }

    (void)mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
    const int fps = static_cast<int>(std::max(1u, fps_));
    const int bps = static_cast<int>(CAMERA_PRINT_H264_BITRATE);
    mpp_enc_cfg_set_s32(cfg, "prep:width", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:height", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    /* ~0.5s IDR（每秒两个）：客户端中途 PLAY 起图更快，丢包后恢复也更快，
     * 代价是同码率下 I 帧占比翻倍、P 帧预算被摊薄。 */
    mpp_enc_cfg_set_s32(cfg, "rc:gop", std::max(1, fps / 2));
    (void)mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

    /* 每个 IDR 都重发 SPS/PPS：live555 客户端中途接入必须拿到参数集。 */
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    (void)mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);

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
void H264StreamEncoder::close_encoder_locked()
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

/** 请求下一帧编为 IDR；编码器未开时静默返回。 */
void H264StreamEncoder::request_idr()
{
#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
    void* ctx = nullptr;
    void* mpi = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!mpp_ready_) {
            return;
        }
        ctx = mpp_ctx_;
        mpi = mpp_mpi_;
    }
    auto* mpp_mpi = reinterpret_cast<MppApi*>(mpi);
    (void)mpp_mpi->control(reinterpret_cast<MppCtx>(ctx), MPP_ENC_SET_IDR_FRAME, nullptr);
#endif
}

/**
 * 编码一帧并把产出的 packet 分发给各 sink。
 *
 * 全程在锁外执行（只有参数集缓存会短暂加锁）：MPP put/get 与 sink 回调都可能阻塞。
 * sink 回调返回 false 或已被摘除（alive=false）时跳过并计入 failed。
 */
bool H264StreamEncoder::encode_and_dispatch(
    void* ctx, void* mpi,
    const std::vector<uint8_t>& nv12, uint32_t w, uint32_t h, int64_t pts_us,
    const std::vector<std::shared_ptr<SinkEntry>>& sinks,
    std::vector<std::shared_ptr<SinkEntry>>& failed,
    size_t& out_bytes)
{
    out_bytes = 0;
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    (void)ctx;
    (void)mpi;
    (void)nv12;
    (void)w;
    (void)h;
    (void)pts_us;
    (void)sinks;
    (void)failed;
    return false;
#else
    if(!ctx || !mpi) {
        return false;
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

    /* 取出本帧产生的全部 packet（可能含多 NALU），逐个分发给各 sink。 */
    for(;;) {
        MppPacket pkt = nullptr;
        if(mpp_mpi->encode_get_packet(mpp_ctx, &pkt) != MPP_OK || !pkt) {
            break;
        }
        const void* p = mpp_packet_get_pos(pkt);
        const size_t n = static_cast<size_t>(mpp_packet_get_length(pkt));
        if(p && n > 0) {
            const auto* bytes = static_cast<const uint8_t*>(p);
            bool keyframe = false;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                keyframe = scan_packet(bytes, n);
            }
            out_bytes += n;
            /* 本帧所有 packet 共用同一个采帧时刻，见 PacketCb 注释。 */
            for(const auto& s : sinks) {
                if(!s->alive.load()) {
                    continue;
                }
                if(!s->on_packet(bytes, n, keyframe, pts_us)) {
                    s->alive.store(false);
                    failed.push_back(s);
                }
            }
        }
        mpp_packet_deinit(&pkt);
    }
    return true;
#endif
}

} // namespace upc::media
