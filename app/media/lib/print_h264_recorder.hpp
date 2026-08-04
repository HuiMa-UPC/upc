#ifndef PRINT_H264_RECORDER_HPP
#define PRINT_H264_RECORDER_HPP

#include "device_config.hpp"
#include "msg_runtime.hpp"

#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace upc::media {

/**
 * 打印期连续录像：
 * - 媒体消费线程只拷贝 NV12 入队；后台 worker 实时喂给 RKMPP 编码器
 * - 直接输出 print.h264（H264 裸流）
 * - 写盘队列满时丢帧保预览，不阻塞媒体流消费
 * - 输出目录：/home/media/video/tmp/print.h264
 */
class PrintH264Recorder {
public:
    PrintH264Recorder() = default;
    ~PrintH264Recorder();

    PrintH264Recorder(const PrintH264Recorder&) = delete;
    PrintH264Recorder& operator=(const PrintH264Recorder&) = delete;

    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);
    void on_session_start(const middleware::PrintVideoSessionPayload& info);
    void on_session_finish(const middleware::PrintVideoSessionPayload& info);
    void on_session_abort(const middleware::PrintVideoSessionPayload& info);
    void set_paused(bool paused);
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);
    /** 清理固定临时目录内残留的 print.h264 / print_h264.mp4 */
    void enqueue_cleanup_tmp();

private:
    enum class JobKind : uint8_t {
        WriteFrame = 0,
        Finalize   = 1,
        Cleanup    = 2,
    };

    struct Job {
        JobKind kind = JobKind::WriteFrame;
        uint64_t session_seq = 0;
        /* WriteFrame */
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> nv12;
        /* Finalize（frame_count / write_err 在 worker 弹出时再读共享计数） */
        bool aborted = false;
        std::string session_id;
        /* Cleanup */
        std::string cleanup_dir;
    };

    void enqueue_finalize_locked(bool aborted);
    void clear_session_paths_locked();
    void reset_session_state_locked();
    void start_worker_if_needed_locked();
    void stop_worker();
    void worker_fn();
    bool open_encoder_locked();
    int close_encoder_locked();
    void run_write_frame_job(const Job& job);
    void run_finalize_job(Job job);
    void run_cleanup_job(const Job& job);
    static std::string sanitize_filename(const char* name);
    static size_t count_pending_write_frames(const std::deque<Job>& jobs);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable finalize_cv_;
    std::condition_variable idle_cv_;
    std::deque<Job> jobs_;
    std::thread worker_;
    bool worker_stop_ = false;
    bool worker_busy_ = false;
    uint64_t worker_busy_session_ = 0;

    bool active_ = false;
    bool paused_ = false;
    bool had_write_error_ = false;
    bool finalize_in_progress_ = false;
    uint64_t session_seq_ = 0;
    uint32_t width_ = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_ = CAMERA_FPS_DEFAULT;
    uint32_t bitrate_bps_ = CAMERA_PRINT_H264_BITRATE;
    uint32_t frame_count_ = 0;
    uint32_t dropped_frames_ = 0;

    std::string session_id_;
    std::string session_name_;
    std::string session_dir_;
    std::string h264_path_;
    FILE* h264_fp_ = nullptr;

    /* RKMPP 对象（避免在头文件引入 RK 头） */
    void* mpp_ctx_ = nullptr;
    void* mpp_mpi_ = nullptr;
    void* mpp_cfg_ = nullptr;
    bool mpp_ready_ = false;
};

} // namespace upc::media

#endif /* PRINT_H264_RECORDER_HPP */
