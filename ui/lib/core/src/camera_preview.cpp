#include "camera_preview.hpp"
#include "camera_msg_proxy.hpp"
#include "msg_port.hpp"
#include "rs_log.hpp"

#include <rga/im2d.h>
#include <rga/rga.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace ui {

namespace {

constexpr uint32_t kUiRefreshPeriodMs = 33u;

constexpr int kNv12RgaFormat = RK_FORMAT_YCbCr_420_SP;
constexpr int kDstRgaFormat  = RK_FORMAT_BGRX_8888;

constexpr int kRgaRetryCount   = 3;
constexpr int kRgaRetryDelayMs = 10;

const char* rga_status_str(IM_STATUS st)
{
    const char* msg = imStrError(st);
    return msg ? msg : "unknown";
}

bool rga_status_ok(IM_STATUS st)
{
    return st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR;
}

bool rga_cvtcolor_checked(rga_buffer_t src, rga_buffer_t dst_buf, const im_rect& rect,
                          IM_STATUS* out_st);
bool rga_nv12_to_bgrx_fd(int src_fd, int dst_fd, int w, int h, int src_stride_y,
                         int dst_stride_px, IM_STATUS* out_st);

inline uint8_t clamp_u8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void nv12_to_bgrx_sw(const uint8_t* y_plane, const uint8_t* uv_plane,
                     int w, int h, int stride_y, uint8_t* dst, int dst_stride_px)
{
    if(!y_plane || !uv_plane || !dst || w <= 0 || h <= 0) {
        return;
    }

    for(int row = 0; row < h; ++row) {
        const uint8_t* y_row   = y_plane + static_cast<size_t>(stride_y) * row;
        const uint8_t* uv_row  = uv_plane + static_cast<size_t>(stride_y) * (row / 2);
        uint8_t* d_row =
            dst + static_cast<size_t>(dst_stride_px) * static_cast<size_t>(row) * 4u;

        for(int col = 0; col < w; ++col) {
            const int Y       = y_row[col];
            const int uv_off  = (col & ~1);
            const int U       = static_cast<int>(uv_row[uv_off]) - 128;
            const int V       = static_cast<int>(uv_row[uv_off + 1]) - 128;
            const int C       = Y - 16;
            const int R       = (298 * C + 409 * V + 128) >> 8;
            const int G       = (298 * C - 100 * U - 208 * V + 128) >> 8;
            const int B       = (298 * C + 516 * U + 128) >> 8;
            const size_t px   = static_cast<size_t>(col) * 4u;

            d_row[px + 0] = clamp_u8(B);
            d_row[px + 1] = clamp_u8(G);
            d_row[px + 2] = clamp_u8(R);
            d_row[px + 3] = 0;
        }
    }
}

bool rga_cvtcolor_checked(rga_buffer_t src, rga_buffer_t dst_buf, const im_rect& rect,
                          IM_STATUS* out_st)
{
    const IM_STATUS chk = imcheck(src, dst_buf, rect, rect);
    if(!rga_status_ok(chk)) {
        if(out_st) {
            *out_st = chk;
        }
        return false;
    }

    IM_STATUS st = imcvtcolor_t(src, dst_buf, kNv12RgaFormat, kDstRgaFormat,
                                IM_YUV_TO_RGB_BT601_LIMIT, 1);
    if(!rga_status_ok(st)) {
        st = imcvtcolor_t(src, dst_buf, kNv12RgaFormat, kDstRgaFormat,
                          IM_COLOR_SPACE_DEFAULT, 1);
    }
    if(out_st) {
        *out_st = st;
    }
    return rga_status_ok(st);
}

bool rga_nv12_to_bgrx_fd(int src_fd, int dst_fd, int w, int h, int src_stride_y,
                         int dst_stride_px, IM_STATUS* out_st)
{
    if(src_fd < 0 || dst_fd < 0 || w <= 0 || h <= 0 || src_stride_y <= 0 ||
       dst_stride_px <= 0) {
        return false;
    }

    const int src_bytes = src_stride_y * h * 3 / 2;
    const int dst_bytes = dst_stride_px * h * 4;

    rga_buffer_handle_t src_h = importbuffer_fd(src_fd, src_bytes);
    rga_buffer_handle_t dst_h = importbuffer_fd(dst_fd, dst_bytes);
    if(src_h == 0 || dst_h == 0) {
        if(src_h) {
            releasebuffer_handle(src_h);
        }
        if(dst_h) {
            releasebuffer_handle(dst_h);
        }
        if(out_st) {
            *out_st = IM_STATUS_FAILED;
        }
        return false;
    }

    rga_buffer_t src = wrapbuffer_handle(src_h, w, h, kNv12RgaFormat,
                                         src_stride_y, h);
    rga_buffer_t dst_buf = wrapbuffer_handle(dst_h, w, h, kDstRgaFormat,
                                             dst_stride_px, h);
    const im_rect rect{0, 0, w, h};
    const bool ok = rga_cvtcolor_checked(src, dst_buf, rect, out_st);

    releasebuffer_handle(src_h);
    releasebuffer_handle(dst_h);
    return ok;
}

void log_nv12_probe(const char* tag, const upc_ipc::IpcShmFrameView& frame)
{
    if(!frame.y_plane || frame.width == 0 || frame.height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(frame.width) * frame.height;
    const uint8_t* y   = frame.y_plane;
    const uint8_t* uv  = frame.uv_plane ? frame.uv_plane : (y + y_size);
    const size_t mid_y_off =
        static_cast<size_t>(frame.width) * (frame.height / 2u) + frame.width / 2u;

    UPC_LOG_CAMERA_PREVIEW(Info,
        "%s nv12 probe slot=%u seq=%u %ux%u y=%p uv=%p "
        "Y[0]=%u Y[mid]=%u UV[0]=%u UV[1]=%u",
        tag, frame.slot_index, frame.seq, frame.width, frame.height,
        static_cast<const void*>(y), static_cast<const void*>(uv),
        y[0], y[mid_y_off < y_size ? mid_y_off : 0], uv[0], uv[1]);
}

int pick_write_buf(int candidate, int shown, int buf_count)
{
    for(int i = 0; i < buf_count; ++i) {
        const int idx = (candidate + i) % buf_count;
        if(idx != shown) {
            return idx;
        }
    }
    return candidate;
}

void log_bgrx_probe(const char* tag, const uint8_t* dst, uint32_t w, uint32_t h,
                    uint32_t pixel_stride_px)
{
    if(!dst || w == 0 || h == 0 || pixel_stride_px == 0) {
        return;
    }
    const size_t mid = (static_cast<size_t>(pixel_stride_px) * (h / 2u) + w / 2u) * 4u;
    const uint32_t px0 =
        static_cast<uint32_t>(dst[0]) | (static_cast<uint32_t>(dst[1]) << 8) |
        (static_cast<uint32_t>(dst[2]) << 16) | (static_cast<uint32_t>(dst[3]) << 24);
    const uint32_t pxm =
        static_cast<uint32_t>(dst[mid]) | (static_cast<uint32_t>(dst[mid + 1]) << 8) |
        (static_cast<uint32_t>(dst[mid + 2]) << 16) |
        (static_cast<uint32_t>(dst[mid + 3]) << 24);
    UPC_LOG_CAMERA_PREVIEW(Info,
        "%s bgrx probe px[0]=0x%08x px[mid]=0x%08x stride_px=%u",
        tag, px0, pxm, pixel_stride_px);
}

} // namespace

void CameraPreviewPlayer::FrameSlot::clear()
{
    dma.free();
    sw.clear();
}

bool CameraPreviewPlayer::FrameSlot::alloc(size_t bytes, bool use_dma)
{
    clear();
    if(bytes == 0) {
        return false;
    }
    if(use_dma) {
        return dma.alloc(bytes);
    }
    sw.assign(bytes, 0);
    return true;
}

uint8_t* CameraPreviewPlayer::FrameSlot::data() noexcept
{
    return is_dma() ? static_cast<uint8_t*>(dma.va()) : sw.data();
}

size_t CameraPreviewPlayer::FrameSlot::size() const noexcept
{
    return is_dma() ? dma.size() : sw.size();
}

int CameraPreviewPlayer::FrameSlot::fd() const noexcept
{
    return is_dma() ? dma.fd() : -1;
}

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
    if(active_.load()) {
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
    UPC_LOG_CAMERA_PREVIEW(Info,
        "stream start rsp ok=%d %ux%u@%u shm=%s active=%d",
        info.ok, info.width, info.height, info.fps, info.shm_name,
        active_.load() ? 1 : 0);

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

void CameraPreviewPlayer::clear_frame_buffers()
{
    for(int i = 0; i < kFrameSlots; ++i) {
        slots_[i].clear();
        img_dsc_[i] = {};
    }
    nv12_staging_.free();
    rga_path_ready_ = false;
}

bool CameraPreviewPlayer::setup_frame_buffers(size_t frame_bytes, size_t nv12_bytes,
                                              uint32_t row_stride_bytes)
{
    clear_frame_buffers();

    rga_path_ready_ = nv12_staging_.alloc(nv12_bytes);
    if(rga_path_ready_) {
        for(int i = 0; i < kFrameSlots; ++i) {
            if(!slots_[i].alloc(frame_bytes, true)) {
                rga_path_ready_ = false;
                break;
            }
        }
    }

    if(!rga_path_ready_) {
        UPC_LOG_CAMERA_PREVIEW(Warn,
            "RGA dma32 alloc failed, using software NV12 convert");
        clear_frame_buffers();
        for(int i = 0; i < kFrameSlots; ++i) {
            if(!slots_[i].alloc(frame_bytes, false)) {
                return false;
            }
        }
    }

    for(int i = 0; i < kFrameSlots; ++i) {
        uint8_t* data = slots_[i].data();
        if(!data) {
            return false;
        }
        img_dsc_[i]              = {};
        img_dsc_[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        img_dsc_[i].header.cf    = LV_COLOR_FORMAT_XRGB8888;
        img_dsc_[i].header.w     = width_;
        img_dsc_[i].header.h     = height_;
        img_dsc_[i].header.stride = row_stride_bytes;
        img_dsc_[i].data         = data;
        img_dsc_[i].data_size    = frame_bytes;
    }
    return true;
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
        static_cast<size_t>(pixel_stride_px_) * height_ * 4u;
    const size_t nv12_bytes = static_cast<size_t>(width_) * height_ * 3u / 2u;
    const uint32_t row_stride_bytes = pixel_stride_px_ * 4u;

    if(!setup_frame_buffers(frame_bytes, nv12_bytes, row_stride_bytes)) {
        show_hint(LV_SYMBOL_WARNING "  Preview buffer alloc failed");
        active_.store(false);
        return;
    }

    logged_first_ui_show_ = false;
    UPC_LOG_CAMERA_PREVIEW(Info,
        "decode ready %ux%u@%u fps frame_bytes=%zu nv12_bytes=%zu "
        "rga_path=%d row_stride=%u px_stride=%u ui_timer=%ums",
        width_, height_, fps_, frame_bytes, nv12_bytes, rga_path_ready_ ? 1 : 0,
        row_stride_bytes, pixel_stride_px_, kUiRefreshPeriodMs);

    display_buf_.store(-1);
    shown_buf_ = -1;
    stream_up_.store(true);
    begin_decode_thread();
    schedule_ui_timer();
    show_hint(LV_SYMBOL_VIDEO "  Loading preview...");
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
    bool logged_first_out = false;
    uint32_t frames_ok = 0;
    uint32_t frames_fail = 0;
    uint32_t frames_no_slot = 0;
    auto stats_since = std::chrono::steady_clock::now();

    UPC_LOG_CAMERA_PREVIEW(Info,
        "decode thread started target_fps=%u period_us=%lld rga_path=%d",
        fps_, static_cast<long long>(frame_period.count()),
        rga_path_ready_ ? 1 : 0);

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

            write_idx = pick_write_buf(write_idx, shown_buf_, kFrameSlots);
            if(convert_frame(*frame, write_idx)) {
                ++frames_ok;
                display_buf_.store(write_idx, std::memory_order_release);
                if(!logged_first_out) {
                    log_bgrx_probe("first_convert_out", slots_[write_idx].data(),
                                   width_, height_, pixel_stride_px_);
                    logged_first_out = true;
                }
                write_idx = (write_idx + 1) % kFrameSlots;
            } else {
                ++frames_fail;
            }
            consumer_.release_slot(frame->slot_index);
        }

        const auto now = std::chrono::steady_clock::now();
        if(now - stats_since >= std::chrono::seconds(1)) {
            UPC_LOG_CAMERA_PREVIEW(Debug,
                "decode 1s: ok=%u fail=%u no_frame=%u display_buf=%d write_idx=%d",
                frames_ok, frames_fail, frames_no_slot,
                display_buf_.load(std::memory_order_relaxed), write_idx);
            frames_ok = frames_no_slot = frames_fail = 0;
            stats_since = now;
        }

        const auto elapsed = now - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }

    UPC_LOG_CAMERA_PREVIEW(Info, "decode thread exiting");
}

bool CameraPreviewPlayer::convert_frame(const upc_ipc::IpcShmFrameView& frame,
                                        int slot_idx)
{
    if(slot_idx < 0 || slot_idx >= kFrameSlots || !frame.y_plane) {
        return false;
    }
    if(frame.width != width_ || frame.height != height_) {
        return false;
    }

    FrameSlot& slot = slots_[slot_idx];
    uint8_t* dst = slot.data();
    if(!dst || slot.size() == 0) {
        return false;
    }

    const int w             = static_cast<int>(width_);
    const int h             = static_cast<int>(height_);
    const int dst_stride_px = static_cast<int>(pixel_stride_px_);
    const size_t nv12_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u / 2u;
    const size_t need_dst   =
        static_cast<size_t>(dst_stride_px) * static_cast<size_t>(h) * 4u;
    if(slot.size() < need_dst) {
        return false;
    }

    if(rga_path_ready_ && nv12_staging_.valid() &&
       nv12_staging_.size() >= nv12_bytes && slot.is_dma()) {
        std::memcpy(nv12_staging_.va(), frame.y_plane, nv12_bytes);

        for(int attempt = 0; attempt < kRgaRetryCount; ++attempt) {
            IM_STATUS st = IM_STATUS_FAILED;
            if(rga_nv12_to_bgrx_fd(nv12_staging_.fd(), slot.fd(), w, h, w,
                                   dst_stride_px, &st)) {
                return true;
            }

            if(attempt + 1 >= kRgaRetryCount) {
                static std::atomic<uint32_t> fail_log_count{0};
                const uint32_t n =
                    fail_log_count.fetch_add(1, std::memory_order_relaxed);
                if(n < 8u) {
                    UPC_LOG_CAMERA_PREVIEW(Warn,
                        "RGA cvt fail #%u st=%d (%s) %dx%d attempt=%d/%d",
                        n + 1u, static_cast<int>(st), rga_status_str(st),
                        w, h, attempt + 1, kRgaRetryCount);
                } else if(n == 8u) {
                    UPC_LOG_CAMERA_PREVIEW(Warn,
                        "RGA cvt fail (suppressing further warnings)");
                }
            } else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kRgaRetryDelayMs));
            }
        }

        static std::atomic<bool> sw_fallback_logged{false};
        if(!sw_fallback_logged.exchange(true)) {
            UPC_LOG_CAMERA_PREVIEW(Warn,
                "RGA failed at runtime, falling back to software NV12 convert");
        }
    }

    const uint8_t* uv = frame.uv_plane
                            ? frame.uv_plane
                            : frame.y_plane + frame.y_size;
    nv12_to_bgrx_sw(frame.y_plane, uv, w, h, w, dst, dst_stride_px);
    return true;
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
    if(idx < 0 || idx >= kFrameSlots || !self->image_) {
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
    clear_frame_buffers();
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
