#ifndef PRINT_VIDEO_RECORDER_HPP
#define PRINT_VIDEO_RECORDER_HPP

#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
 * Finish/Abort 时 EOS flush → ffmpeg remux 为 .mp4 + 抽最后一帧 .jpg，再删临时 .h264。
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
     * 设置 dma fd 查询回调。根据元数据环中的 buffer_idx 拿到 fd。
     */
    void set_dma_fd_lookup(std::function<int(uint32_t buffer_idx)> fn);

private:
    void worker_fn();
    void enqueue_job(PrintVideoJob job);
    bool open_consumer();
    void close_consumer();
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
    /** ffmpeg：从 MP4 抽最后一帧为 JPEG 缩略图 */
    bool extract_last_frame_thumb(const char* mp4_path, const char* jpg_path) const;
    bool remove_session_dir();
    static bool mkdir_p(const char* path);
    static std::string sanitize_filename(const char* name);
    static int run_shell_cmd(const char* cmd, const char* log_tag);

    std::mutex              mtx_;
    std::condition_variable cv_;
    std::queue<PrintVideoJob> jobs_;
    std::thread             worker_;
    std::atomic<bool>       stop_{false};

    std::function<int(uint32_t)> fd_lookup_;

    upc_ipc::IpcDmaFrameRingConsumer consumer_;
    bool consumer_open_ = false;

    bool     session_active_ = false;
    uint32_t stream_width_   = CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_  = CAMERA_HEIGHT_DEFAULT;
    uint32_t layer_fps_      = CAMERA_PRINT_VIDEO_LAYER_FPS;
    std::string session_id_;
    std::string job_name_;
    std::string session_dir_;
    std::string h264_path_;

    void*    mpp_ctx_      = nullptr;
    void*    mpp_api_      = nullptr;
    void*    mpp_buf_grp_  = nullptr;
    int64_t  mpp_pts_      = 0;
    bool     encoder_open_ = false;
    bool     header_written_ = false;
};

} // namespace upc::camera

#endif /* PRINT_VIDEO_RECORDER_HPP */
