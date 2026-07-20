#include "video_player.hpp"
#include "rs_log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>

/**
 * @file video_player.cpp
 * @brief 延时摄影播放器引擎实现（A2 自研 libav* 软解）。
 *
 * 线程模型：
 *   - 解码线程：avformat/avcodec 读包解帧 → sws_scale 到 BGRA 三缓冲，按 pts 节流后
 *     发布 display_buf_；处理 seek / pause / EOF。
 *   - 主线程 lv_timer(33ms)：换 lv_image 帧指针 + 触发进度回调（可安全操作 LVGL）。
 */

namespace ui {

namespace {
using clock_t_ = std::chrono::steady_clock;

double now_sec()
{
    return std::chrono::duration<double>(clock_t_::now().time_since_epoch()).count();
}
} // namespace

VideoPlayer& VideoPlayer::instance()
{
    static VideoPlayer inst;
    return inst;
}

void VideoPlayer::reset_state()
{
    stop_.store(false);
    playing_.store(true);
    ended_.store(false);
    ended_notified_.store(false);
    position_.store(0.0);
    duration_.store(0.0);
    seek_target_.store(-1.0);
    display_buf_.store(-1);
    shown_buf_.store(-1);
    dst_w_ = 0;
    dst_h_ = 0;
}

bool VideoPlayer::alloc_buffers(int w, int h, int row_stride_bytes)
{
    const size_t bytes = static_cast<size_t>(row_stride_bytes) * static_cast<size_t>(h);
    for(int i = 0; i < kFrameSlots; ++i) {
        slot_buf_[i].assign(bytes, 0);
        img_dsc_[i]               = {};
        img_dsc_[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        img_dsc_[i].header.cf     = LV_COLOR_FORMAT_XRGB8888;
        img_dsc_[i].header.w      = static_cast<uint32_t>(w);
        img_dsc_[i].header.h      = static_cast<uint32_t>(h);
        img_dsc_[i].header.stride = static_cast<uint32_t>(row_stride_bytes);
        img_dsc_[i].data          = slot_buf_[i].data();
        img_dsc_[i].data_size     = static_cast<uint32_t>(bytes);
    }
    return true;
}

void VideoPlayer::free_buffers()
{
    for(int i = 0; i < kFrameSlots; ++i) {
        slot_buf_[i].clear();
        slot_buf_[i].shrink_to_fit();
        img_dsc_[i] = {};
    }
}

bool VideoPlayer::open(const std::string& path, lv_obj_t* image_widget)
{
    close(); // 幂等：确保上一次已停止

    if(path.empty() || !image_widget) {
        return false;
    }
    path_  = path;
    image_ = image_widget;
    reset_state();
    open_.store(true);

    preview_stop_.store(false);
    preview_req_.store(-1.0);
    pv_ready_idx_.store(-1);

    decode_thread_  = std::thread(&VideoPlayer::decode_thread_fn, this);
    preview_thread_ = std::thread(&VideoPlayer::preview_thread_fn, this);
    ui_timer_ = lv_timer_create(ui_timer_cb, kUiPeriodMs, this);
    UPC_LOG_CAMERA_PREVIEW(Info, "video_player open %s", path_.c_str());
    return true;
}

void VideoPlayer::close()
{
    if(!open_.load() && !decode_thread_.joinable()) {
        return;
    }
    stop_.store(true);
    playing_.store(true); // 唤醒可能在暂停等待的解码线程
    cv_.notify_all();
    preview_stop_.store(true);
    pv_cv_.notify_all();
    if(decode_thread_.joinable()) {
        decode_thread_.join();
    }
    if(preview_thread_.joinable()) {
        preview_thread_.join();
    }
    if(ui_timer_) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    open_.store(false);
    image_ = nullptr;
    free_buffers();
    pv_buf_[0].clear(); pv_buf_[0].shrink_to_fit();
    pv_buf_[1].clear(); pv_buf_[1].shrink_to_fit();
    pv_dsc_[0] = {}; pv_dsc_[1] = {};
    pv_ready_idx_.store(-1);
    reset_state();
}

void VideoPlayer::play()
{
    if(!open_.load()) {
        return;
    }
    if(ended_.load()) {
        seek_target_.store(0.0); // 播完后再播 → 回到开头
        ended_.store(false);
    }
    ended_notified_.store(false);
    playing_.store(true);
    cv_.notify_all();
}

void VideoPlayer::pause()
{
    playing_.store(false);
}

void VideoPlayer::toggle()
{
    if(is_playing()) {
        pause();
    } else {
        play();
    }
}

void VideoPlayer::seek(double sec)
{
    if(!open_.load()) {
        return;
    }
    const double dur = duration_.load();
    if(sec < 0.0) sec = 0.0;
    if(dur > 0.0 && sec > dur) sec = dur;
    seek_target_.store(sec);
    ended_.store(false);
    ended_notified_.store(false);
    cv_.notify_all(); // 若暂停中，唤醒去解码目标帧
}

void VideoPlayer::request_preview(double sec)
{
    if(!open_.load()) {
        return;
    }
    const double dur = duration_.load();
    if(sec < 0.0) sec = 0.0;
    if(dur > 0.0 && sec > dur) sec = dur;
    preview_req_.store(sec);
    pv_cv_.notify_all();
}

/* ---------------- 解码线程 ---------------- */

void VideoPlayer::decode_thread_fn()
{
    AVFormatContext* fmt = nullptr;
    if(avformat_open_input(&fmt, path_.c_str(), nullptr, nullptr) != 0) {
        UPC_LOG_CAMERA_PREVIEW(Error, "video_player open_input fail %s", path_.c_str());
        return;
    }
    if(avformat_find_stream_info(fmt, nullptr) < 0) {
        UPC_LOG_CAMERA_PREVIEW(Error, "video_player find_stream_info fail");
        avformat_close_input(&fmt);
        return;
    }

    const int vid = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if(vid < 0) {
        UPC_LOG_CAMERA_PREVIEW(Error, "video_player no video stream");
        avformat_close_input(&fmt);
        return;
    }
    AVStream* st = fmt->streams[vid];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if(!dec) {
        UPC_LOG_CAMERA_PREVIEW(Error, "video_player no decoder");
        avformat_close_input(&fmt);
        return;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if(!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 ||
       avcodec_open2(ctx, dec, nullptr) < 0) {
        UPC_LOG_CAMERA_PREVIEW(Error, "video_player codec open fail");
        if(ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }

    const int src_w = ctx->width;
    const int src_h = ctx->height;
    /* 目标尺寸：长边限制到 kMaxEdge，等比、偶数 */
    int dst_w = src_w, dst_h = src_h;
    const int longest = src_w > src_h ? src_w : src_h;
    if(longest > kMaxEdge && longest > 0) {
        dst_w = static_cast<int>(static_cast<int64_t>(src_w) * kMaxEdge / longest);
        dst_h = static_cast<int>(static_cast<int64_t>(src_h) * kMaxEdge / longest);
    }
    dst_w &= ~1; dst_h &= ~1;
    if(dst_w < 2) dst_w = 2;
    if(dst_h < 2) dst_h = 2;

    SwsContext* sws = sws_getContext(src_w, src_h, ctx->pix_fmt,
                                     dst_w, dst_h, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if(!sws) {
        UPC_LOG_CAMERA_PREVIEW(Error, "video_player sws_getContext fail");
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }

    const int row_stride = dst_w * 4;
    dst_w_ = dst_w;
    dst_h_ = dst_h;
    alloc_buffers(dst_w, dst_h, row_stride);

    /* 时长（秒） */
    double dur = 0.0;
    if(fmt->duration > 0) {
        dur = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    } else if(st->duration > 0) {
        dur = static_cast<double>(st->duration) * av_q2d(st->time_base);
    }
    duration_.store(dur);

    const double tb = av_q2d(st->time_base);
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    double clock_base = now_sec();  /* 播放时钟基准：playtime = now - clock_base */
    int    write_idx  = 0;

    auto publish_frame = [&](double pts_sec) {
        uint8_t* dst_data[4] = { slot_buf_[write_idx].data(), nullptr, nullptr, nullptr };
        int      dst_ls[4]   = { row_stride, 0, 0, 0 };
        sws_scale(sws, frame->data, frame->linesize, 0, src_h, dst_data, dst_ls);
        display_buf_.store(write_idx, std::memory_order_release);
        position_.store(pts_sec);
        write_idx = (write_idx + 1) % kFrameSlots;
    };

    while(!stop_.load()) {
        /* ---- seek ---- */
        double st_target = seek_target_.exchange(-1.0);
        if(st_target >= 0.0) {
            const int64_t ts =
                static_cast<int64_t>(st_target / (tb > 0 ? tb : 1.0));
            av_seek_frame(fmt, vid, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(ctx);
            position_.store(st_target);
            clock_base = now_sec() - st_target;
            ended_.store(false);
            /* 解码并发布一帧到目标位置（暂停时也刷新画面） */
            bool shown = false;
            while(!shown && !stop_.load() && av_read_frame(fmt, pkt) >= 0) {
                if(pkt->stream_index == vid &&
                   avcodec_send_packet(ctx, pkt) == 0) {
                    while(avcodec_receive_frame(ctx, frame) == 0) {
                        const int64_t ptsv = frame->best_effort_timestamp;
                        const double ps = (ptsv != AV_NOPTS_VALUE) ? ptsv * tb : st_target;
                        if(ps + 1e-3 >= st_target || shown) {
                            publish_frame(ps);
                            clock_base = now_sec() - ps;
                            shown = true;
                            av_frame_unref(frame);
                            break;
                        }
                        av_frame_unref(frame);
                    }
                }
                av_packet_unref(pkt);
            }
            continue;
        }

        /* ---- 暂停：等待 play/seek/stop ---- */
        if(!playing_.load()) {
            std::unique_lock<std::mutex> lk(cv_mtx_);
            cv_.wait(lk, [this] {
                return stop_.load() || playing_.load() || seek_target_.load() >= 0.0;
            });
            continue;
        }

        /* ---- 正常读帧 ---- */
        const int rret = av_read_frame(fmt, pkt);
        if(rret < 0) {
            /* EOF：flush 解码器，停在结尾 */
            avcodec_send_packet(ctx, nullptr);
            while(avcodec_receive_frame(ctx, frame) == 0) {
                const int64_t ptsv = frame->best_effort_timestamp;
                const double ps = (ptsv != AV_NOPTS_VALUE) ? ptsv * tb
                                                           : duration_.load();
                publish_frame(ps);
                av_frame_unref(frame);
            }
            avcodec_flush_buffers(ctx);
            playing_.store(false);
            ended_.store(true);
            if(duration_.load() > 0.0) {
                position_.store(duration_.load());
            }
            continue;
        }

        if(pkt->stream_index != vid) {
            av_packet_unref(pkt);
            continue;
        }
        if(avcodec_send_packet(ctx, pkt) != 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while(avcodec_receive_frame(ctx, frame) == 0) {
            const int64_t ptsv = frame->best_effort_timestamp;
            const double ps = (ptsv != AV_NOPTS_VALUE) ? ptsv * tb : position_.load();

            /* 按 pts 节流；被 stop/seek/pause 打断则提前退出 */
            bool abort = false;
            while(playing_.load() && !stop_.load() && seek_target_.load() < 0.0) {
                const double playtime = now_sec() - clock_base;
                const double delay = ps - playtime;
                if(delay <= 0.0) {
                    break;
                }
                const double s = delay < 0.02 ? delay : 0.02;
                std::this_thread::sleep_for(std::chrono::duration<double>(s));
            }
            if(stop_.load() || seek_target_.load() >= 0.0) {
                abort = true;
            }
            publish_frame(ps);
            av_frame_unref(frame);
            if(abort) {
                break;
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

/* ---------------- 拖动预览解码线程（独立 AVFormatContext，不扰动播放） ---------------- */

void VideoPlayer::preview_thread_fn()
{
    AVFormatContext* fmt = nullptr;
    if(avformat_open_input(&fmt, path_.c_str(), nullptr, nullptr) != 0 ||
       avformat_find_stream_info(fmt, nullptr) < 0) {
        if(fmt) avformat_close_input(&fmt);
        return; // 预览不可用（不影响播放）
    }
    const int vid = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if(vid < 0) {
        avformat_close_input(&fmt);
        return;
    }
    AVStream* st = fmt->streams[vid];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    if(!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 ||
       avcodec_open2(ctx, dec, nullptr) < 0) {
        if(ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }

    const int src_w = ctx->width, src_h = ctx->height;
    int pw = src_w, ph = src_h;
    const int longest = src_w > src_h ? src_w : src_h;
    if(longest > kPreviewEdge && longest > 0) {
        pw = static_cast<int>(static_cast<int64_t>(src_w) * kPreviewEdge / longest);
        ph = static_cast<int>(static_cast<int64_t>(src_h) * kPreviewEdge / longest);
    }
    pw &= ~1; ph &= ~1;
    if(pw < 2) pw = 2;
    if(ph < 2) ph = 2;

    SwsContext* sws = sws_getContext(src_w, src_h, ctx->pix_fmt,
                                     pw, ph, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if(!sws) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }

    const int row_stride = pw * 4;
    const size_t bytes = static_cast<size_t>(row_stride) * ph;
    for(int i = 0; i < 2; ++i) {
        pv_buf_[i].assign(bytes, 0);
        pv_dsc_[i]               = {};
        pv_dsc_[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        pv_dsc_[i].header.cf     = LV_COLOR_FORMAT_XRGB8888;
        pv_dsc_[i].header.w      = static_cast<uint32_t>(pw);
        pv_dsc_[i].header.h      = static_cast<uint32_t>(ph);
        pv_dsc_[i].header.stride = static_cast<uint32_t>(row_stride);
        pv_dsc_[i].data          = pv_buf_[i].data();
        pv_dsc_[i].data_size     = static_cast<uint32_t>(bytes);
    }

    const double tb = av_q2d(st->time_base);
    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    int widx = 0;

    while(!preview_stop_.load()) {
        {
            std::unique_lock<std::mutex> lk(pv_mtx_);
            pv_cv_.wait(lk, [this] {
                return preview_stop_.load() || preview_req_.load() >= 0.0;
            });
        }
        if(preview_stop_.load()) {
            break;
        }
        const double sec = preview_req_.exchange(-1.0);
        if(sec < 0.0) {
            continue;
        }

        const int64_t ts = static_cast<int64_t>(sec / (tb > 0 ? tb : 1.0));
        av_seek_frame(fmt, vid, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);

        bool got = false;
        int  guard = 0;
        /* 只取关键帧附近首帧，速度优先；更新请求到来则提前中止 */
        while(!got && !preview_stop_.load() && preview_req_.load() < 0.0 &&
              guard++ < 400 && av_read_frame(fmt, pkt) >= 0) {
            if(pkt->stream_index == vid && avcodec_send_packet(ctx, pkt) == 0) {
                if(avcodec_receive_frame(ctx, frame) == 0) {
                    uint8_t* dst[4] = { pv_buf_[widx].data(), nullptr, nullptr, nullptr };
                    int      dls[4] = { row_stride, 0, 0, 0 };
                    sws_scale(sws, frame->data, frame->linesize, 0, src_h, dst, dls);
                    pv_ready_sec_.store(sec);
                    pv_ready_idx_.store(widx, std::memory_order_release);
                    widx ^= 1;
                    got = true;
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(pkt);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

/* ---------------- 主线程 UI 定时器 ---------------- */

void VideoPlayer::ui_timer_cb(lv_timer_t* timer)
{
    auto* self = static_cast<VideoPlayer*>(lv_timer_get_user_data(timer));
    if(!self || !self->open_.load()) {
        return;
    }

    const int idx = self->display_buf_.load(std::memory_order_acquire);
    if(idx >= 0 && idx < kFrameSlots && self->image_ &&
       idx != self->shown_buf_.load(std::memory_order_relaxed)) {
        self->shown_buf_.store(idx, std::memory_order_relaxed);
        lv_image_set_src(self->image_, &self->img_dsc_[idx]);
        lv_obj_invalidate(self->image_);
    }

    if(self->progress_cb_) {
        self->progress_cb_(self->position_.load(), self->duration_.load());
    }

    /* 拖动预览：有新解码好的小图则回调（主线程，安全操作 LVGL） */
    const int pidx = self->pv_ready_idx_.exchange(-1, std::memory_order_acquire);
    if(pidx >= 0 && pidx < 2 && self->preview_cb_) {
        self->preview_cb_(&self->pv_dsc_[pidx], self->pv_ready_sec_.load());
    }
    if(self->ended_.load() && !self->ended_notified_.load()) {
        self->ended_notified_.store(true);
        if(self->ended_cb_) {
            self->ended_cb_();
        }
    }
}

} // namespace ui
