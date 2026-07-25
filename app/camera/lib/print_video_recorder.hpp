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

namespace upc::camera {

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
 * Finish/Abort 时使用“最后一次层拍成功帧”生成 thumb.rthm，
 * 并 upsert /home/video/manifest.json（含 complete/terminated 状态）。
 */
class PrintVideoRecorder {
public:
    PrintVideoRecorder() = default;
    ~PrintVideoRecorder();

    PrintVideoRecorder(const PrintVideoRecorder&) = delete;
    PrintVideoRecorder& operator=(const PrintVideoRecorder&) = delete;

    void set_stream_geometry(uint32_t width, uint32_t height);

    void on_session_start(const middleware::PrintVideoSessionPayload& info);
    void on_layer_snap(const middleware::PrintVideoLayerSnapPayload& snap);
    void on_session_finish(const middleware::PrintVideoSessionPayload& info);
    void on_session_abort(const middleware::PrintVideoSessionPayload& info);

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
    /** PNG 序列 -> MP4（timelapse.mp4），不足 2 帧不生成 */
    bool encode_png_sequence_to_mp4(const char* out_path) const;
    bool handle_layer_snap(const PrintVideoJob& job);
    void finalize_session(bool aborted);
    /** RGA：把最后一帧 NV12 生成 RTHM（RGB565）缩略图 */
    bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                    const char* out_path) const;
    /** 完成目录落盘后 upsert /home/video/manifest.json */
    void upsert_print_history(bool aborted,
                              const std::string& frames_dir_name,
                              const std::string& thumb_name,
                              const std::string& video_name) const;
    bool remove_session_dir();
    std::string frame_png_path(uint32_t frame_index) const;
    static bool mkdir_p(const char* path);
    static std::string sanitize_filename(const char* name);
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
    std::string session_dir_;
    int64_t     start_unix_ = 0; /**< Session_Start 墙钟秒，写入 manifest */
};

} // namespace upc::camera

#endif /* PRINT_VIDEO_RECORDER_HPP */
