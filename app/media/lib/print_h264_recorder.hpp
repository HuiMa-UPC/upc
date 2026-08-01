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
 * - 媒体消费线程只拷贝 NV12 入队；写 raw / mpi_enc 均在后台 worker（对齐清粉录像）
 * - 写盘队列满时丢帧保预览，不阻塞媒体流消费
 * - 会话结束（含终止）只入队 Encode；worker 先排空其前的写帧再转码 print.h264
 * - 输出目录：/home/video/{job_name}/print.h264
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
    void enqueue_cleanup_for_session(const char* job_name, const char* session_id);

private:
    enum class JobKind : uint8_t {
        WriteFrame = 0,
        Encode     = 1,
        Cleanup    = 2,
    };

    struct Job {
        JobKind kind = JobKind::WriteFrame;
        uint64_t session_seq = 0;
        /* WriteFrame */
        uint32_t width = 0;
        uint32_t height = 0;
        std::string raw_path;
        std::vector<uint8_t> nv12;
        /* Encode（frame_count / write_err 在 worker 弹出时再读共享计数） */
        bool aborted = false;
        uint32_t fps = 0;
        std::string session_id;
        std::string h264_path;
        /* Cleanup */
        std::string cleanup_dir;
    };

    void enqueue_finalize_locked(bool aborted);
    void start_worker_if_needed_locked();
    void stop_worker();
    void worker_fn();
    void close_raw_fp_locked();
    void run_write_frame_job(const Job& job);
    void run_encode_job(Job job);
    void run_cleanup_job(const Job& job);
    static std::string sanitize_filename(const char* name);
    static int run_shell_cmd(const char* cmd, const char* log_tag);
    static size_t count_pending_write_frames(const std::deque<Job>& jobs);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable finalize_cv_;
    std::deque<Job> jobs_;
    std::thread worker_;
    bool worker_stop_ = false;

    bool active_ = false;
    bool paused_ = false;
    bool had_write_error_ = false;
    bool finalize_in_progress_ = false;
    uint64_t session_seq_ = 0;
    uint32_t width_ = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_ = CAMERA_FPS_DEFAULT;
    uint32_t frame_count_ = 0;
    uint32_t dropped_frames_ = 0;

    std::string session_id_;
    std::string session_name_;
    std::string session_dir_;
    std::string raw_path_;
    std::string h264_path_;
    /** 仅 worker 线程使用；开关时持 mtx_ */
    FILE* raw_fp_ = nullptr;
    std::string raw_fp_path_;
};

} // namespace upc::media

#endif /* PRINT_H264_RECORDER_HPP */
