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

namespace upc::media {

/**
 * 清粉延时摄影（与打印层拍对齐）：
 * - Start：接续打印目录已有 1..N.jpg，按 timelapse fps 定时抽帧写 (N+1).jpg...
 *          若打印层拍 JPEG 不存在（N=0），清粉段不参与录制/合成。
 * - Stop 成功：记录清粉帧范围并更新末帧 thumb.rthm；MP4 统一在结果页退出后合成
 * - Stop 失败/用户关闭：删除清粉 JPEG，仅保留打印段 JPEG
 */
class CleanVideoRecorder {
public:
    CleanVideoRecorder() = default;
    ~CleanVideoRecorder();

    CleanVideoRecorder(const CleanVideoRecorder&) = delete;
    CleanVideoRecorder& operator=(const CleanVideoRecorder&) = delete;

    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);
    void set_timelapse_fps(uint32_t fps);
    void on_session_start(const middleware::CleanVideoSessionStartPayload& req);
    void on_session_stop(const middleware::CleanVideoSessionStopPayload& req);
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);

private:
    struct CleanFrameJob {
        uint64_t session_seq = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string session_dir;
        std::vector<uint8_t> nv12;
    };

    void stop_locked(std::unique_lock<std::mutex>& lock, uint8_t result_code);
    void publish_clean_test_done(uint8_t result_code, bool has_mp4,
                                 const std::string& mp4_rel_path) const;
    void start_worker_if_needed_locked();
    void stop_worker();
    void worker_fn();
    void wait_session_jobs_drain_locked(std::unique_lock<std::mutex>& lock,
                                        uint64_t session_seq);
    static uint64_t monotonic_us();

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
    uint32_t stream_fps_ = CAMERA_FPS_DEFAULT;
    uint32_t timelapse_fps_ = CAMERA_PRINT_VIDEO_LAYER_FPS;
    uint32_t dropped_frames_ = 0;
    uint32_t input_frame_count_ = 0;
    uint64_t session_seq_ = 0;
    uint64_t next_sample_us_ = 0;
    uint64_t sample_interval_us_ = 200000; /**< 默认 5fps */

    std::string session_id_;
    std::string job_name_;
    std::string session_dir_;
    uint32_t print_frame_count_ = 0; /**< 启动时已有打印 JPEG 数 */
    uint32_t clean_frame_count_ = 0; /**< 本次清粉成功写入的 JPEG 数 */
    std::vector<uint8_t> last_clean_nv12_;
};

} // namespace upc::media

#endif /* CLEAN_VIDEO_RECORDER_HPP */
