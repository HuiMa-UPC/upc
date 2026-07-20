#ifndef VIDEO_PLAYER_HPP
#define VIDEO_PLAYER_HPP

#include "lvgl_wrap.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ui {

/**
 * @brief 延时摄影 MP4 播放器（A2：自研 libav* 软解 → BGRA → lv_image(XRGB8888)）
 *
 * 与 rongsu_ui.defaults（LV_COLOR_DEPTH 32 / DRM EGL）对齐。仿 CameraPreviewPlayer：
 * - 解码在独立线程，按 pts 节流；主线程 lv_timer 仅换帧指针（三缓冲）。
 * - open/close/play/pause/seek 均须在 LVGL 主线程调用。
 * - 进度回调在主线程 lv_timer 内触发，可安全操作 LVGL。
 *
 * RK3568 软解低帧率延时视频压力不大；为省内存/CPU，解码时把长边限制到
 * kMaxEdge，最终由 lv_image COVER 适配全屏。
 */
class VideoPlayer {
public:
    /** 进度回调：pos/dur 单位秒（在主线程 lv_timer 内调用） */
    using ProgressCb = std::function<void(double pos_sec, double dur_sec)>;
    /** 播放到结尾回调（主线程） */
    using EndedCb = std::function<void()>;
    /** 拖动预览回调：小图 dsc + 对应秒（主线程 lv_timer 内调用） */
    using PreviewCb = std::function<void(const lv_image_dsc_t* dsc, double sec)>;

    static VideoPlayer& instance();

    /** 打开文件并开始播放（默认自动播放）；失败返回 false。主线程调用。 */
    bool open(const std::string& path, lv_obj_t* image_widget);
    /** 停止解码线程、释放资源、删 lv_timer。主线程调用。幂等。 */
    void close();

    void play();
    void pause();
    void toggle();
    [[nodiscard]] bool is_playing() const noexcept { return playing_.load(); }
    [[nodiscard]] bool is_open() const noexcept { return open_.load(); }

    /** 跳转到绝对秒（用于进度条点击/拖动）。 */
    void seek(double sec);
    [[nodiscard]] double duration() const noexcept { return duration_.load(); }
    [[nodiscard]] double position() const noexcept { return position_.load(); }

    void set_progress_cb(ProgressCb cb) { progress_cb_ = std::move(cb); }
    void set_ended_cb(EndedCb cb) { ended_cb_ = std::move(cb); }
    void set_preview_cb(PreviewCb cb) { preview_cb_ = std::move(cb); }

    /** 请求解码 sec 处的预览小图（拖动进度条时调用，独立线程处理，不扰动播放）。 */
    void request_preview(double sec);

private:
    static constexpr int kFrameSlots = 3;
    static constexpr int kMaxEdge    = 720; /**< 解码目标长边上限（省内存/CPU） */
    static constexpr int kPreviewEdge = 320; /**< 拖动预览小图长边上限 */
    static constexpr uint32_t kUiPeriodMs = 33u;

    VideoPlayer() = default;

    void decode_thread_fn();
    void preview_thread_fn();
    static void ui_timer_cb(lv_timer_t* timer);
    bool alloc_buffers(int w, int h, int row_stride_bytes);
    void free_buffers();
    void reset_state();

    lv_obj_t*   image_    = nullptr;
    lv_timer_t* ui_timer_ = nullptr;

    std::thread       decode_thread_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> ended_{false};
    std::atomic<bool> ended_notified_{false}; /**< ended 回调仅触发一次 */

    /* 播放位置/时长（秒），由解码线程写、主线程读 */
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};

    /* seek 请求：>=0 表示待处理的目标秒 */
    std::atomic<double> seek_target_{-1.0};

    /* 暂停/恢复/seek 唤醒 */
    std::mutex              cv_mtx_;
    std::condition_variable cv_;

    /* 三缓冲 BGRA */
    int              dst_w_ = 0;
    int              dst_h_ = 0;
    std::vector<uint8_t> slot_buf_[kFrameSlots];
    lv_image_dsc_t   img_dsc_[kFrameSlots]{};
    std::atomic<int> display_buf_{-1};
    std::atomic<int> shown_buf_{-1};

    std::string path_;

    /* 拖动预览：独立解码线程 + 双缓冲小图 */
    std::thread             preview_thread_;
    std::atomic<bool>       preview_stop_{false};
    std::atomic<double>     preview_req_{-1.0};
    std::mutex              pv_mtx_;
    std::condition_variable pv_cv_;
    std::vector<uint8_t>    pv_buf_[2];
    lv_image_dsc_t          pv_dsc_[2]{};
    std::atomic<int>        pv_ready_idx_{-1};
    std::atomic<double>     pv_ready_sec_{0.0};

    ProgressCb progress_cb_;
    EndedCb    ended_cb_;
    PreviewCb  preview_cb_;
};

} // namespace ui

#endif /* VIDEO_PLAYER_HPP */
