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
    FinishMux = 2,
    AbortCleanup = 3,
};

struct PrintVideoJob {
    PrintVideoJobKind kind = PrintVideoJobKind::LayerSnap;
    uint32_t          layer_index = 0;
    uint16_t          layer_height_um = 0;
};

/**
 * 打印层拍照：dma 元数据环取帧 → MPP H.264；
 * Finish/Abort 时 EOS flush → ffmpeg remux 为 .mp4 + 生成最后一帧缩略图（优先 RTHM，失败回退 JPG），再删临时 .h264，
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
    bool ensure_encoder();
    void close_encoder();
    bool drain_encoder_packets(FILE* fp);
    bool flush_encoder_eos();
    bool encode_nv12_frame(const uint8_t* nv12, size_t bytes, uint32_t layer_index);
    bool grab_latest_nv12(std::vector<uint8_t>& out);
    bool handle_layer_snap(const PrintVideoJob& job);
    void finalize_session(bool aborted);
    bool append_encoder_output_to_h264();
    /** ffmpeg：临时 Annex-B H.264 remux 为 MP4（-c copy） */
    bool remux_h264_to_mp4(const char* h264_path, const char* mp4_path) const;
    /** RGA：把最后一帧 NV12 生成 RTHM（RGB565）缩略图 */
    bool create_raw_thumb_from_nv12(const std::vector<uint8_t>& nv12,
                                    const char* out_path) const;
    /** ffmpeg：从 MP4 抽最后一帧为 JPEG（raw 失败时兜底） */
    bool extract_last_frame_thumb(const char* mp4_path, const char* jpg_path) const;
    /** remux 成功后 upsert /home/video/manifest.json */
    void upsert_print_history(bool aborted,
                              const std::string& video_name,
                              const std::string& thumb_name) const;
    bool remove_session_dir();
    static bool mkdir_p(const char* path);
    static std::string sanitize_filename(const char* name);
    /** 本地墙钟 → YYYYMMDD_HHMMSS */
    static std::string format_stamp_local(int64_t unix_sec);
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
    std::string session_id_;
    std::string job_name_;
    std::string session_dir_;
    std::string h264_path_;
    int64_t     start_unix_ = 0; /**< Session_Start 墙钟秒，写入 manifest */

    void*    mpp_ctx_      = nullptr;
    void*    mpp_api_      = nullptr;
    void*    mpp_buf_grp_  = nullptr;
    int64_t  mpp_pts_      = 0;
    bool     encoder_open_ = false;
    bool     header_written_ = false;
};

} // namespace upc::camera

#endif /* PRINT_VIDEO_RECORDER_HPP */
