#include "clean_video_recorder.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cstdio>

namespace upc::camera {

namespace {

constexpr const char* kMp4Name = "clean_timelapse.mp4";
constexpr const char* kRawNv12Name = "clean_raw.nv12";
constexpr uint32_t kCleanRawFps = 10u;
constexpr size_t kMaxPendingJobs = 2u;

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

CleanVideoRecorder::~CleanVideoRecorder()
{
    {
        std::unique_lock<std::mutex> lock(mtx_);
        stop_locked(lock, 0xff, true);
    }
    stop_worker();
}

void CleanVideoRecorder::set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps)
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

std::string CleanVideoRecorder::sanitize_name(const char* raw)
{
    if(!raw || !raw[0]) {
        return "print";
    }
    std::string out(raw);
    for(char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    if(out.empty()) {
        out = "print";
    }
    return out;
}

bool CleanVideoRecorder::ensure_dir(const std::string& dir)
{
    if(dir.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

int CleanVideoRecorder::run_shell_cmd(const char* cmd, const char* tag)
{
    if(!cmd || !cmd[0]) {
        return -1;
    }
    UPC_LOG_CAMERA_SERVICE(Info, "clean_video %s: %s", tag ? tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_CAMERA_SERVICE(Warn, "clean_video %s rc=%d", tag ? tag : "cmd", rc);
    }
    return rc;
}

bool CleanVideoRecorder::append_nv12_frame(const CleanFrameJob& job)
{
    if(job.raw_path.empty() || job.nv12.empty() || job.width == 0 || job.height == 0) {
        return false;
    }
    const size_t need = static_cast<size_t>(job.width) * job.height * 3u / 2u;
    if(job.nv12.size() < need) {
        return false;
    }

    FILE* fp = std::fopen(job.raw_path.c_str(), "ab");
    if(!fp) {
        UPC_LOG_CAMERA_SERVICE(Error, "clean_video open raw fail %s: %s",
                               job.raw_path.c_str(), std::strerror(errno));
        return false;
    }
    const size_t written = std::fwrite(job.nv12.data(), 1, need, fp);
    const bool write_ok = (written == need);
    if(std::fclose(fp) != 0 || !write_ok) {
        UPC_LOG_CAMERA_SERVICE(Error, "clean_video write raw fail %s %zu/%zu",
                               job.raw_path.c_str(), written, need);
        return false;
    }
    return true;
}

bool CleanVideoRecorder::encode_raw_nv12_to_mp4(const std::string& raw_path,
                                                const std::string& mp4_path,
                                                uint32_t width, uint32_t height,
                                                uint32_t fps)
{
    if(raw_path.empty() || mp4_path.empty() || width == 0 || height == 0 || fps == 0) {
        return false;
    }
    char cmd[1536];
    std::snprintf(cmd, sizeof(cmd),
                  "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -r %u -i '%s' "
                  "-c:v mpeg4 -q:v 5 -pix_fmt yuv420p -movflags +faststart '%s'",
                  CAMERA_FFMPEG_BIN, width, height, fps, raw_path.c_str(),
                  mp4_path.c_str());
    return run_shell_cmd(cmd, "ffmpeg clean mp4") == 0;
}

void CleanVideoRecorder::start_worker_if_needed_locked()
{
    if(worker_.joinable()) {
        return;
    }
    worker_stop_ = false;
    worker_ = std::thread(&CleanVideoRecorder::worker_fn, this);
}

void CleanVideoRecorder::stop_worker()
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

void CleanVideoRecorder::wait_session_jobs_drain_locked(
    std::unique_lock<std::mutex>& lock, uint64_t session_seq)
{
    auto pending_session_jobs = [&](uint64_t seq) {
        size_t pending = 0;
        for(const auto& job : jobs_) {
            if(job.session_seq == seq) {
                ++pending;
            }
        }
        return pending;
    };
    constexpr auto kDrainWarnInterval = std::chrono::seconds(2);
    uint32_t wait_warn_count = 0;

    while(pending_session_jobs(session_seq) > 0u ||
          (worker_busy_ && worker_busy_session_ == session_seq)) {
        if(idle_cv_.wait_for(lock, kDrainWarnInterval) == std::cv_status::timeout) {
            ++wait_warn_count;
            const size_t pending = pending_session_jobs(session_seq);
            UPC_LOG_CAMERA_SERVICE(
                Warn,
                "clean_video wait drain timeout seq=%llu pending=%zu worker_busy=%d worker_busy_session=%llu rounds=%u",
                static_cast<unsigned long long>(session_seq),
                pending,
                worker_busy_ ? 1 : 0,
                static_cast<unsigned long long>(worker_busy_session_),
                wait_warn_count);
        }
    }
}

void CleanVideoRecorder::worker_fn()
{
    for(;;) {
        CleanFrameJob job{};
        uint32_t frame_index = 0;
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
            worker_busy_ = true;
            worker_busy_session_ = job.session_seq;
            if(job.session_seq == session_seq_) {
                frame_index = frame_count_ + 1u;
            }
        }

        bool ok = false;
        if(frame_index > 0u) {
            ok = append_nv12_frame(job);
        }

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if(ok && job.session_seq == session_seq_ &&
               frame_index == frame_count_ + 1u) {
                ++frame_count_;
            } else if(frame_index > 0u && !ok) {
                UPC_LOG_CAMERA_SERVICE(Warn,
                    "clean_video raw append fail frame=%u input_frame=%u",
                    frame_index, job.input_frame);
            }
            worker_busy_ = false;
            worker_busy_session_ = 0;
        }
        idle_cv_.notify_all();
    }
}

void CleanVideoRecorder::on_session_start(
    const middleware::CleanVideoSessionStartPayload& req)
{
    std::unique_lock<std::mutex> lock(mtx_);
    stop_locked(lock, 0xfe, true);
    start_worker_if_needed_locked();

    session_id_ = req.session_id;
    job_name_ = sanitize_name(req.job_name);
    if(req.width > 0) {
        width_ = req.width;
    }
    if(req.height > 0) {
        height_ = req.height;
    }
    if(req.fps > 0) {
        fps_ = req.fps;
    }

    ++session_seq_;
    session_dir_ = std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + job_name_;
    raw_path_ = session_dir_ + "/" + kRawNv12Name;
    mp4_path_ = session_dir_ + "/" + kMp4Name;
    sample_step_ = std::max<uint32_t>(1u, (fps_ + kCleanRawFps - 1u) / kCleanRawFps);

    if(!ensure_dir(CAMERA_PRINT_VIDEO_OUT_DIR) || !ensure_dir(session_dir_)) {
        UPC_LOG_CAMERA_SERVICE(Error, "clean_video mkdir failed dir=%s",
                               session_dir_.c_str());
        return;
    }
    (void)std::remove(raw_path_.c_str());
    (void)std::remove(mp4_path_.c_str());

    dropped_frames_ = 0;
    input_frame_count_ = 0;
    frame_count_ = 0;
    active_ = true;
    UPC_LOG_CAMERA_SERVICE(
        Info,
        "clean_video start id=%s dir=%s %ux%u@%u sample_fps=%u sample_step=%u raw=%s",
        session_id_.c_str(), session_dir_.c_str(),
        width_, height_, fps_, kCleanRawFps, sample_step_, raw_path_.c_str());
}

void CleanVideoRecorder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv,
                                       uint32_t stride_y)
{
    uint64_t current_session_seq = 0;
    uint32_t current_width = 0;
    uint32_t current_height = 0;
    uint32_t current_input_frame = 0;
    std::string current_raw_path;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_) {
            return;
        }

        ++input_frame_count_;
        current_input_frame = input_frame_count_;
        if((current_input_frame - 1u) % sample_step_ != 0u) {
            return;
        }

        if(jobs_.size() >= kMaxPendingJobs) {
            ++dropped_frames_;
            if((dropped_frames_ % 20u) == 1u) {
                UPC_LOG_CAMERA_SERVICE(Warn,
                    "clean_video queue full drop=%u pending=%zu",
                    dropped_frames_, jobs_.size());
            }
            return;
        }

        current_session_seq = session_seq_;
        current_width = width_;
        current_height = height_;
        current_raw_path = raw_path_;
    }

    if(!src_y || !src_uv || stride_y == 0u ||
       current_width == 0u || current_height == 0u || current_raw_path.empty()) {
        UPC_LOG_CAMERA_SERVICE(
            Warn,
            "clean_video invalid frame input_frame=%u src_y=%p src_uv=%p stride=%u %ux%u raw_empty=%d",
            current_input_frame,
            static_cast<const void*>(src_y),
            static_cast<const void*>(src_uv),
            stride_y,
            current_width,
            current_height,
            current_raw_path.empty() ? 1 : 0);
        return;
    }
    if(stride_y < current_width) {
        UPC_LOG_CAMERA_SERVICE(
            Warn,
            "clean_video stride smaller than width input_frame=%u stride=%u width=%u",
            current_input_frame, stride_y, current_width);
        return;
    }

    const size_t need = static_cast<size_t>(current_width) * current_height * 3u / 2u;
    CleanFrameJob job{};
    job.session_seq = current_session_seq;
    job.width = current_width;
    job.height = current_height;
    job.input_frame = current_input_frame;
    job.raw_path = current_raw_path;
    job.nv12.resize(need);
    copy_nv12_tight(job.nv12.data(), src_y, src_uv, stride_y, current_width, current_height);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || current_session_seq != session_seq_) {
            return;
        }
        if(jobs_.size() >= kMaxPendingJobs) {
            ++dropped_frames_;
            if((dropped_frames_ % 20u) == 1u) {
                UPC_LOG_CAMERA_SERVICE(Warn,
                    "clean_video queue full after copy drop=%u pending=%zu",
                    dropped_frames_, jobs_.size());
            }
            return;
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void CleanVideoRecorder::on_session_stop(
    const middleware::CleanVideoSessionStopPayload& req)
{
    std::unique_lock<std::mutex> lock(mtx_);
    if(!session_id_.empty() && req.session_id[0] != '\0' &&
       session_id_ != req.session_id) {
        return;
    }
    stop_locked(lock, req.result_code, req.keep_raw != 0);
}

void CleanVideoRecorder::stop_locked(std::unique_lock<std::mutex>& lock,
                                     uint8_t result_code, bool keep_raw)
{
    if(!active_) {
        return;
    }
    active_ = false;

    const uint64_t session_seq = session_seq_;
    wait_session_jobs_drain_locked(lock, session_seq);

    const std::string stop_session_id = session_id_;
    const std::string stop_raw_path   = raw_path_;
    const std::string stop_mp4_path   = mp4_path_;
    const uint32_t stop_width         = width_;
    const uint32_t stop_height        = height_;
    const uint32_t stop_sample_fps    = kCleanRawFps;
    const uint32_t stop_frame_count   = frame_count_;
    const uint32_t stop_input_frames  = input_frame_count_;
    const uint32_t stop_dropped       = dropped_frames_;

    lock.unlock();

    const bool closed_by_user = (result_code == 0xfeu);
    bool mp4_ok = false;
    if(closed_by_user) {
        /* 返回首页触发 clean_test_close：临时 raw 直接丢弃，不合成 mp4。 */
        (void)std::remove(stop_raw_path.c_str());
        (void)std::remove(stop_mp4_path.c_str());
    } else if(stop_frame_count >= 2u) {
        mp4_ok = encode_raw_nv12_to_mp4(
            stop_raw_path, stop_mp4_path, stop_width, stop_height, stop_sample_fps);
        if(mp4_ok && !keep_raw) {
            (void)std::remove(stop_raw_path.c_str());
        }
    } else if(stop_frame_count == 1u) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "clean_video only one sampled frame, skip mp4 compose");
    }

    std::error_code ec;
    const bool raw_exists = !stop_raw_path.empty() &&
                            std::filesystem::exists(stop_raw_path, ec);
    ec.clear();
    const bool mp4_exists = !stop_mp4_path.empty() &&
                            std::filesystem::exists(stop_mp4_path, ec);
    ec.clear();

    UPC_LOG_CAMERA_SERVICE(
        Info,
        "clean_video stop id=%s result=%u sampled_frames=%u input_frames=%u dropped=%u keep_raw=%d closed_by_user=%d mp4_ok=%d raw_exists=%d mp4_exists=%d raw=%s mp4=%s",
        stop_session_id.c_str(),
        static_cast<unsigned>(result_code),
        stop_frame_count,
        stop_input_frames,
        stop_dropped,
        keep_raw ? 1 : 0,
        closed_by_user ? 1 : 0,
        mp4_ok ? 1 : 0,
        raw_exists ? 1 : 0,
        mp4_exists ? 1 : 0,
        stop_raw_path.c_str(),
        stop_mp4_path.c_str());

    lock.lock();

    session_id_.clear();
    raw_path_.clear();
    dropped_frames_ = 0;
    input_frame_count_ = 0;
    frame_count_ = 0;
}

} // namespace upc::camera
