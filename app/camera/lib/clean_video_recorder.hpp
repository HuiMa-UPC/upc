#ifndef CLEAN_VIDEO_RECORDER_HPP
#define CLEAN_VIDEO_RECORDER_HPP

#include "device_config.hpp"
#include "msg_runtime.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace upc::camera {

/**
 * 清粉连续录像：
 * - Start：按预览流每帧保存 NV12 原始帧流（目标 30fps）
 * - Stop：先将 raw NV12 编成 clean_timelapse.h264，再封装 clean_timelapse.mp4
 * - MP4 成功后默认删除中间文件；失败时保留中间文件供排查
 */
class CleanVideoRecorder {
public:
    CleanVideoRecorder() = default;
    ~CleanVideoRecorder();

    CleanVideoRecorder(const CleanVideoRecorder&) = delete;
    CleanVideoRecorder& operator=(const CleanVideoRecorder&) = delete;

    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);
    void on_session_start(const middleware::CleanVideoSessionStartPayload& req);
    void on_session_stop(const middleware::CleanVideoSessionStopPayload& req);
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);

private:
    struct CleanFrameJob {
        uint64_t session_seq = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t input_frame = 0;
        std::string raw_path;
        std::vector<uint8_t> nv12;
    };

    void stop_locked(std::unique_lock<std::mutex>& lock,
                     uint8_t result_code, bool keep_raw);
    static std::string sanitize_name(const char* raw);
    static bool ensure_dir(const std::string& dir);
    static int run_shell_cmd(const char* cmd, const char* tag);
    static bool append_nv12_frame(const CleanFrameJob& job);
    static bool encode_raw_nv12_to_h264(const std::string& raw_path,
                                        const std::string& h264_path,
                                        uint32_t width, uint32_t height,
                                        uint32_t fps, uint32_t frame_count);
    static bool remux_h264_to_mp4(const std::string& h264_path,
                                  const std::string& mp4_path,
                                  uint32_t fps);
    void start_worker_if_needed_locked();
    void stop_worker();
    void worker_fn();
    void wait_session_jobs_drain_locked(std::unique_lock<std::mutex>& lock,
                                        uint64_t session_seq);

    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::deque<CleanFrameJob> jobs_;
    std::thread worker_;
    bool worker_stop_ = false;
    bool worker_busy_ = false;
    uint64_t worker_busy_session_ = 0;
    bool active_ = false;

    uint32_t width_  = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_    = CAMERA_FPS_DEFAULT;
    uint32_t dropped_frames_ = 0;
    uint32_t input_frame_count_ = 0;
    uint64_t session_seq_ = 0;

    std::string session_id_;
    std::string job_name_;
    std::string session_dir_;
    std::string raw_path_;
    std::string h264_path_;
    std::string mp4_path_;
    uint32_t frame_count_ = 0;
};

} // namespace upc::camera

#endif /* CLEAN_VIDEO_RECORDER_HPP */
