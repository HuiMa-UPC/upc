#ifndef CAMERA_PREVIEW_HPP
#define CAMERA_PREVIEW_HPP

#include "ipc_dma_frame_ring.hpp"
#include "lvgl_wrap.hpp"
#include "msg_runtime.hpp"
#include "rga_dma_buf.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace ui {

/**
 * @brief 打印中摄像头预览：dma fd + 元数据环 + RGA NV12→BGRX + lv_image(XRGB8888) 显示
 *
 * 与 rongsu_ui.defaults（LV_COLOR_DEPTH 32 / DRM EGL）对齐。
 * - start/stop 须在 LVGL 主线程调用（与 page_user_home 打印模式挂钩）
 * - 解码在独立线程；主线程 lv_timer 仅换帧指针
 */
class CameraPreviewPlayer {
public:
    static CameraPreviewPlayer& instance();

    /** 注册 CameraMsgProxy 回调（ui_app::app_init） */
    void init();

    /**
     * @brief 进入预览：发 Start_Req，成功后开 dma 元数据环 + 解码线程
     * @param scene 场景标签（如 "monitor"/"print"），仅用于诊断日志；可为 nullptr
     */
    void start(lv_obj_t* image_widget, lv_obj_t* hint_widget,
               const char* scene = nullptr);

    /** 退出打印 / 终止任务：停线程、关资源、发 Stop_Req */
    void stop();

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] uint32_t stream_width() const noexcept { return width_; }
    [[nodiscard]] uint32_t stream_height() const noexcept { return height_; }

private:
    static constexpr int kFrameSlots = 3;

    /** 显示缓冲：优先 dma32，失败时退化为 heap vector */
    struct FrameSlot {
        RgaDmaBuf            dma;
        std::vector<uint8_t> sw;

        void clear();
        bool alloc(size_t bytes, bool use_dma);
        [[nodiscard]] uint8_t* data() noexcept;
        [[nodiscard]] size_t size() const noexcept;
        [[nodiscard]] int fd() const noexcept;
        [[nodiscard]] bool is_dma() const noexcept { return dma.valid(); }
    };

    /** 后台等待旧解码线程退出的独立状态，不引用 Player 生命周期。 */
    struct RetireState {
        std::atomic<bool> done{false};
    };

    CameraPreviewPlayer() = default;

    void on_stream_start_rsp(const middleware::CameraStreamInfoPayload& info);
    void apply_stream_start_ui(const middleware::CameraStreamInfoPayload& info);
    bool begin_decode_thread();
    void decode_thread_fn();
    void reset_ui();
    void show_hint(const char* text);
    void close_dma_fds();
    void close_active_dma_fds();
    void close_pending_dma_fds();
    [[nodiscard]] int dma_fd(uint32_t idx) const noexcept;
    void schedule_ui_timer();
    void delete_ui_timer();
    void request_decode_retire_ui();
    void finish_decode_retire_ui();
    void clear_frame_buffers();
    bool setup_frame_buffers(size_t frame_bytes, uint32_t row_stride_bytes);

    static void ui_timer_cb(lv_timer_t* timer);
    static void retire_poll_timer_cb(lv_timer_t* timer);
    static void async_apply_stream_start(void* user);
    bool convert_frame(const upc_ipc::IpcDmaFrameView& frame, int slot_idx);
    bool convert_frame_dma(const upc_ipc::IpcDmaFrameView& frame, int slot_idx);
    bool convert_frame_rga_streaming(int src_fd, uint8_t* dst,
                                     int w, int h, int src_stride_y,
                                     int dst_stride_px);

    lv_obj_t* image_ = nullptr;
    lv_obj_t* hint_  = nullptr;
    lv_timer_t* ui_timer_ = nullptr;
    lv_timer_t* retire_poll_timer_ = nullptr;
    char scene_[32] = {}; /**< start() 传入的场景标签，供诊断日志 */
    lv_obj_t* pending_image_ = nullptr;
    lv_obj_t* pending_hint_  = nullptr;
    char pending_scene_[32] = {};
    bool pending_start_ = false;

    upc_ipc::IpcDmaFrameRingConsumer consumer_;

    std::vector<int> dma_fds_;
    std::vector<int> pending_dma_fds_;
    size_t           dma_nv12_bytes_ = 0;

    std::thread decode_thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_decode_{false};
    std::atomic<bool> stream_up_{false};
    std::atomic<bool> decode_retiring_{false};
    std::shared_ptr<RetireState> retire_state_;
    bool start_req_sent_ = false;

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t fps_    = 25;
    uint32_t pixel_stride_px_ = 0;
    uint32_t src_stride_y_    = 0; /**< 源 NV12 实际 stride（字节） */

    FrameSlot            slots_[kFrameSlots];
    bool                 rga_path_ready_ = false;
    lv_image_dsc_t       img_dsc_[kFrameSlots]{};
    std::atomic<int>     display_buf_{-1};
    std::atomic<int>     shown_buf_{-1};
    bool                 logged_first_ui_show_ = false;
    std::atomic<uint64_t> last_frame_ok_ms_{0};
    std::atomic<bool>     stalled_{false};

#if defined(UPC_UI_RGA_STREAMING) && UPC_UI_RGA_STREAMING
    RgaDmaBuf rga_stream_src_;
    RgaDmaBuf rga_stream_dst_;
    uint32_t  rga_stream_lines_ = 128;
#endif

    middleware::CameraStreamInfoPayload pending_start_rsp_{};
    bool pending_start_rsp_valid_ = false;
};

} // namespace ui

#endif /* CAMERA_PREVIEW_HPP */
