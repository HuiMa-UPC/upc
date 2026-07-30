#include "print_h264_recorder.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace upc::media {

namespace {

constexpr const char* kH264Name = "print.h264";
constexpr const char* kRawName = "print_raw.nv12";
/** 待写帧上限：满则丢帧保预览（约 3 × ~1.9MB） */
constexpr size_t kMaxPendingFrameJobs = 3u;

void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y, const uint8_t* src_uv,
                     uint32_t src_stride_y, uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || width == 0 || height == 0 || src_stride_y == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }
    for(uint32_t r = 0; r < height; ++r) {
        std::memcpy(dst + static_cast<size_t>(r) * width,
                    src_y + static_cast<size_t>(r) * src_stride_y, width);
    }
    for(uint32_t r = 0; r < height / 2u; ++r) {
        std::memcpy(dst_uv + static_cast<size_t>(r) * width,
                    src_uv + static_cast<size_t>(r) * src_stride_y, width);
    }
}

} // namespace

size_t PrintH264Recorder::count_pending_write_frames(const std::deque<Job>& jobs)
{
    size_t n = 0;
    for(const auto& job : jobs) {
        if(job.kind == JobKind::WriteFrame) {
            ++n;
        }
    }
    return n;
}

PrintH264Recorder::~PrintH264Recorder()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        active_ = false;
        close_raw_fp_locked();
    }
    stop_worker();
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

void PrintH264Recorder::start_worker_if_needed_locked()
{
    if(worker_.joinable()) {
        return;
    }
    worker_stop_ = false;
    worker_ = std::thread(&PrintH264Recorder::worker_fn, this);
}

void PrintH264Recorder::stop_worker()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        worker_stop_ = true;
    }
    cv_.notify_all();
    if(worker_.joinable()) {
        worker_.join();
    }
}

void PrintH264Recorder::close_raw_fp_locked()
{
    if(raw_fp_) {
        std::fflush(raw_fp_);
        std::fclose(raw_fp_);
        raw_fp_ = nullptr;
    }
    raw_fp_path_.clear();
}

void PrintH264Recorder::on_session_start(const middleware::PrintVideoSessionPayload& info)
{
    std::unique_lock<std::mutex> lock(mtx_);
    /* 上一场后台转码未完成时阻塞开新录，避免同路径 raw/h264 被覆盖 */
    while(finalize_in_progress_) {
        finalize_cv_.wait(lock);
    }
    close_raw_fp_locked();
    active_ = false;
    had_write_error_ = false;
    frame_count_ = 0;
    dropped_frames_ = 0;
    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    raw_path_.clear();
    h264_path_.clear();

    if(info.h264_continuous_on == 0) {
        return;
    }

    start_worker_if_needed_locked();

    ++session_seq_;
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
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mkdir fail dir=%s err=%d",
                               session_dir_.c_str(), ec.value());
        return;
    }
    (void)std::remove(raw_path_.c_str());
    (void)std::remove(h264_path_.c_str());
    active_ = true;
    UPC_LOG_MEDIA_SERVICE(Info,
        "print_h264 start id=%s dir=%s %ux%u@%u (async write)",
        session_id_.c_str(), session_dir_.c_str(), width_, height_, fps_);
}

void PrintH264Recorder::on_session_finish(const middleware::PrintVideoSessionPayload& info)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(!active_) {
        return;
    }
    if(info.session_id[0] != '\0' && session_id_ != info.session_id) {
        return;
    }
    enqueue_finalize_locked(false);
}

void PrintH264Recorder::on_session_abort(const middleware::PrintVideoSessionPayload& info)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(!active_) {
        return;
    }
    if(info.session_id[0] != '\0' && session_id_ != info.session_id) {
        return;
    }
    enqueue_finalize_locked(true);
}

void PrintH264Recorder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv,
                                      uint32_t stride_y)
{
    uint64_t current_session_seq = 0;
    uint32_t current_width = 0;
    uint32_t current_height = 0;
    std::string current_raw_path;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || had_write_error_ || !src_y || !src_uv ||
           width_ == 0 || height_ == 0 || raw_path_.empty()) {
            return;
        }
        if(count_pending_write_frames(jobs_) >= kMaxPendingFrameJobs) {
            ++dropped_frames_;
            if((dropped_frames_ % 30u) == 1u) {
                UPC_LOG_MEDIA_SERVICE(Warn,
                    "print_h264 queue full drop=%u pending=%zu (keep preview)",
                    dropped_frames_, count_pending_write_frames(jobs_));
            }
            return;
        }
        current_session_seq = session_seq_;
        current_width = width_;
        current_height = height_;
        current_raw_path = raw_path_;
    }

    if(stride_y == 0) {
        stride_y = current_width;
    }
    if(stride_y < current_width) {
        return;
    }

    const size_t need =
        static_cast<size_t>(current_width) * current_height * 3u / 2u;
    Job job{};
    job.kind = JobKind::WriteFrame;
    job.session_seq = current_session_seq;
    job.width = current_width;
    job.height = current_height;
    job.raw_path = std::move(current_raw_path);
    job.nv12.resize(need);
    copy_nv12_tight(job.nv12.data(), src_y, src_uv, stride_y,
                    current_width, current_height);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || current_session_seq != session_seq_ || had_write_error_) {
            return;
        }
        if(count_pending_write_frames(jobs_) >= kMaxPendingFrameJobs) {
            ++dropped_frames_;
            return;
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void PrintH264Recorder::enqueue_finalize_locked(bool aborted)
{
    start_worker_if_needed_locked();

    /* 停收新帧；队列中已有 WriteFrame 仍在 Encode 之前，由 worker FIFO 写完再转码 */
    active_ = false;
    finalize_in_progress_ = true;

    Job job{};
    job.kind = JobKind::Encode;
    job.session_seq = session_seq_;
    job.aborted = aborted;
    job.width = width_;
    job.height = height_;
    job.fps = fps_;
    job.session_id = session_id_;
    job.raw_path = raw_path_;
    job.h264_path = h264_path_;

    const uint32_t dropped = dropped_frames_;
    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    /* raw_path_/计数留给 worker 写完残留帧后再清 */

    jobs_.push_back(std::move(job));
    cv_.notify_one();
    UPC_LOG_MEDIA_SERVICE(Info,
        "print_h264 encode queued aborted=%d dropped=%u (async write+encode)",
        aborted ? 1 : 0, dropped);
}

void PrintH264Recorder::worker_fn()
{
    for(;;) {
        Job job{};
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] {
                return worker_stop_ || !jobs_.empty();
            });
            if(worker_stop_ && jobs_.empty()) {
                break;
            }
            if(jobs_.empty()) {
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        if(job.kind == JobKind::WriteFrame) {
            run_write_frame_job(job);
        } else {
            run_encode_job(std::move(job));
        }
    }

    std::lock_guard<std::mutex> lock(mtx_);
    close_raw_fp_locked();
    finalize_in_progress_ = false;
    finalize_cv_.notify_all();
}

void PrintH264Recorder::run_write_frame_job(const Job& job)
{
    if(job.raw_path.empty() || job.nv12.empty() ||
       job.width == 0 || job.height == 0) {
        return;
    }
    const size_t need =
        static_cast<size_t>(job.width) * job.height * 3u / 2u;
    if(job.nv12.size() < need) {
        return;
    }

    std::unique_lock<std::mutex> lock(mtx_);
    /* 新会话已开始则丢弃旧场残留帧 */
    if(job.session_seq != session_seq_) {
        return;
    }
    if(had_write_error_) {
        return;
    }

    if(!raw_fp_ || raw_fp_path_ != job.raw_path) {
        close_raw_fp_locked();
        raw_fp_ = std::fopen(job.raw_path.c_str(), "ab");
        if(!raw_fp_) {
            had_write_error_ = true;
            UPC_LOG_MEDIA_SERVICE(Error, "print_h264 open raw fail %s: %s",
                                   job.raw_path.c_str(), std::strerror(errno));
            return;
        }
        raw_fp_path_ = job.raw_path;
    }

    FILE* fp = raw_fp_;
    lock.unlock();

    if(std::fwrite(job.nv12.data(), 1, need, fp) != need) {
        std::lock_guard<std::mutex> err_lock(mtx_);
        if(job.session_seq == session_seq_) {
            had_write_error_ = true;
        }
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 write raw fail path=%s",
                               job.raw_path.c_str());
        return;
    }

    std::lock_guard<std::mutex> cnt_lock(mtx_);
    if(job.session_seq == session_seq_) {
        ++frame_count_;
    }
}

void PrintH264Recorder::run_encode_job(Job job)
{
    uint32_t frame_count = 0;
    bool had_write_error = false;
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        /* FIFO：此时本场 WriteFrame 均已处理完 */
        if(job.session_seq == session_seq_) {
            frame_count = frame_count_;
            had_write_error = had_write_error_;
            dropped = dropped_frames_;
            frame_count_ = 0;
            had_write_error_ = false;
            dropped_frames_ = 0;
            raw_path_.clear();
            h264_path_.clear();
        }
        close_raw_fp_locked();
    }

    if(had_write_error || frame_count == 0) {
        (void)std::remove(job.raw_path.c_str());
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_h264 skip encode session=%s aborted=%d frames=%u "
            "write_err=%d dropped=%u",
            job.session_id.c_str(), job.aborted ? 1 : 0,
            frame_count, had_write_error ? 1 : 0, dropped);
    } else {
        const uint32_t fps = job.fps > 0 ? job.fps : 1u;
        char cmd[2048];
        std::snprintf(cmd, sizeof(cmd),
                      "%s -i '%s' -o '%s' -w %u -h %u -hstride %u "
                      "-f 0 -t 7 -n %u -fps %u -bps %u -rc 1",
                      CAMERA_MPI_ENC_BIN,
                      job.raw_path.c_str(),
                      job.h264_path.c_str(),
                      job.width, job.height, job.width,
                      frame_count, fps, CAMERA_PRINT_H264_BITRATE);
        const bool ok = run_shell_cmd(cmd, "mpi_enc h264") == 0;
        if(ok) {
            (void)std::remove(job.raw_path.c_str());
        }
        UPC_LOG_MEDIA_SERVICE(Info,
            "print_h264 %s session=%s aborted=%d frames=%u dropped=%u out=%s",
            ok ? "done" : "failed",
            job.session_id.c_str(),
            job.aborted ? 1 : 0,
            frame_count,
            dropped,
            job.h264_path.c_str());
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        const bool more_encode = std::any_of(
            jobs_.begin(), jobs_.end(),
            [](const Job& j) { return j.kind == JobKind::Encode; });
        if(!more_encode) {
            finalize_in_progress_ = false;
            finalize_cv_.notify_all();
        }
    }
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
    UPC_LOG_MEDIA_SERVICE(Info, "print_h264 %s: %s",
                           log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_h264 %s failed rc=%d",
            log_tag ? log_tag : "cmd", rc);
    }
    return rc;
}

} // namespace upc::media
