#ifndef PRINT_H264_RECORDER_HPP
#define PRINT_H264_RECORDER_HPP

#include "device_config.hpp"
#include "msg_runtime.hpp"

#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace upc::camera {

/**
 * 打印期连续录像：
 * - 会话开启时持续接收预览 NV12 每帧，顺序写入 raw 文件
 * - 会话结束（包含终止）时一次性转码为 print.h264（同名覆盖）
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
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);

private:
    void finalize_locked(std::unique_lock<std::mutex>& lock, bool aborted);
    static std::string sanitize_filename(const char* name);
    static int run_shell_cmd(const char* cmd, const char* log_tag);

    std::mutex mtx_;
    std::condition_variable finalize_cv_;

    bool active_ = false;
    bool had_write_error_ = false;
    bool finalize_in_progress_ = false;
    uint32_t width_ = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_ = CAMERA_FPS_DEFAULT;
    uint32_t frame_count_ = 0;

    std::string session_id_;
    std::string session_name_;
    std::string session_dir_;
    std::string raw_path_;
    std::string h264_path_;
    FILE* raw_fp_ = nullptr;
    std::vector<uint8_t> frame_buf_;
};

} // namespace upc::camera

#endif /* PRINT_H264_RECORDER_HPP */
