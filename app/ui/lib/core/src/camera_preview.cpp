#include "camera_preview.hpp"
#include "camera_msg_proxy.hpp"
#include "page_user_home.hpp"
#include "msg_port.hpp"
#include "rs_log.hpp"

#include <rga/im2d.hpp>  // C++ 重载 importbuffer_fd(fd,size)/wrapbuffer_handle(...)，内部含 im2d.h
#include <rga/rga.h>

#include <linux/dma-buf.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace ui {

namespace {

constexpr uint32_t kUiRefreshPeriodMs = 33u;
/* 录像收尾（尤其 print_h264）存在短时抖动，适当放宽卡帧告警阈值。 */
constexpr uint64_t kPreviewStallWarnMs = 1200u;

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

uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void close_fd_vector(std::vector<int>& fds)
{
    for(int fd : fds) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
    fds.clear();
}

inline uint8_t clamp_u8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void nv12_to_bgrx_sw(const uint8_t* y_plane, const uint8_t* uv_plane,
                     int w, int h, int stride_y, uint8_t* dst, int dst_stride_px)
{
    if(!y_plane || !dst || w <= 0 || h <= 0) {
        return;
    }

    for(int row = 0; row < h; ++row) {
        const uint8_t* y_row   = y_plane + static_cast<size_t>(stride_y) * row;
        const uint8_t* uv_row  = uv_plane
                                     ? uv_plane + static_cast<size_t>(stride_y) * (row / 2)
                                     : y_plane + static_cast<size_t>(stride_y) * h +
                                           static_cast<size_t>(stride_y) * (row / 2);
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

void dma_buf_sync_start_read(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
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

void dma_buf_sync_start_write(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void dma_buf_sync_end_read(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int pick_write_buf(int candidate, int shown, int display, int buf_count)
{
    for(int i = 0; i < buf_count; ++i) {
        const int idx = (candidate + i) % buf_count;
        if(idx != shown && idx != display) {
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

void CameraPreviewPlayer::close_dma_fds()
{
    close_active_dma_fds();
    close_pending_dma_fds();
}

void CameraPreviewPlayer::close_active_dma_fds()
{
    close_fd_vector(dma_fds_);
    dma_nv12_bytes_ = 0;
}

void CameraPreviewPlayer::close_pending_dma_fds()
{
    close_fd_vector(pending_dma_fds_);
}

int CameraPreviewPlayer::dma_fd(uint32_t idx) const noexcept
{
    if(idx >= dma_fds_.size()) {
        return -1;
    }
    return dma_fds_[idx];
}

void CameraPreviewPlayer::init()
{
    CameraMsgProxy::instance().register_stream_start_rsp(
        [this](const middleware::CameraStreamInfoPayload& info) {
            on_stream_start_rsp(info);
        });
}

void CameraPreviewPlayer::start(lv_obj_t* image_widget, lv_obj_t* hint_widget,
                                 const char* scene)
{
    if(decode_retiring_.load(std::memory_order_acquire)) {
        UPC_LOG_CAMERA_PREVIEW(Warn,
            "start deferred: previous decode session is still retiring");
        return;
    }
    if(active_.load()) {
        stop();
    }

    image_     = image_widget;
    hint_      = hint_widget;
    if(scene && scene[0]) {
        std::strncpy(scene_, scene, sizeof(scene_) - 1);
        scene_[sizeof(scene_) - 1] = '\0';
    } else {
        scene_[0] = '\0';
    }
    shown_buf_.store(-1, std::memory_order_relaxed);
    logged_first_ui_show_ = false;
    active_.store(true);
    stream_up_.store(false);
    display_buf_.store(-1);
    last_frame_ok_ms_.store(monotonic_ms(), std::memory_order_relaxed);
    stalled_.store(false, std::memory_order_relaxed);

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
    UPC_LOG_CAMERA_PREVIEW(Info,
        "start req scene=%s image=%p hint=%p",
        scene_[0] ? scene_ : "(none)",
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
    stalled_.store(false, std::memory_order_relaxed);
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

    if(decode_thread_.joinable()) {
        request_decode_retire_ui();
    } else {
        reset_ui();
    }
}

void CameraPreviewPlayer::on_stream_start_rsp(
    const middleware::CameraStreamInfoPayload& info)
{
    UPC_LOG_CAMERA_PREVIEW(Info,
        "stream start rsp ok=%d %ux%u@%u dma_mode=%u shm=%s active=%d",
        info.ok, info.width, info.height, info.fps, info.dma_mode, info.shm_name,
        active_.load() ? 1 : 0);

    if(!active_.load()) {
        if(info.ok) {
            std::vector<int> drain;
            if(middleware::MsgPort::instance().take_peer_dma_fds(
                   middleware::ModuleId::APP_CAMERA, drain)) {
                for(int fd : drain) {
                    if(fd >= 0) {
                        ::close(fd);
                    }
                }
            }
            if(middleware::MsgPort::instance().peer_connected(
                   middleware::ModuleId::APP_CAMERA)) {
                CameraMsgProxy::instance().send_stream_stop();
            }
        }
        return;
    }

    /*
     * Camera watchdog can push a replacement response while the decode thread
     * still owns the active dma fds.  Only replace the staged response here;
     * async_apply_stream_start() stops that thread before releasing old fds.
     */
    close_pending_dma_fds();
    if(info.ok && info.dma_mode) {
        if(!middleware::MsgPort::instance().take_peer_dma_fds(
               middleware::ModuleId::APP_CAMERA, pending_dma_fds_)) {
            UPC_LOG_CAMERA_PREVIEW(Error, "IPC dma fd take failed");
        }
    }

    pending_start_rsp_       = info;
    pending_start_rsp_valid_ = true;
    lv_async_call(async_apply_stream_start, this);
}

void CameraPreviewPlayer::async_apply_stream_start(void* user)
{
    auto* self = static_cast<CameraPreviewPlayer*>(user);
    if(!self || !self->pending_start_rsp_valid_ || !self->active_.load()) {
        if(self) {
            self->close_pending_dma_fds();
            self->pending_start_rsp_valid_ = false;
        }
        return;
    }
    if(self->decode_retiring_.load(std::memory_order_acquire)) {
        return;
    }
    if(self->decode_thread_.joinable()) {
        self->request_decode_retire_ui();
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
    rga_path_ready_ = false;
}

bool CameraPreviewPlayer::setup_frame_buffers(size_t frame_bytes,
                                              uint32_t row_stride_bytes)
{
    clear_frame_buffers();

    rga_path_ready_ = true;
    for(int i = 0; i < kFrameSlots; ++i) {
        if(!slots_[i].alloc(frame_bytes, true)) {
            rga_path_ready_ = false;
            break;
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

    /*
     * A successful Camera stream response may be the normal reply to Start
     * or a watchdog resync.  In either case the prior decode thread can still
     * access its SHM mapping and dma fds, so stop it before replacing them.
     */
    if(decode_thread_.joinable()) {
        pending_start_rsp_       = info;
        pending_start_rsp_valid_ = true;
        request_decode_retire_ui();
        return;
    }
    stream_up_.store(false);
    consumer_.close();
    display_buf_.store(-1, std::memory_order_relaxed);
    shown_buf_.store(-1, std::memory_order_relaxed);
    show_hint(LV_SYMBOL_VIDEO "  Reconnecting camera...");
    close_active_dma_fds();
    clear_frame_buffers();

    if(!info.ok) {
        close_pending_dma_fds();
        show_hint(LV_SYMBOL_WARNING "  Camera start failed");
        active_.store(false);
        return;
    }

    width_  = info.width;
    height_ = info.height;
    fps_    = info.fps > 0 ? info.fps : 25;

    if(width_ == 0 || height_ == 0) {
        close_pending_dma_fds();
        show_hint(LV_SYMBOL_WARNING "  Invalid stream size");
        active_.store(false);
        return;
    }

    if(consumer_.open(info.shm_name) != upc_ipc::IpcShmError::OK) {
        UPC_LOG_CAMERA_PREVIEW(Error, "dma frame ring open failed name=%s", info.shm_name);
        close_pending_dma_fds();
        show_hint(LV_SYMBOL_WARNING "  SHM open failed");
        active_.store(false);
        return;
    }

    UPC_LOG_CAMERA_PREVIEW(Info,
        "dma frame ring opened name=%s %ux%u stride_y=%u slots=%u format=0x%08x",
        info.shm_name, consumer_.width(), consumer_.height(),
        consumer_.stride_y(), consumer_.slot_count(), consumer_.format());

    const uint32_t ring_stride = consumer_.stride_y();
    src_stride_y_ = ring_stride > 0 ? ring_stride
                    : (info.stride_y > 0 ? info.stride_y : width_);

    const size_t src_size = static_cast<size_t>(src_stride_y_) * height_ * 3u / 2u;
    if(pending_dma_fds_.empty()) {
        UPC_LOG_CAMERA_PREVIEW(Error, "IPC dma fds not available");
        consumer_.close();
        show_hint(LV_SYMBOL_WARNING "  DMA fd missing");
        active_.store(false);
        return;
    }
    if(pending_dma_fds_.size() != info.dma_buf_count) {
        UPC_LOG_CAMERA_PREVIEW(Error,
            "IPC dma fd count mismatch got=%zu expected=%u",
            pending_dma_fds_.size(), info.dma_buf_count);
        close_pending_dma_fds();
        show_hint(LV_SYMBOL_WARNING "  DMA fd mismatch");
        active_.store(false);
        return;
    }
    for(int fd : pending_dma_fds_) {
        if(fd < 0) {
            UPC_LOG_CAMERA_PREVIEW(Error, "IPC dma fd invalid");
            close_pending_dma_fds();
            show_hint(LV_SYMBOL_WARNING "  DMA fd invalid");
            active_.store(false);
            return;
        }
    }
    dma_fds_ = std::move(pending_dma_fds_);
    pending_dma_fds_.clear();
    dma_nv12_bytes_ = src_size;
    UPC_LOG_CAMERA_PREVIEW(Info, "dma fds adopted from IPC count=%zu size=%zu",
                           dma_fds_.size(), dma_nv12_bytes_);

    pixel_stride_px_ = (width_ + 3u) & ~3u;
    const size_t frame_bytes =
        static_cast<size_t>(pixel_stride_px_) * height_ * 4u;
    const uint32_t row_stride_bytes = pixel_stride_px_ * 4u;

    if(!setup_frame_buffers(frame_bytes, row_stride_bytes)) {
        consumer_.close();
        close_active_dma_fds();
        show_hint(LV_SYMBOL_WARNING "  Preview buffer alloc failed");
        active_.store(false);
        return;
    }

    if(src_stride_y_ == 0) {
        src_stride_y_ = width_;
    }

    logged_first_ui_show_ = false;
    UPC_LOG_CAMERA_PREVIEW(Info,
        "decode ready scene=%s image=%p %ux%u@%u fps frame_bytes=%zu "
        "rga_path=%d row_stride=%u px_stride=%u src_stride=%u ui_timer=%ums",
        scene_[0] ? scene_ : "(none)", static_cast<void*>(image_),
        width_, height_, fps_, frame_bytes, rga_path_ready_ ? 1 : 0,
        row_stride_bytes, pixel_stride_px_, src_stride_y_, kUiRefreshPeriodMs);

    display_buf_.store(-1);
    shown_buf_ = -1;
    last_frame_ok_ms_.store(monotonic_ms(), std::memory_order_relaxed);
    stalled_.store(false, std::memory_order_relaxed);
    stream_up_.store(true);
    if(!begin_decode_thread()) {
        consumer_.close();
        close_active_dma_fds();
        clear_frame_buffers();
        show_hint(LV_SYMBOL_WARNING "  Preview decoder unavailable");
        active_.store(false);
        return;
    }
    schedule_ui_timer();
    show_hint(LV_SYMBOL_VIDEO "  Loading preview...");
}

void CameraPreviewPlayer::request_decode_retire_ui()
{
    if(decode_retiring_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    stop_decode_.store(true, std::memory_order_release);
    stream_up_.store(false, std::memory_order_release);
    decode_retired_.store(false, std::memory_order_release);
    display_buf_.store(-1, std::memory_order_relaxed);
    shown_buf_.store(-1, std::memory_order_relaxed);
    show_hint(LV_SYMBOL_VIDEO "  Reconnecting camera...");

    std::thread retiring_thread = std::move(decode_thread_);
    std::thread([thread = std::move(retiring_thread), this]() mutable {
        thread.join();
        decode_retired_.store(true, std::memory_order_release);
    }).detach();

    if(!retire_poll_timer_) {
        retire_poll_timer_ = lv_timer_create(retire_poll_timer_cb, 20u, this);
    }
}

void CameraPreviewPlayer::retire_poll_timer_cb(lv_timer_t* timer)
{
    auto* self = static_cast<CameraPreviewPlayer*>(lv_timer_get_user_data(timer));
    if(!self || !self->decode_retired_.load(std::memory_order_acquire)) {
        return;
    }
    self->finish_decode_retire_ui();
}

void CameraPreviewPlayer::finish_decode_retire_ui()
{
    if(!decode_retired_.load(std::memory_order_acquire)) {
        return;
    }
    if(retire_poll_timer_) {
        lv_timer_delete(retire_poll_timer_);
        retire_poll_timer_ = nullptr;
    }

    decode_retired_.store(false, std::memory_order_release);
    decode_retiring_.store(false, std::memory_order_release);
    consumer_.close();
    display_buf_.store(-1, std::memory_order_relaxed);
    shown_buf_.store(-1, std::memory_order_relaxed);
    close_active_dma_fds();
    clear_frame_buffers();

    if(!active_.load()) {
        reset_ui();
        return;
    }
    if(!pending_start_rsp_valid_) {
        return;
    }

    const middleware::CameraStreamInfoPayload info = pending_start_rsp_;
    pending_start_rsp_valid_ = false;
    apply_stream_start_ui(info);
}

bool CameraPreviewPlayer::begin_decode_thread()
{
    if(decode_thread_.joinable()) {
        UPC_LOG_CAMERA_PREVIEW(Error,
            "decode thread still joinable before start; refusing unsafe replacement");
        stream_up_.store(false);
        return false;
    }
    stop_decode_.store(false);
    decode_thread_ = std::thread(&CameraPreviewPlayer::decode_thread_fn, this);
    return true;
}

void CameraPreviewPlayer::decode_thread_fn()
{
    const auto frame_period = std::chrono::microseconds(
        1000000u / (fps_ > 0 ? fps_ : 1u));
    int write_idx = 0;
    bool logged_first_meta = false;
    bool logged_first_out = false;
    uint32_t frames_ok = 0;
    uint32_t frames_fail = 0;
    uint32_t frames_no_slot = 0;
    uint32_t last_seq = 0;
    uint64_t convert_us_sum = 0;
    uint64_t convert_us_max = 0;
    uint64_t loop_us_sum = 0;
    uint64_t loop_us_max = 0;
    auto stats_since = std::chrono::steady_clock::now();

    UPC_LOG_CAMERA_PREVIEW(Info,
        "decode thread started scene=%s image=%p target_fps=%u period_us=%lld "
        "rga_path=%d",
        scene_[0] ? scene_ : "(none)", static_cast<void*>(image_),
        fps_, static_cast<long long>(frame_period.count()),
        rga_path_ready_ ? 1 : 0);

    while(!stop_decode_.load(std::memory_order_relaxed)) {
        const auto tick_start = std::chrono::steady_clock::now();

        std::optional<upc_ipc::IpcDmaFrameView> frame =
            consumer_.acquire_latest_frame();
        if(!frame) {
            ++frames_no_slot;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
            if(!logged_first_meta) {
                UPC_LOG_CAMERA_PREVIEW(Info,
                    "first meta frame scene=%s slot=%u seq=%u buf_idx=%u "
                    "%ux%u stride=%u",
                    scene_[0] ? scene_ : "(none)",
                    frame->slot_index, frame->seq, frame->buffer_idx,
                    frame->width(), frame->height(), frame->stride_y());
                logged_first_meta = true;
            }
            last_seq = frame->seq;

            write_idx = pick_write_buf(write_idx,
                                       shown_buf_.load(std::memory_order_relaxed),
                                       display_buf_.load(std::memory_order_relaxed),
                                       kFrameSlots);
            const auto cvt_start = std::chrono::steady_clock::now();
            if(convert_frame(*frame, write_idx)) {
                ++frames_ok;
                last_frame_ok_ms_.store(monotonic_ms(), std::memory_order_relaxed);
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
            const uint64_t cvt_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - cvt_start)
                    .count());
            convert_us_sum += cvt_us;
            if(cvt_us > convert_us_max) {
                convert_us_max = cvt_us;
            }
            consumer_.release_slot(frame->slot_index);
        }

        const auto now = std::chrono::steady_clock::now();
        const uint64_t loop_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - tick_start)
                .count());
        loop_us_sum += loop_us;
        if(loop_us > loop_us_max) {
            loop_us_max = loop_us;
        }

        if(now - stats_since >= std::chrono::seconds(5)) {
            const uint32_t timed =
                frames_ok + frames_fail; /* 有 convert 计时的帧数 */
            const uint32_t loops = frames_ok + frames_fail + frames_no_slot;
            const uint64_t cvt_avg =
                timed > 0 ? (convert_us_sum / timed) : 0;
            const uint64_t loop_avg =
                loops > 0 ? (loop_us_sum / loops) : 0;
            UPC_LOG_CAMERA_PREVIEW(Debug,
                "decode 5s: scene=%s image=%p ok=%u fail=%u no_frame=%u "
                "last_seq=%u display_buf=%d "
                "cvt_avg_us=%llu cvt_max_us=%llu "
                "loop_avg_us=%llu loop_max_us=%llu rga=%d",
                scene_[0] ? scene_ : "(none)", static_cast<void*>(image_),
                frames_ok, frames_fail, frames_no_slot, last_seq,
                display_buf_.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(cvt_avg),
                static_cast<unsigned long long>(convert_us_max),
                static_cast<unsigned long long>(loop_avg),
                static_cast<unsigned long long>(loop_us_max),
                rga_path_ready_ ? 1 : 0);
            frames_ok = frames_no_slot = frames_fail = 0;
            convert_us_sum = convert_us_max = 0;
            loop_us_sum = loop_us_max = 0;
            stats_since = now;
        }

        const auto elapsed = now - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }

    UPC_LOG_CAMERA_PREVIEW(Info, "decode thread exiting scene=%s",
                           scene_[0] ? scene_ : "(none)");
}

bool CameraPreviewPlayer::convert_frame(const upc_ipc::IpcDmaFrameView& frame,
                                        int slot_idx)
{
    return convert_frame_dma(frame, slot_idx);
}

bool CameraPreviewPlayer::convert_frame_dma(const upc_ipc::IpcDmaFrameView& frame,
                                            int slot_idx)
{
    if(slot_idx < 0 || slot_idx >= kFrameSlots) {
        return false;
    }
    if(frame.width() != width_ || frame.height() != height_) {
        return false;
    }

    const int src_fd = dma_fd(frame.buffer_idx);
    if(src_fd < 0) {
        UPC_LOG_CAMERA_PREVIEW(Warn, "convert_frame_dma bad fd buf_idx=%u",
                               frame.buffer_idx);
        return false;
    }

    FrameSlot& slot = slots_[slot_idx];
    uint8_t* dst = slot.data();
    if(!dst || slot.size() == 0) {
        return false;
    }

    const int w             = static_cast<int>(width_);
    const int h             = static_cast<int>(height_);
    const int src_stride_y  = static_cast<int>(
        src_stride_y_ > 0
            ? src_stride_y_
            : (frame.stride_y() > 0 ? frame.stride_y() : width_));
    const int dst_stride_px = static_cast<int>(pixel_stride_px_);
    const size_t need_dst   =
        static_cast<size_t>(dst_stride_px) * static_cast<size_t>(h) * 4u;
    if(slot.size() < need_dst) {
        return false;
    }

    if(rga_path_ready_ && slot.is_dma()) {
        dma_buf_sync_start_read(src_fd);
        for(int attempt = 0; attempt < kRgaRetryCount; ++attempt) {
            IM_STATUS st = IM_STATUS_FAILED;
            if(rga_nv12_to_bgrx_fd(src_fd, slot.fd(), w, h,
                                   src_stride_y,
                                   dst_stride_px, &st)) {
                dma_buf_sync_end_read(src_fd);
                dma_buf_sync_end_write(slot.fd());
                return true;
            }
            if(attempt + 1 >= kRgaRetryCount) {
                static std::atomic<uint32_t> fail_log_count{0};
                const uint32_t n =
                    fail_log_count.fetch_add(1, std::memory_order_relaxed);
                if(n < 8u) {
                    UPC_LOG_CAMERA_PREVIEW(Warn,
                        "RGA cvt fail #%u st=%d (%s) %dx%d src_stride=%d attempt=%d/%d",
                        n + 1u, static_cast<int>(st), rga_status_str(st),
                        w, h, src_stride_y, attempt + 1, kRgaRetryCount);
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

    // RGA 失败或目的缓冲非 dma：结束 src 的 read sync，再 mmap 软件回退
    dma_buf_sync_end_read(src_fd);

    // SW 回退：mmap 源 fd
    const size_t src_size = dma_nv12_bytes_;
    dma_buf_sync_start_read(src_fd);
    void* src_va = mmap(nullptr, src_size, PROT_READ, MAP_SHARED, src_fd, 0);
    if(src_va == MAP_FAILED) {
        dma_buf_sync_end_read(src_fd);
        UPC_LOG_CAMERA_PREVIEW(Warn, "mmap src fd %d failed", src_fd);
        return false;
    }
    const uint8_t* src_y = static_cast<const uint8_t*>(src_va);
    const size_t y_plane_size = src_size * 2u / 3u;
    const uint8_t* src_uv = src_y + y_plane_size;
    if(slot.is_dma()) {
        dma_buf_sync_start_write(slot.fd());
    }
    nv12_to_bgrx_sw(src_y, src_uv, w, h, src_stride_y,
                    dst, dst_stride_px);
    munmap(src_va, src_size);
    dma_buf_sync_end_read(src_fd);
    if(slot.is_dma()) {
        dma_buf_sync_end_write(slot.fd());
    }
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

    if(self->stream_up_.load(std::memory_order_relaxed)) {
        const uint64_t now_ms = monotonic_ms();
        const uint64_t last_ok_ms =
            self->last_frame_ok_ms_.load(std::memory_order_relaxed);
        if(last_ok_ms > 0 && now_ms > last_ok_ms) {
            const uint64_t age_ms = now_ms - last_ok_ms;
            if(age_ms > kPreviewStallWarnMs &&
               !self->stalled_.exchange(true, std::memory_order_relaxed)) {
                UPC_LOG_CAMERA_PREVIEW(Warn,
                    "preview stalled age=%llums scene=%s",
                    static_cast<unsigned long long>(age_ms),
                    self->scene_[0] ? self->scene_ : "(none)");
            }
            if(age_ms <= kPreviewStallWarnMs) {
                self->stalled_.store(false, std::memory_order_relaxed);
            }
        }
    }

    const int idx = self->display_buf_.load(std::memory_order_acquire);
    if(idx < 0 || idx >= kFrameSlots || !self->image_) {
        return;
    }

    if(idx == self->shown_buf_.load(std::memory_order_relaxed)) {
        return;
    }
    self->shown_buf_.store(idx, std::memory_order_relaxed);

    const bool first_show = !self->logged_first_ui_show_;
    if(first_show) {
        self->logged_first_ui_show_ = true;
        UPC_LOG_CAMERA_PREVIEW(Info,
            "ui first show buf=%d w=%u h=%u stride=%u cf=XRGB8888",
            idx, self->img_dsc_[idx].header.w, self->img_dsc_[idx].header.h,
            self->img_dsc_[idx].header.stride);
    }

    lv_image_set_src(self->image_, &self->img_dsc_[idx]);
    if(first_show) {
        page_user_home_update_camera_preview_layout(self->width_, self->height_);
    }
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
    src_stride_y_    = 0;
    logged_first_ui_show_ = false;
    last_frame_ok_ms_.store(0, std::memory_order_relaxed);
    stalled_.store(false, std::memory_order_relaxed);
    pending_start_rsp_valid_ = false;
    decode_retiring_.store(false, std::memory_order_relaxed);
    decode_retired_.store(false, std::memory_order_relaxed);
    close_dma_fds();
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
