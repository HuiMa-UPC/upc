#include "camera_preview.hpp"
#include "camera_msg_proxy.hpp"
#include "msg_port.hpp"
#include "rs_log.hpp"

#include <rga/im2d.h>
#include <rga/rga.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

//临时
// #include <fstream>

namespace ui {

namespace {

constexpr int kFrameBufCount = 2;
/** 主线程刷新上限 ~20fps，减轻 lv_timer_handler 占用 */
constexpr uint32_t kUiRefreshPeriodMs = 50u;

/** 标准 NV12（Cb:Cr）；Rockchip 枚举 YCrCb_420_SP */
constexpr int kNv12RgaFormat = RK_FORMAT_YCbCr_420_SP;
/** LVGL XRGB8888 + NanoVG 纹理按 BGRX 内存布局上传 */
constexpr int kDstRgaFormat = RK_FORMAT_BGRX_8888;


//临时
// void dump_bgrx_frame_once(const char* path, const uint8_t* data, size_t bytes)
// {
//     static std::atomic<bool> done{false};
//     if(done.exchange(true)) {
//         return;
//     }
//     std::ofstream out(path, std::ios::binary);
//     if(!out) {
//         UPC_LOG_CAMERA_PREVIEW(Error, "dump open failed: %s", path);
//         return;
//     }
//     out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
//     UPC_LOG_CAMERA_PREVIEW(Info, "dumped bgrx %zu bytes -> %s", bytes, path);
// }

const char* rga_status_str(IM_STATUS st)
{
    const char* msg = imStrError(st);
    return msg ? msg : "unknown";
}

void log_nv12_probe(const char* tag, const upc_ipc::IpcShmFrameView& frame)
{
    if(!frame.y_plane || frame.width == 0 || frame.height == 0) {
        return;
    }
    const uint32_t sy =
        frame.stride_y > 0 ? frame.stride_y : frame.width;
    const size_t y_size = static_cast<size_t>(sy) * frame.height;
    const uint8_t* y = frame.y_plane;
    const uint8_t* uv = frame.uv_plane ? frame.uv_plane : (y + y_size);
    const size_t mid_y_off =
        static_cast<size_t>(sy) * (frame.height / 2u) + frame.width / 2u;

    UPC_LOG_CAMERA_PREVIEW(Info,
        "%s nv12 probe slot=%u seq=%u %ux%u stride_y=%u y=%p uv=%p "
        "Y[0]=%u Y[mid]=%u UV[0]=%u UV[1]=%u",
        tag, frame.slot_index, frame.seq, frame.width, frame.height, sy,
        static_cast<const void*>(y), static_cast<const void*>(uv),
        y[0], y[mid_y_off < y_size ? mid_y_off : 0], uv[0], uv[1]);
}

void log_xrgb_probe(const char* tag, const uint8_t* dst, uint32_t w, uint32_t h,
                    uint32_t pixel_stride_px)
{
    if(!dst || w == 0 || h == 0 || pixel_stride_px == 0) {
        return;
    }
    const size_t mid = (static_cast<size_t>(pixel_stride_px) * (h / 2u) +
                        w / 2u) *
                       4u;
    const uint32_t px0 = static_cast<uint32_t>(dst[0]) |
                         (static_cast<uint32_t>(dst[1]) << 8) |
                         (static_cast<uint32_t>(dst[2]) << 16) |
                         (static_cast<uint32_t>(dst[3]) << 24);
    const uint32_t pxm = static_cast<uint32_t>(dst[mid]) |
                         (static_cast<uint32_t>(dst[mid + 1]) << 8) |
                         (static_cast<uint32_t>(dst[mid + 2]) << 16) |
                         (static_cast<uint32_t>(dst[mid + 3]) << 24);
    UPC_LOG_CAMERA_PREVIEW(Info,
        "%s bgrx probe px[0]=0x%08x px[mid]=0x%08x stride_px=%u",
        tag, px0, pxm, pixel_stride_px);
}

} // namespace

CameraPreviewPlayer& CameraPreviewPlayer::instance()
{
    static CameraPreviewPlayer inst;
    return inst;
}

void CameraPreviewPlayer::init()
{
    CameraMsgProxy::instance().register_stream_start_rsp(
        [this](const middleware::CameraStreamInfoPayload& info) {
            on_stream_start_rsp(info);
        });
}

void CameraPreviewPlayer::start(lv_obj_t* image_widget, lv_obj_t* hint_widget)
{
    if(active_) {
        stop();
    }

    image_     = image_widget;
    hint_      = hint_widget;
    shown_buf_ = -1;
    logged_first_ui_show_ = false;
    active_.store(true);
    stream_up_.store(false);
    display_buf_.store(-1);

    if(image_) {
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }

    if(!middleware::MsgPort::instance().peer_connected(
           middleware::ModuleId::APP_CAMERA)) {
        show_hint(LV_SYMBOL_WARNING "  Camera offline");
        active_.store(false);
        return;
    }

    show_hint(LV_SYMBOL_VIDEO "  Connecting camera...");
    start_req_sent_ = true;
    UPC_LOG_CAMERA_PREVIEW(Info, "start req image=%p hint=%p",
                           static_cast<void*>(image_widget),
                           static_cast<void*>(hint_widget));
    CameraMsgProxy::instance().send_stream_start(nullptr);
}

void CameraPreviewPlayer::stop()
{
    const bool had_session =
        active_.load() || stream_up_.load() || start_req_sent_;

    active_.store(false);
    stop_decode_.store(true);
    UPC_LOG_CAMERA_PREVIEW(Info, "stop (had_session=%d stream_up=%d)",
                           had_session ? 1 : 0, stream_up_.load() ? 1 : 0);

    if(had_session &&
       middleware::MsgPort::instance().peer_connected(
           middleware::ModuleId::APP_CAMERA)) {
        CameraMsgProxy::instance().send_stream_stop();
    }
    start_req_sent_ = false;

    if(decode_thread_.joinable()) {
        decode_thread_.join();
    }

    consumer_.close();
    stream_up_.store(false);

    delete_ui_timer();

    if(image_) {
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }
    if(hint_) {
        lv_label_set_text(hint_,
            LV_SYMBOL_VIDEO "  Camera not connected\n"
            "(live cabin feed will appear while printing)");
        lv_obj_clear_flag(hint_, LV_OBJ_FLAG_HIDDEN);
    }

    reset_ui();
}

void CameraPreviewPlayer::on_stream_start_rsp(
    const middleware::CameraStreamInfoPayload& info)
{
    if(!active_.load()) {
        if(info.ok &&
           middleware::MsgPort::instance().peer_connected(
               middleware::ModuleId::APP_CAMERA)) {
            CameraMsgProxy::instance().send_stream_stop();
        }
        return;
    }

    pending_start_rsp_       = info;
    pending_start_rsp_valid_ = true;
    lv_async_call(async_apply_stream_start, this);
}

void CameraPreviewPlayer::async_apply_stream_start(void* user)
{
    auto* self = static_cast<CameraPreviewPlayer*>(user);
    if(!self || !self->pending_start_rsp_valid_ || !self->active_.load()) {
        return;
    }
    self->pending_start_rsp_valid_ = false;
    self->apply_stream_start_ui(self->pending_start_rsp_);
}

void CameraPreviewPlayer::apply_stream_start_ui(
    const middleware::CameraStreamInfoPayload& info)
{
    if(!active_.load()) {
        return;
    }

    if(!info.ok) {
        show_hint(LV_SYMBOL_WARNING "  Camera start failed");
        active_.store(false);
        return;
    }

    width_  = info.width;
    height_ = info.height;
    fps_    = info.fps > 0 ? info.fps : 25;

    if(width_ == 0 || height_ == 0) {
        show_hint(LV_SYMBOL_WARNING "  Invalid stream size");
        active_.store(false);
        return;
    }

    consumer_.close();
    if(consumer_.open(info.shm_name) != upc_ipc::IpcShmError::Ok) {
        UPC_LOG_CAMERA_PREVIEW(Error, "SHM open failed name=%s", info.shm_name);
        show_hint(LV_SYMBOL_WARNING "  SHM open failed");
        active_.store(false);
        return;
    }

    if(const upc_ipc::IpcShmGlobalHeader* hdr = consumer_.header()) {
        UPC_LOG_CAMERA_PREVIEW(Info,
            "SHM opened name=%s hdr %ux%u stride_y=%u slots=%u slot_stride=%u "
            "format=%u payload_bytes=%u",
            info.shm_name, hdr->width, hdr->height, hdr->stride_y,
            hdr->slot_count, hdr->slot_stride_bytes, hdr->format,
            hdr->payload_bytes);
    }

    pixel_stride_px_ = (width_ + 3u) & ~3u;
    const size_t frame_bytes =
        static_cast<size_t>(pixel_stride_px_) *
        static_cast<size_t>(height_) * 4u;
    const uint32_t row_stride_bytes = pixel_stride_px_ * 4u;

    for(int i = 0; i < kFrameBufCount; ++i) {
        frame_buf_[i].assign(frame_bytes, 0);
        img_dsc_[i]              = {};
        img_dsc_[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        img_dsc_[i].header.cf    = LV_COLOR_FORMAT_XRGB8888;
        img_dsc_[i].header.w     = width_;
        img_dsc_[i].header.h     = height_;
        img_dsc_[i].header.stride = row_stride_bytes;
        img_dsc_[i].data         = frame_buf_[i].data();
        img_dsc_[i].data_size    = frame_bytes;
    }

    logged_first_ui_show_ = false;
    UPC_LOG_CAMERA_PREVIEW(Info,
        "decode ready %ux%u@%u fps frame_bytes=%zu lv_cf=XRGB8888 "
        "row_stride=%u px_stride=%u ui_timer=%ums (LV_COLOR_DEPTH 32)",
        width_, height_, fps_, frame_bytes, row_stride_bytes, pixel_stride_px_,
        kUiRefreshPeriodMs);

    display_buf_.store(-1);
    shown_buf_ = -1;

    stream_up_.store(true);
    begin_decode_thread();
    schedule_ui_timer();
}

void CameraPreviewPlayer::begin_decode_thread()
{
    stop_decode_.store(false);
    if(decode_thread_.joinable()) {
        decode_thread_.join();
    }
    decode_thread_ = std::thread(&CameraPreviewPlayer::decode_thread_fn, this);
}

void CameraPreviewPlayer::decode_thread_fn()
{
    const auto frame_period = std::chrono::microseconds(
        1000000u / (fps_ > 0 ? fps_ : 1u));
    int write_idx = 0;
    bool logged_first_shm = false;
    bool logged_first_rga = false;
    uint32_t frames_ok = 0;
    uint32_t frames_rga_fail = 0;
    uint32_t frames_no_slot = 0;
    auto stats_since = std::chrono::steady_clock::now();

    UPC_LOG_CAMERA_PREVIEW(Info,
        "decode thread started target_fps=%u period_us=%lld nv12_fmt=YCrCb420SP "
        "dst_fmt=BGRX8888",
        fps_, static_cast<long long>(frame_period.count()));

    while(!stop_decode_.load(std::memory_order_relaxed)) {
        const auto tick_start = std::chrono::steady_clock::now();

        std::optional<upc_ipc::IpcShmFrameView> frame =
            consumer_.acquire_latest_frame();
        if(!frame || !frame->y_plane) {
            ++frames_no_slot;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
            if(!logged_first_shm) {
                log_nv12_probe("first_shm_frame", *frame);
                logged_first_shm = true;
            }

            const size_t dst_bytes = frame_buf_[write_idx].size();
            if(convert_frame_rga(*frame, frame_buf_[write_idx].data(), dst_bytes)) {
                ++frames_ok;
                display_buf_.store(write_idx, std::memory_order_release);
                if(!logged_first_rga) {
                    log_xrgb_probe("first_rga_out", frame_buf_[write_idx].data(),
                                   width_, height_, pixel_stride_px_);

                    //临时
                    // dump_bgrx_frame_once("/tmp/camera_rga_bgrx.bin",
                    //  frame_buf_[write_idx].data(),
                    //  frame_buf_[write_idx].size());

                    logged_first_rga = true;
                }
                write_idx = (write_idx + 1) % kFrameBufCount;
            } else {
                ++frames_rga_fail;
            }

            consumer_.release_slot(frame->slot_index);
        }

        const auto now = std::chrono::steady_clock::now();
        if(now - stats_since >= std::chrono::seconds(1)) {
            UPC_LOG_CAMERA_PREVIEW(Debug,
                "decode 1s: ok=%u rga_fail=%u no_frame=%u display_buf=%d write_idx=%d",
                frames_ok, frames_rga_fail, frames_no_slot,
                display_buf_.load(std::memory_order_relaxed), write_idx);
            frames_ok = 0;
            frames_rga_fail = 0;
            frames_no_slot = 0;
            stats_since = now;
        }

        const auto elapsed = now - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }

    UPC_LOG_CAMERA_PREVIEW(Info, "decode thread exiting");
}

bool CameraPreviewPlayer::convert_frame_rga(const upc_ipc::IpcShmFrameView& frame,
                                          uint8_t* dst, size_t dst_bytes)
{
    if(!dst || dst_bytes == 0 || !frame.y_plane || pixel_stride_px_ == 0) {
        return false;
    }

    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    const int stride_y = static_cast<int>(
        frame.stride_y > 0 ? frame.stride_y : frame.width);
    const int dst_stride_px = static_cast<int>(pixel_stride_px_);

    rga_buffer_t src = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(frame.y_plane), w, h,
        kNv12RgaFormat, stride_y, h);

    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(
        dst, w, h, kDstRgaFormat, dst_stride_px, h);

    const IM_STATUS st = imcvtcolor(src, dst_buf, kNv12RgaFormat, kDstRgaFormat);
    const bool ok = (st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR);
    if(!ok) {
        static std::atomic<uint32_t> fail_log_count{0};
        const uint32_t n =
            fail_log_count.fetch_add(1, std::memory_order_relaxed);
        if(n < 8u) {
            UPC_LOG_CAMERA_PREVIEW(Warn,
                "RGA imcvtcolor fail #%u st=%d (%s) %dx%d stride_y=%d "
                "dst_stride_px=%d dst_bytes=%zu",
                n + 1u, static_cast<int>(st), rga_status_str(st), w, h, stride_y,
                dst_stride_px, dst_bytes);
        } else if(n == 8u) {
            UPC_LOG_CAMERA_PREVIEW(Warn,
                "RGA imcvtcolor fail (suppressing further warnings)");
        }
    }
    return ok;
}

void CameraPreviewPlayer::schedule_ui_timer()
{
    delete_ui_timer();
    ui_timer_ = lv_timer_create(ui_timer_cb, kUiRefreshPeriodMs, this);
}

void CameraPreviewPlayer::delete_ui_timer()
{
    if(ui_timer_) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
}

void CameraPreviewPlayer::ui_timer_cb(lv_timer_t* timer)
{
    auto* self = static_cast<CameraPreviewPlayer*>(lv_timer_get_user_data(timer));
    if(!self || !self->active_.load()) {
        return;
    }

    const int idx = self->display_buf_.load(std::memory_order_acquire);
    if(idx < 0 || idx >= kFrameBufCount || !self->image_) {
        return;
    }

    if(idx == self->shown_buf_) {
        return;
    }
    self->shown_buf_ = idx;

    if(!self->logged_first_ui_show_) {
        self->logged_first_ui_show_ = true;
        UPC_LOG_CAMERA_PREVIEW(Info,
            "ui first show buf=%d w=%u h=%u stride=%u cf=XRGB8888",
            idx, self->img_dsc_[idx].header.w, self->img_dsc_[idx].header.h,
            self->img_dsc_[idx].header.stride);
    }

    lv_image_set_src(self->image_, &self->img_dsc_[idx]);
    lv_obj_clear_flag(self->image_, LV_OBJ_FLAG_HIDDEN);
    if(self->hint_) {
        lv_obj_add_flag(self->hint_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(self->image_);
}

void CameraPreviewPlayer::reset_ui()
{
    image_     = nullptr;
    hint_      = nullptr;
    shown_buf_ = -1;
    display_buf_.store(-1);
    width_  = 0;
    height_ = 0;
    pixel_stride_px_ = 0;
    logged_first_ui_show_ = false;
    pending_start_rsp_valid_ = false;

    for(int i = 0; i < kFrameBufCount; ++i) {
        frame_buf_[i].clear();
        img_dsc_[i] = {};
    }
}

void CameraPreviewPlayer::show_hint(const char* text)
{
    if(!hint_ || !text) {
        return;
    }
    lv_label_set_text(hint_, text);
    lv_obj_clear_flag(hint_, LV_OBJ_FLAG_HIDDEN);
    if(image_) {
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace ui
