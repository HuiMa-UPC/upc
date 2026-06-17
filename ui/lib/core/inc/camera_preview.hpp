#ifndef CAMERA_PREVIEW_HPP
#define CAMERA_PREVIEW_HPP

#include "lvgl_wrap.hpp"
#include "msg_types.hpp"
#include "ipc_shm_ring.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace ui {

/**
 * @brief 打印中摄像头预览：SHM 消费 + RGA NV12→BGRX + lv_image(XRGB8888) 显示
 *
 * 与 rongsu_ui.defaults（LV_COLOR_DEPTH 32 / DRM EGL）对齐。
 * - start/stop 须在 LVGL 主线程调用（与 page_user_home 打印模式挂钩）
 * - 解码在独立线程；主线程 lv_timer 仅换帧指针
 */
class CameraPreviewPlayer {
public:
    static CameraPreviewPlayer& instance();

    /** 进程启动时 RGA 预热 + 注册 CameraMsgProxy 回调（ui_app::app_init） */
    void init();

    /** 进入打印模式：发 Start_Req，成功后开 SHM + 解码线程 */
    void start(lv_obj_t* image_widget, lv_obj_t* hint_widget);

    /** 退出打印 / 终止任务：停线程、关 SHM、发 Stop_Req */
    void stop();

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    CameraPreviewPlayer() = default;

    void on_stream_start_rsp(const middleware::CameraStreamInfoPayload& info);
    void apply_stream_start_ui(const middleware::CameraStreamInfoPayload& info);
    void begin_decode_thread();
    void decode_thread_fn();
    void reset_ui();
    void show_hint(const char* text);
    void schedule_ui_timer();
    void delete_ui_timer();

    static void ui_timer_cb(lv_timer_t* timer);
    static void async_apply_stream_start(void* user);
    bool convert_frame_rga(const upc_ipc::IpcShmFrameView& frame,
                           uint8_t* dst, size_t dst_bytes);

    lv_obj_t* image_ = nullptr;
    lv_obj_t* hint_  = nullptr;
    lv_timer_t* ui_timer_ = nullptr;

    upc_ipc::IpcShmRingConsumer consumer_;

    std::thread decode_thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_decode_{false};
    std::atomic<bool> stream_up_{false};
    bool start_req_sent_ = false;

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t fps_    = 25;
    uint32_t pixel_stride_px_ = 0;

    std::vector<uint8_t> frame_buf_[3];
    lv_image_dsc_t       img_dsc_[3]{};
    std::atomic<int>     display_buf_{-1};
    int                  shown_buf_ = -1;
    bool                 logged_first_ui_show_ = false;

    middleware::CameraStreamInfoPayload pending_start_rsp_{};
    bool pending_start_rsp_valid_ = false;
};

} // namespace ui

#endif
