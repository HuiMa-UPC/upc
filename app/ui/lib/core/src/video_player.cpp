#include "video_player.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(UPC_UI_HAS_LIBAV) && UPC_UI_HAS_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace ui {

#if defined(UPC_UI_HAS_LIBAV) && UPC_UI_HAS_LIBAV

namespace {

int64_t clamp_non_negative(int64_t v)
{
    return v < 0 ? 0 : v;
}

int64_t pts_to_ms(int64_t pts, AVRational tb)
{
    if(pts == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(pts, tb, AVRational{1, 1000});
}

int64_t ms_to_pts(int64_t ms, AVRational tb)
{
    return av_rescale_q(clamp_non_negative(ms), AVRational{1, 1000}, tb);
}

} // namespace

struct VideoPlayer::Impl {
    enum class CommandType : uint8_t {
        Seek,
        Preview,
    };

    struct Command {
        CommandType type = CommandType::Seek;
        int64_t     ms = 0;
    };

    std::mutex mtx;
    std::mutex decoder_mtx;
    std::condition_variable cv;
    std::thread decode_thread;
    std::atomic<bool> stop{false};
    bool opened = false;
    bool playing = false;
    std::atomic<bool> eof{false};
    std::atomic<bool> interrupt_requested{false};

    lv_obj_t* image_target = nullptr;

    std::deque<Command> commands;

    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    int stream_index = -1;
    AVRational time_base{1, 1000};
    int64_t duration_ms = 0;
    int64_t position_ms = 0;

    SwsContext* sws = nullptr;
    int frame_w = 0;
    int frame_h = 0;

    std::vector<uint8_t> main_rgb565;
    lv_image_dsc_t main_dsc{};
    std::vector<uint8_t> preview_rgb565;
    lv_image_dsc_t preview_dsc{};

    ProgressCb progress_cb;
    EndedCb ended_cb;
    PreviewCb preview_cb;

    std::mutex ui_payload_mtx;
    int64_t pending_progress_pos_ms = 0;
    int64_t pending_progress_dur_ms = 0;
    bool pending_progress = false;
    int64_t pending_preview_ms = 0;
    bool pending_preview = false;
    bool pending_ended = false;

    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    static void async_apply_main(void* user)
    {
        auto* self = static_cast<Impl*>(user);
        if(!self) {
            return;
        }
        std::lock_guard<std::mutex> lk(self->ui_payload_mtx);
        if(self->image_target && self->main_dsc.data) {
            lv_image_set_src(self->image_target, &self->main_dsc);
            lv_obj_invalidate(self->image_target);
        }
        if(self->pending_progress && self->progress_cb) {
            self->progress_cb(self->pending_progress_pos_ms, self->pending_progress_dur_ms);
        }
        if(self->pending_ended && self->ended_cb) {
            self->ended_cb();
        }
        self->pending_progress = false;
        self->pending_ended = false;
    }

    static void async_apply_preview(void* user)
    {
        auto* self = static_cast<Impl*>(user);
        if(!self) {
            return;
        }
        std::lock_guard<std::mutex> lk(self->ui_payload_mtx);
        if(self->pending_preview && self->preview_cb && self->preview_dsc.data) {
            self->preview_cb(&self->preview_dsc, self->pending_preview_ms);
        }
        self->pending_preview = false;
    }


    static int avio_interrupt_cb(void* opaque)
    {
        auto* self = static_cast<Impl*>(opaque);
        if(!self) {
            return 0;
        }
        return self->interrupt_requested.load(std::memory_order_relaxed) ? 1 : 0;
    }

    void notify_frame_locked(int64_t position, int64_t duration)
    {
        std::lock_guard<std::mutex> lk(ui_payload_mtx);
        pending_progress = true;
        pending_progress_pos_ms = position;
        pending_progress_dur_ms = duration;
        lv_async_call(async_apply_main, this);
    }

    void notify_preview(int64_t preview_ms)
    {
        std::lock_guard<std::mutex> lk(ui_payload_mtx);
        pending_preview = true;
        pending_preview_ms = preview_ms;
        lv_async_call(async_apply_preview, this);
    }

    void notify_ended()
    {
        std::lock_guard<std::mutex> lk(ui_payload_mtx);
        pending_ended = true;
        pending_progress = true;
        pending_progress_pos_ms = duration_ms;
        pending_progress_dur_ms = duration_ms;
        lv_async_call(async_apply_main, this);
    }

    void clear_decoder()
    {
        if(sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if(frame) {
            av_frame_free(&frame);
        }
        if(packet) {
            av_packet_free(&packet);
        }
        if(codec) {
            avcodec_free_context(&codec);
        }
        if(fmt) {
            avformat_close_input(&fmt);
        }
        stream = nullptr;
        stream_index = -1;
        time_base = AVRational{1, 1000};
        frame_w = 0;
        frame_h = 0;
        main_rgb565.clear();
        preview_rgb565.clear();
        std::memset(&main_dsc, 0, sizeof(main_dsc));
        std::memset(&preview_dsc, 0, sizeof(preview_dsc));
    }

    bool ensure_buffers()
    {
        if(frame_w <= 0 || frame_h <= 0) {
            return false;
        }
        const size_t bytes = static_cast<size_t>(frame_w) * frame_h * 2u;
        if(main_rgb565.size() != bytes) {
            main_rgb565.assign(bytes, 0);
        }
        if(preview_rgb565.size() != bytes) {
            preview_rgb565.assign(bytes, 0);
        }
        if(!main_dsc.data) {
            main_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            main_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            main_dsc.header.w = frame_w;
            main_dsc.header.h = frame_h;
            main_dsc.header.stride = static_cast<uint32_t>(frame_w * 2);
            main_dsc.data = main_rgb565.data();
            main_dsc.data_size = main_rgb565.size();
        }
        if(!preview_dsc.data) {
            preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            preview_dsc.header.w = frame_w;
            preview_dsc.header.h = frame_h;
            preview_dsc.header.stride = static_cast<uint32_t>(frame_w * 2);
            preview_dsc.data = preview_rgb565.data();
            preview_dsc.data_size = preview_rgb565.size();
        }
        return true;
    }

    bool open_decoder(const char* path)
    {
        if(!path || !path[0]) {
            return false;
        }
        clear_decoder();

        fmt = avformat_alloc_context();
        if(!fmt) {
            return false;
        }
        fmt->interrupt_callback.callback = &Impl::avio_interrupt_cb;
        fmt->interrupt_callback.opaque = this;
        interrupt_requested.store(false, std::memory_order_relaxed);

        if(avformat_open_input(&fmt, path, nullptr, nullptr) < 0) {
            clear_decoder();
            return false;
        }
        if(avformat_find_stream_info(fmt, nullptr) < 0) {
            return false;
        }
        stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if(stream_index < 0) {
            return false;
        }
        stream = fmt->streams[stream_index];
        time_base = stream->time_base;

        const AVCodec* codec_def = avcodec_find_decoder(stream->codecpar->codec_id);
        if(!codec_def) {
            return false;
        }
        codec = avcodec_alloc_context3(codec_def);
        if(!codec) {
            return false;
        }
        if(avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
            return false;
        }
        if(avcodec_open2(codec, codec_def, nullptr) < 0) {
            return false;
        }
        frame_w = codec->width;
        frame_h = codec->height;
        if(frame_w <= 0 || frame_h <= 0) {
            return false;
        }
        sws = sws_getContext(frame_w, frame_h, codec->pix_fmt,
                             frame_w, frame_h, AV_PIX_FMT_RGB565LE,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if(!sws) {
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if(!packet || !frame) {
            return false;
        }
        if(!ensure_buffers()) {
            return false;
        }

        if(fmt->duration != AV_NOPTS_VALUE) {
            duration_ms = fmt->duration / (AV_TIME_BASE / 1000);
        } else if(stream->duration != AV_NOPTS_VALUE) {
            duration_ms = pts_to_ms(stream->duration, time_base);
        } else {
            duration_ms = 0;
        }
        duration_ms = clamp_non_negative(duration_ms);
        position_ms = 0;
        eof.store(false, std::memory_order_relaxed);
        return true;
    }

    bool convert_frame(AVFrame* src, std::vector<uint8_t>& dst, lv_image_dsc_t& dsc)
    {
        if(!src || !sws) {
            return false;
        }
        uint8_t* dst_data[4] = { dst.data(), nullptr, nullptr, nullptr };
        int dst_linesize[4] = { frame_w * 2, 0, 0, 0 };
        const int out_h = sws_scale(sws, src->data, src->linesize, 0, frame_h,
                                    dst_data, dst_linesize);
        if(out_h != frame_h) {
            return false;
        }
        dsc.data = dst.data();
        dsc.data_size = dst.size();
        return true;
    }

    bool decode_one_video_frame_locked(bool for_preview, int64_t target_ms = -1)
    {
        while(!stop.load(std::memory_order_relaxed)) {
            const int rr = av_read_frame(fmt, packet);
            if(rr < 0) {
                if(!for_preview) {
                    eof.store(true, std::memory_order_relaxed);
                }
                return false;
            }
            if(packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            if(avcodec_send_packet(codec, packet) < 0) {
                av_packet_unref(packet);
                continue;
            }
            av_packet_unref(packet);

            while(!stop.load(std::memory_order_relaxed)) {
                const int gr = avcodec_receive_frame(codec, frame);
                if(gr == AVERROR(EAGAIN)) {
                    break;
                }
                if(gr == AVERROR_EOF) {
                    if(!for_preview) {
                        eof.store(true, std::memory_order_relaxed);
                    }
                    return false;
                }
                if(gr < 0) {
                    return false;
                }

                const int64_t frame_ms = pts_to_ms(frame->best_effort_timestamp, time_base);
                if(target_ms >= 0 && frame_ms + 15 < target_ms) {
                    av_frame_unref(frame);
                    continue;
                }

                if(for_preview) {
                    if(!convert_frame(frame, preview_rgb565, preview_dsc)) {
                        av_frame_unref(frame);
                        return false;
                    }
                    av_frame_unref(frame);
                    notify_preview(frame_ms);
                    return true;
                }

                if(!convert_frame(frame, main_rgb565, main_dsc)) {
                    av_frame_unref(frame);
                    return false;
                }
                int64_t emit_pos = frame_ms;
                int64_t emit_dur = 0;
                {
                    std::lock_guard<std::mutex> state_lk(mtx);
                    position_ms = frame_ms;
                    if(duration_ms > 0) {
                        position_ms = std::min(position_ms, duration_ms);
                    }
                    emit_pos = position_ms;
                    emit_dur = duration_ms;
                }
                av_frame_unref(frame);
                notify_frame_locked(emit_pos, emit_dur);
                return true;
            }
        }
        return false;
    }

    bool decode_one_video_frame(bool for_preview, int64_t target_ms = -1)
    {
        std::lock_guard<std::mutex> decoder_lk(decoder_mtx);
        return decode_one_video_frame_locked(for_preview, target_ms);
    }

    bool seek_to_ms(int64_t pos_ms, bool preview_only)
    {
        std::lock_guard<std::mutex> decoder_lk(decoder_mtx);
        interrupt_requested.store(false, std::memory_order_relaxed);
        if(!fmt || !codec || stream_index < 0) {
            return false;
        }
        const int64_t clamped_ms = duration_ms > 0
            ? std::clamp(clamp_non_negative(pos_ms), int64_t{0}, duration_ms)
            : clamp_non_negative(pos_ms);
        const int64_t target_pts = ms_to_pts(clamped_ms, time_base);
        if(av_seek_frame(fmt, stream_index, target_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codec);
        eof.store(false, std::memory_order_relaxed);
        if(!decode_one_video_frame_locked(preview_only, clamped_ms)) {
            return false;
        }
        return true;
    }

    void push_command(CommandType type, int64_t ms)
    {
        if(type == CommandType::Preview || type == CommandType::Seek) {
            commands.erase(std::remove_if(commands.begin(), commands.end(),
                                          [type](const Command& c) {
                                              return c.type == type;
                                          }),
                           commands.end());
        }
        commands.push_back(Command{type, ms});
        if(type == CommandType::Seek || type == CommandType::Preview) {
            interrupt_requested.store(true, std::memory_order_relaxed);
        }
        cv.notify_all();
    }

    void decode_loop()
    {
        std::unique_lock<std::mutex> lk(mtx);
        while(!stop.load(std::memory_order_relaxed)) {
            if(!opened) {
                cv.wait(lk, [this] {
                    return stop.load(std::memory_order_relaxed) || opened;
                });
                continue;
            }

            if(!commands.empty()) {
                Command cmd = commands.front();
                commands.pop_front();
                lk.unlock();
                if(cmd.type == CommandType::Seek) {
                    (void)seek_to_ms(cmd.ms, false);
                } else if(cmd.type == CommandType::Preview) {
                    (void)seek_to_ms(cmd.ms, true);
                }
                lk.lock();
                continue;
            }

            if(playing) {
                lk.unlock();
                const bool ok = decode_one_video_frame(false);
                lk.lock();
                if(!ok) {
                    playing = false;
                    if(eof.load(std::memory_order_relaxed)) {
                        position_ms = duration_ms;
                        notify_ended();
                    }
                }
            } else {
                cv.wait(lk, [this] {
                    return stop.load(std::memory_order_relaxed) ||
                           !commands.empty() || playing;
                });
            }
        }
    }
};

VideoPlayer::VideoPlayer()
{
    impl_ = new Impl();
    impl_->decode_thread = std::thread([this] {
        impl_->decode_loop();
    });
}

VideoPlayer::~VideoPlayer()
{
    close();
    if(impl_) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->stop = true;
            impl_->cv.notify_all();
        }
        if(impl_->decode_thread.joinable()) {
            impl_->decode_thread.join();
        }
        impl_->clear_decoder();
        delete impl_;
        impl_ = nullptr;
    }
}

VideoPlayer& VideoPlayer::instance()
{
    static VideoPlayer player;
    return player;
}

bool VideoPlayer::open(lv_obj_t* image_target, const char* path)
{
    if(!impl_) {
        return false;
    }
    close();
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->image_target = image_target;
        if(!impl_->open_decoder(path)) {
            impl_->opened = false;
            return false;
        }
        impl_->opened = true;
        impl_->playing = false;
        impl_->commands.clear();
        impl_->cv.notify_all();
    }
    if(!impl_->seek_to_ms(0, false)) {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->opened = false;
        impl_->clear_decoder();
        return false;
    }
    return true;
}

void VideoPlayer::close()
{
    if(!impl_) {
        return;
    }
    /* 锁顺序统一为 decoder_mtx -> mtx，避免与解码线程（decoder->mtx）反向死锁。 */
    impl_->interrupt_requested.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> dlk(impl_->decoder_mtx);
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->playing = false;
        impl_->opened = false;
        impl_->commands.clear();
        impl_->position_ms = 0;
        impl_->duration_ms = 0;
        impl_->cv.notify_all();
    }
    impl_->clear_decoder();
    impl_->interrupt_requested.store(false, std::memory_order_relaxed);
}

bool VideoPlayer::play()
{
    if(!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if(!impl_->opened) {
        return false;
    }
    impl_->playing = true;
    impl_->cv.notify_all();
    return true;
}

void VideoPlayer::pause()
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->playing = false;
    impl_->interrupt_requested.store(true, std::memory_order_relaxed);
}

bool VideoPlayer::seek(int64_t position_ms)
{
    if(!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if(!impl_->opened) {
        return false;
    }
    impl_->push_command(Impl::CommandType::Seek, position_ms);
    return true;
}

void VideoPlayer::request_preview(int64_t position_ms)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if(!impl_->opened) {
        return;
    }
    impl_->push_command(Impl::CommandType::Preview, position_ms);
}

void VideoPlayer::set_progress_cb(ProgressCb cb)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->progress_cb = std::move(cb);
}

void VideoPlayer::set_ended_cb(EndedCb cb)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->ended_cb = std::move(cb);
}

void VideoPlayer::set_preview_cb(PreviewCb cb)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->preview_cb = std::move(cb);
}

bool VideoPlayer::is_open() const
{
    if(!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->opened;
}

bool VideoPlayer::is_playing() const
{
    if(!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->playing;
}

int64_t VideoPlayer::duration_ms() const
{
    if(!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->duration_ms;
}

int64_t VideoPlayer::position_ms() const
{
    if(!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->position_ms;
}

#else

struct VideoPlayer::Impl {};

VideoPlayer::VideoPlayer()
{
    impl_ = new Impl();
}

VideoPlayer::~VideoPlayer()
{
    delete impl_;
    impl_ = nullptr;
}

VideoPlayer& VideoPlayer::instance()
{
    static VideoPlayer player;
    return player;
}

bool VideoPlayer::open(lv_obj_t*, const char*)
{
    return false;
}

void VideoPlayer::close() {}

bool VideoPlayer::play()
{
    return false;
}

void VideoPlayer::pause() {}

bool VideoPlayer::seek(int64_t)
{
    return false;
}

void VideoPlayer::request_preview(int64_t) {}

void VideoPlayer::set_progress_cb(ProgressCb) {}

void VideoPlayer::set_ended_cb(EndedCb) {}

void VideoPlayer::set_preview_cb(PreviewCb) {}

bool VideoPlayer::is_open() const
{
    return false;
}

bool VideoPlayer::is_playing() const
{
    return false;
}

int64_t VideoPlayer::duration_ms() const
{
    return 0;
}

int64_t VideoPlayer::position_ms() const
{
    return 0;
}

#endif

} // namespace ui
