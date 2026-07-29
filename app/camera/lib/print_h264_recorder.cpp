#include "print_h264_recorder.hpp"

#include "rs_log.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace upc::camera {

namespace {

constexpr const char* kH264Name = "print.h264";
constexpr const char* kRawName = "print_raw.nv12";

} // namespace

PrintH264Recorder::~PrintH264Recorder()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(raw_fp_) {
        std::fclose(raw_fp_);
        raw_fp_ = nullptr;
    }
}

void PrintH264Recorder::set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(width > 0) {
        width_ = width;
    }
    if(height > 0) {
        height_ = height;
    }
    if(fps > 0) {
        fps_ = fps;
    }
}

void PrintH264Recorder::on_session_start(const middleware::PrintVideoSessionPayload& info)
{
    std::unique_lock<std::mutex> lock(mtx_);
    while(finalize_in_progress_) {
        finalize_cv_.wait(lock);
    }
    if(raw_fp_) {
        std::fclose(raw_fp_);
        raw_fp_ = nullptr;
    }
    active_ = false;
    had_write_error_ = false;
    frame_count_ = 0;
    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    raw_path_.clear();
    h264_path_.clear();
    frame_buf_.clear();

    if(info.h264_continuous_on == 0) {
        return;
    }

    session_id_ = info.session_id;
    session_name_ = sanitize_filename(info.job_name);
    if(session_name_.empty()) {
        session_name_ = "print";
    }
    session_dir_ = std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + session_name_;
    raw_path_ = session_dir_ + "/" + kRawName;
    h264_path_ = session_dir_ + "/" + kH264Name;

    if(info.width > 0) {
        width_ = info.width;
    }
    if(info.height > 0) {
        height_ = info.height;
    }
    if(info.layer_fps > 0) {
        fps_ = info.layer_fps;
    }

    std::error_code ec;
    std::filesystem::create_directories(session_dir_, ec);
    if(ec) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_h264 mkdir fail dir=%s err=%d",
                               session_dir_.c_str(), ec.value());
        return;
    }
    (void)std::remove(raw_path_.c_str());
    (void)std::remove(h264_path_.c_str());
    raw_fp_ = std::fopen(raw_path_.c_str(), "wb");
    if(!raw_fp_) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_h264 open raw fail %s: %s",
                               raw_path_.c_str(), std::strerror(errno));
        return;
    }
    active_ = true;
    UPC_LOG_CAMERA_SERVICE(Info,
        "print_h264 start id=%s dir=%s %ux%u@%u",
        session_id_.c_str(), session_dir_.c_str(), width_, height_, fps_);
}

void PrintH264Recorder::on_session_finish(const middleware::PrintVideoSessionPayload& info)
{
    std::unique_lock<std::mutex> lock(mtx_);
    if(!active_) {
        return;
    }
    if(info.session_id[0] != '\0' && session_id_ != info.session_id) {
        return;
    }
    finalize_locked(lock, false);
}

void PrintH264Recorder::on_session_abort(const middleware::PrintVideoSessionPayload& info)
{
    std::unique_lock<std::mutex> lock(mtx_);
    if(!active_) {
        return;
    }
    if(info.session_id[0] != '\0' && session_id_ != info.session_id) {
        return;
    }
    finalize_locked(lock, true);
}

void PrintH264Recorder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(!active_ || !raw_fp_ || had_write_error_ || !src_y || !src_uv || width_ == 0 || height_ == 0) {
        return;
    }
    if(stride_y == 0) {
        stride_y = width_;
    }
    const size_t need = static_cast<size_t>(width_) * height_ * 3u / 2u;
    if(frame_buf_.size() != need) {
        frame_buf_.resize(need);
    }
    uint8_t* dst = frame_buf_.data();
    uint8_t* dst_uv = dst + static_cast<size_t>(width_) * height_;
    for(uint32_t row = 0; row < height_; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * width_,
                    src_y + static_cast<size_t>(row) * stride_y, width_);
    }
    for(uint32_t row = 0; row < height_ / 2u; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * width_,
                    src_uv + static_cast<size_t>(row) * stride_y, width_);
    }
    if(std::fwrite(frame_buf_.data(), 1, need, raw_fp_) != need) {
        had_write_error_ = true;
        UPC_LOG_CAMERA_SERVICE(Error, "print_h264 write raw fail path=%s",
                               raw_path_.c_str());
        return;
    }
    ++frame_count_;
}

void PrintH264Recorder::finalize_locked(std::unique_lock<std::mutex>& lock, bool aborted)
{
    struct FinalizeCtx {
        bool aborted = false;
        bool had_write_error = false;
        uint32_t frame_count = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        std::string session_id;
        std::string raw_path;
        std::string h264_path;
    } ctx;

    finalize_in_progress_ = true;
    active_ = false;
    if(raw_fp_) {
        std::fflush(raw_fp_);
        std::fclose(raw_fp_);
        raw_fp_ = nullptr;
    }
    ctx.aborted = aborted;
    ctx.had_write_error = had_write_error_;
    ctx.frame_count = frame_count_;
    ctx.width = width_;
    ctx.height = height_;
    ctx.fps = fps_;
    ctx.session_id = session_id_;
    ctx.raw_path = raw_path_;
    ctx.h264_path = h264_path_;
    /* 会话状态先复位，缩短锁持有时间；重编码放到锁外，避免阻塞取帧路径。 */
    had_write_error_ = false;
    frame_count_ = 0;
    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    raw_path_.clear();
    h264_path_.clear();
    frame_buf_.clear();
    lock.unlock();

    if(ctx.had_write_error || ctx.frame_count == 0) {
        (void)std::remove(ctx.raw_path.c_str());
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_h264 skip encode session=%s aborted=%d frames=%u write_err=%d",
            ctx.session_id.c_str(), ctx.aborted ? 1 : 0,
            ctx.frame_count, ctx.had_write_error ? 1 : 0);
    } else {
        const uint32_t fps = ctx.fps > 0 ? ctx.fps : 1u;
        char cmd[2048];
        std::snprintf(cmd, sizeof(cmd),
                      "%s -i '%s' -o '%s' -w %u -h %u -hstride %u "
                      "-f 0 -t 7 -n %u -fps %u -bps %u -rc 1",
                      CAMERA_MPI_ENC_BIN,
                      ctx.raw_path.c_str(),
                      ctx.h264_path.c_str(),
                      ctx.width, ctx.height, ctx.width,
                      ctx.frame_count, fps, CAMERA_PRINT_H264_BITRATE);
        const bool ok = run_shell_cmd(cmd, "mpi_enc h264") == 0;
        if(ok) {
            (void)std::remove(ctx.raw_path.c_str());
        }
        UPC_LOG_CAMERA_SERVICE(Info,
            "print_h264 %s session=%s aborted=%d frames=%u out=%s",
            ok ? "done" : "failed",
            ctx.session_id.c_str(),
            ctx.aborted ? 1 : 0,
            ctx.frame_count,
            ctx.h264_path.c_str());
    }

    lock.lock();
    finalize_in_progress_ = false;
    finalize_cv_.notify_all();
}

std::string PrintH264Recorder::sanitize_filename(const char* name)
{
    if(!name || !name[0]) {
        return "print";
    }
    std::string out(name);
    for(char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if((uc >= '0' && uc <= '9') ||
           (uc >= 'A' && uc <= 'Z') ||
           (uc >= 'a' && uc <= 'z') ||
           c == '_' || c == '-' || c == '.') {
            continue;
        }
        c = '_';
    }
    if(out.empty()) {
        out = "print";
    }
    return out;
}

int PrintH264Recorder::run_shell_cmd(const char* cmd, const char* log_tag)
{
    if(!cmd || !cmd[0]) {
        return -1;
    }
    UPC_LOG_CAMERA_SERVICE(Info, "print_h264 %s: %s",
                           log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "print_h264 %s failed rc=%d",
            log_tag ? log_tag : "cmd", rc);
    }
    return rc;
}

} // namespace upc::camera
