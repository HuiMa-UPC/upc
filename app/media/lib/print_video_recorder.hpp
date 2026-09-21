#ifndef PRINT_VIDEO_RECORDER_HPP
#define PRINT_VIDEO_RECORDER_HPP

#include "device_config.hpp"
#include "msg_runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace upc::media {

enum class PrintVideoJobKind : uint8_t {
    LayerSnap = 1,
    FinishSave = 2,
    AbortCleanup = 3,
};

struct PrintVideoJob {
    PrintVideoJobKind kind = PrintVideoJobKind::LayerSnap;
    uint32_t          layer_index = 0;
    uint16_t          layer_height_um = 0;
};

/**
 * 打印层拍照：NV12 → CPU JPEG（1.jpg,2.jpg,...）；
 * Finish/Abort 时仅生成 thumb.rthm + manifest 快路径（video_pending /
 * print_video_pending）。开门 finalize：
 *   - `UPC_TIMELAPSE_PLAYBACK_PNG=1`（默认）：不编 timelapse.mp4，保留 JPEG；
 *   - `=0`：JPEG→timelapse.mp4，成功后可删清粉段 JPEG。
 * 若 Session_Start 开了连续 H264，直接使用边录边封装的 print_h264.mp4
 * （与延时开关无关；封装帧率用相机流 fps）。
 */
class PrintVideoRecorder {
public:
    PrintVideoRecorder() = default;
    ~PrintVideoRecorder();

    PrintVideoRecorder(const PrintVideoRecorder&) = delete;
    PrintVideoRecorder& operator=(const PrintVideoRecorder&) = delete;

    /** 预览几何 + 相机流帧率（打印录像 remux 用；勿与 layer_fps 混淆）。 */
    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps = 0);
    /** 延时摄影成片帧率；0 忽略。也可由 SessionStart.layer_fps 覆盖。 */
    void set_timelapse_fps(uint32_t fps);

    void on_session_start(const middleware::PrintVideoSessionPayload& info);
    void on_layer_snap(const middleware::PrintVideoLayerSnapPayload& snap);
    void on_session_finish(const middleware::PrintVideoSessionPayload& info);
    void on_session_abort(const middleware::PrintVideoSessionPayload& info);
    /** 丢弃当前会话的临时媒体；用于存储预算无法满足时，打印本身继续执行。 */
    void discard_current_session();
    /** 结果页退出后先把条目标记为可见。 */
    bool commit_after_exit(const char* frames_dir);
    /**
     * 结果页退出后触发：按目录内 JPEG 统一合成/更新最终 MP4。
     * finalize_print_video=false 时仅完成 Timelapse，保留 print_video_pending
     * 给 H264 finalize done 后的二次收尾。
     */
    void finalize_after_exit(const char* frames_dir, bool finalize_print_video = true);
    /** 磁盘紧张时跳过本会话层间拍照（仍允许会话结束发 manifest）。 */
    void set_layer_snap_enabled(bool enabled);
    void set_sender_module_id(middleware::ModuleId module_id);
    /** 关键写盘前预算检查；返回 false 时本次写盘跳过。 */
    void set_storage_budget_guard(std::function<bool()> fn);
    /**
     * 连续 H264 因空间耗尽被提前停止后，取消本次会话对 print_h264.mp4 的期待。
     * 这样 finalize 阶段不会把 print_video_pending 永久置为 1。
     */
    void disable_h264_expectation();
    /** 当前会话目录（空=无活跃会话）。 */
    std::string current_session_dir() const;
    bool session_active() const;

    /**
     * 设置 NV12 快照回调。回调返回紧凑 NV12（stride==width）帧数据。
     */
    void set_nv12_snapshot_provider(std::function<bool(std::vector<uint8_t>&)> fn);
    /** 连续打印录像的最后帧缩略图提供方；与延时摄影封面保持独立。 */
    void set_print_thumb_provider(std::function<bool(const char*)> fn);

private:
    void worker_fn();
    void enqueue_job(PrintVideoJob job);
    bool grab_latest_nv12(std::vector<uint8_t>& out);
    /** NV12 → JPEG（libturbojpeg）→ session_dir_/N.jpg（N 从 1 连续） */
    bool encode_nv12_to_jpeg(const uint8_t* nv12, size_t bytes, uint32_t frame_index);
    bool handle_layer_snap(const PrintVideoJob& job);
    void finalize_session(bool aborted);
    /** 委托 timelapse_util：dma32 + importbuffer_fd 生成 RTHM 缩略图 */
    bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                    const char* out_path) const;
    /** 完成目录落盘后 upsert /userdata/media/video/config.json；true=已原子替换成功 */
    bool upsert_print_history(bool aborted,
                              const std::string& frames_dir_name,
                              const std::string& thumb_name,
                              const std::string& video_name,
                              bool video_pending,
                              bool print_video_pending) const;
    /**
     * APP_Print_Video_Manifest_Ntf → UI：结果数据已就绪，可切打印结果页。
     * 必须在 manifest rename() 之后调用；0 帧（无 manifest 条目）也要发，
     * 否则 UI 永远停在打印中界面。worker 线程调用，IPC 发帧内部有锁。
     */
    void notify_manifest_ready(const std::string& frames_dir_name,
                               bool has_entry, bool aborted, bool has_video) const;
    /** 生成“作业名 + 本地开始时间毫秒”的唯一正式目录名。 */
    static std::string make_session_name(const std::string& job_name,
                                         int64_t start_unix = 0);
    bool remove_session_dir();
    std::string frame_jpeg_path(uint32_t frame_index) const;
    static bool mkdir_p(const char* path);
    static std::string sanitize_filename(const char* name);
    static int run_shell_cmd(const char* cmd, const char* log_tag);

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::queue<PrintVideoJob> jobs_;
    std::thread             worker_;
    std::atomic<bool>       stop_{false};

    std::function<bool(std::vector<uint8_t>&)> snapshot_provider_;
    std::function<bool(const char*)> print_thumb_provider_;
    std::function<bool()> storage_budget_guard_;

    bool     session_active_ = false;
    bool     layer_snap_enabled_ = true; /**< false=磁盘水位告警，跳过层拍 */
    bool     h264_continuous_on_ = false; /**< Session_Start 时的打印期 H264 开关快照 */
    uint32_t stream_width_   = CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_  = CAMERA_HEIGHT_DEFAULT;
    uint32_t stream_fps_     = CAMERA_FPS_DEFAULT; /**< 与 PrintH264 编码帧率一致 */
    uint32_t layer_fps_      = CAMERA_PRINT_VIDEO_LAYER_FPS;
    uint32_t frame_count_    = 0; /**< 成功写入的 JPEG 帧数（连续 1..N.jpg） */
    std::vector<uint8_t> last_layer_nv12_; /**< 最近一次层拍成功帧（用于生成 thumb） */
    std::string session_id_;
    std::string job_name_;
    std::string session_name_;
    std::string session_dir_;
    bool        discarded_ = false;
    int64_t     start_unix_ = 0; /**< Session_Start 墙钟秒，写入 manifest */
    middleware::ModuleId sender_module_id_ = middleware::ModuleId::APP_MEDIA;
};

} // namespace upc::media

#endif /* PRINT_VIDEO_RECORDER_HPP */
