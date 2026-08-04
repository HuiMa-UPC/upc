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
 * 打印层拍照：dma 元数据环取帧 → 软件编码 PNG（1.png,2.png,...）；
 * Finish/Abort 时仅生成 thumb.rthm + manifest 快路径（video_pending），
 * 结果页退出后再统一按 PNG 合成最终 timelapse.mp4。
 */
class PrintVideoRecorder {
public:
    PrintVideoRecorder() = default;
    ~PrintVideoRecorder();

    PrintVideoRecorder(const PrintVideoRecorder&) = delete;
    PrintVideoRecorder& operator=(const PrintVideoRecorder&) = delete;

    void set_stream_geometry(uint32_t width, uint32_t height);
    /** 合成 MP4 帧率；0 忽略。也可由 SessionStart.layer_fps 覆盖。 */
    void set_timelapse_fps(uint32_t fps);

    void on_session_start(const middleware::PrintVideoSessionPayload& info);
    void on_layer_snap(const middleware::PrintVideoLayerSnapPayload& snap);
    void on_session_finish(const middleware::PrintVideoSessionPayload& info);
    void on_session_abort(const middleware::PrintVideoSessionPayload& info);
    /** 结果页退出后先把条目标记为可见。 */
    bool commit_after_exit(const char* frames_dir);
    /** 结果页退出后触发：按目录内 PNG 统一合成/更新最终 MP4。 */
    void finalize_after_exit(const char* frames_dir);
    void set_sender_module_id(middleware::ModuleId module_id);

    /**
     * 设置 NV12 快照回调。回调返回紧凑 NV12（stride==width）帧数据。
     */
    void set_nv12_snapshot_provider(std::function<bool(std::vector<uint8_t>&)> fn);

private:
    void worker_fn();
    void enqueue_job(PrintVideoJob job);
    bool grab_latest_nv12(std::vector<uint8_t>& out);
    /** NV12 → PNG（ffmpeg 软编）→ session_dir_/N.png（N 从 1 连续） */
    bool encode_nv12_to_png(const uint8_t* nv12, size_t bytes, uint32_t frame_index);
    bool handle_layer_snap(const PrintVideoJob& job);
    void finalize_session(bool aborted);
    /** 委托 timelapse_util：dma32 + importbuffer_fd 生成 RTHM 缩略图 */
    bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                    const char* out_path) const;
    /** 完成目录落盘后 upsert /home/video/manifest.json；true=已原子替换成功 */
    bool upsert_print_history(bool aborted,
                              const std::string& frames_dir_name,
                              const std::string& thumb_name,
                              const std::string& video_name,
                              bool video_pending) const;
    /**
     * APP_Print_Video_Manifest_Ntf → UI：结果数据已就绪，可切打印结果页。
     * 必须在 manifest rename() 之后调用；0 帧（无 manifest 条目）也要发，
     * 否则 UI 永远停在打印中界面。worker 线程调用，IPC 发帧内部有锁。
     */
    void notify_manifest_ready(const std::string& frames_dir_name,
                               bool has_entry, bool aborted, bool has_video) const;
    bool promote_session_dir();
    bool remove_session_dir();
    std::string frame_png_path(uint32_t frame_index) const;
    static bool mkdir_p(const char* path);
    static std::string sanitize_filename(const char* name);
    static std::string sanitize_session_token(const char* token);
    static int run_shell_cmd(const char* cmd, const char* log_tag);

    std::mutex              mtx_;
    std::condition_variable cv_;
    std::queue<PrintVideoJob> jobs_;
    std::thread             worker_;
    std::atomic<bool>       stop_{false};

    std::function<bool(std::vector<uint8_t>&)> snapshot_provider_;

    bool     session_active_ = false;
    uint32_t stream_width_   = CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_  = CAMERA_HEIGHT_DEFAULT;
    uint32_t layer_fps_      = CAMERA_PRINT_VIDEO_LAYER_FPS;
    uint32_t frame_count_    = 0; /**< 成功写入的 PNG 帧数（连续 1..N） */
    std::vector<uint8_t> last_layer_nv12_; /**< 最近一次层拍成功帧（用于生成 thumb） */
    std::string session_id_;
    std::string job_name_;
    std::string session_name_;
    std::string session_final_dir_;
    std::string session_dir_;
    int64_t     start_unix_ = 0; /**< Session_Start 墙钟秒，写入 manifest */
    middleware::ModuleId sender_module_id_ = middleware::ModuleId::APP_MEDIA;
};

} // namespace upc::media

#endif /* PRINT_VIDEO_RECORDER_HPP */
