#include "clean_video_recorder.hpp"

#include "nv12_copy.hpp"
#include "msg_port.hpp"
#include "print_history_manifest.hpp"
#include "rs_log.hpp"
#include "timelapse_util.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace upc::media {

namespace {

constexpr size_t kMaxPendingJobs = 2u;

bool update_clean_history_metadata(const std::string& frames_dir,
                                   const std::string& thumb_rel,
                                   uint32_t clean_first_png,
                                   uint32_t clean_last_png)
{
    if(frames_dir.empty()) {
        return false;
    }
    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "clean_video history load fail path=%s", UPC_PRINT_VIDEO_MANIFEST);
        return false;
    }

    bool found = false;
    for(auto& e : entries) {
        if(e.frames_dir == frames_dir) {
            if(!thumb_rel.empty()) {
                e.thumb = thumb_rel;
            }
            e.clean_first_png = clean_first_png;
            e.clean_last_png = clean_last_png;
            found = true;
            break;
        }
    }
    if(!found) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "clean_video history miss frames_dir=%s", frames_dir.c_str());
        return false;
    }

    const std::string tmp_path = std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries)) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "clean_video history save fail tmp=%s", tmp_path.c_str());
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    if(std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "clean_video history rename fail %s -> %s: %s",
            tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST, std::strerror(errno));
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

} // namespace

CleanVideoRecorder::~CleanVideoRecorder()
{
    {
        std::unique_lock<std::mutex> lock(mtx_);
        stop_locked(lock, 0xff);
    }
    stop_worker();
}

void CleanVideoRecorder::set_stream_geometry(uint32_t width, uint32_t height,
                                             uint32_t fps)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(width > 0) {
        width_ = width;
    }
    if(height > 0) {
        height_ = height;
    }
    if(fps > 0) {
        stream_fps_ = fps;
    }
}

void CleanVideoRecorder::set_timelapse_fps(uint32_t fps)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(fps > 0) {
        timelapse_fps_ = fps;
        sample_interval_us_ = 1000000ull / static_cast<uint64_t>(fps);
    }
}

uint64_t CleanVideoRecorder::monotonic_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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
            UPC_LOG_MEDIA_SERVICE(
                Warn,
                "clean_video wait drain timeout seq=%llu pending=%zu worker_busy=%d rounds=%u",
                static_cast<unsigned long long>(session_seq),
                pending,
                worker_busy_ ? 1 : 0,
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
                frame_index = print_frame_count_ + clean_frame_count_ + 1u;
            }
        }

        bool ok = false;
        if(frame_index > 0u) {
            ok = timelapse_util::encode_nv12_to_jpeg(
                job.nv12.data(), job.nv12.size(),
                job.width, job.height, job.session_dir, frame_index);
        }

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if(ok && job.session_seq == session_seq_ &&
               frame_index == print_frame_count_ + clean_frame_count_ + 1u) {
                ++clean_frame_count_;
                last_clean_nv12_ = std::move(job.nv12);
            } else if(frame_index > 0u && !ok) {
                UPC_LOG_MEDIA_SERVICE(Warn,
                    "clean_video jpeg encode fail frame=%u", frame_index);
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
    stop_locked(lock, 0xfe);
    start_worker_if_needed_locked();

    session_id_ = req.session_id;
    job_name_ = timelapse_util::sanitize_filename(req.job_name);
    if(req.width > 0) {
        width_ = req.width;
    }
    if(req.height > 0) {
        height_ = req.height;
    }
    if(req.fps > 0) {
        timelapse_fps_ = req.fps;
    }
    if(timelapse_fps_ == 0) {
        timelapse_fps_ = CAMERA_PRINT_VIDEO_LAYER_FPS;
    }
    sample_interval_us_ =
        1000000ull / static_cast<uint64_t>(std::max(1u, timelapse_fps_));

    ++session_seq_;
    /* 结果页未 finalize 前 JPEG 在 video/tmp；已 promote 则在 {job}/ */
    const std::string tmp_dir = UPC_VIDEO_TMP;
    const std::string job_dir =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + job_name_;
    const uint32_t tmp_jpeg = timelapse_util::scan_max_jpeg_index(tmp_dir);
    if(tmp_jpeg > 0u) {
        session_dir_ = tmp_dir;
    } else {
        session_dir_ = job_dir;
    }

    if(!timelapse_util::mkdir_p(CAMERA_PRINT_VIDEO_OUT_DIR) ||
       !timelapse_util::mkdir_p(session_dir_.c_str())) {
        UPC_LOG_MEDIA_SERVICE(Error, "clean_video mkdir failed dir=%s",
                               session_dir_.c_str());
        return;
    }

    print_frame_count_ = timelapse_util::scan_max_jpeg_index(session_dir_);
    clean_frame_count_ = 0;
    last_clean_nv12_.clear();
    dropped_frames_ = 0;
    input_frame_count_ = 0;
    next_sample_us_ = 0;
    active_ = true;

    UPC_LOG_MEDIA_SERVICE(
        Info,
        "clean_video start id=%s dir=%s %ux%u stream_fps=%u timelapse_fps=%u print_png=%u",
        session_id_.c_str(), session_dir_.c_str(),
        width_, height_, stream_fps_, timelapse_fps_, print_frame_count_);
    if(print_frame_count_ == 0u) {
        UPC_LOG_MEDIA_SERVICE(
            Warn,
            "clean_video disabled for merge: no print png found dir=%s",
            session_dir_.c_str());
    }
}

void CleanVideoRecorder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv,
                                       uint32_t stride_y)
{
    uint64_t current_session_seq = 0;
    uint32_t current_width = 0;
    uint32_t current_height = 0;
    std::string current_session_dir;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_) {
            return;
        }
        /* 清粉 JPEG 仅在存在打印段 JPEG 时才允许参与后续合成。 */
        if(print_frame_count_ == 0u) {
            return;
        }

        ++input_frame_count_;
        const uint64_t now_us = monotonic_us();
        if(next_sample_us_ == 0) {
            next_sample_us_ = now_us;
        }
        if(now_us < next_sample_us_) {
            return;
        }
        /* 按目标间隔推进，落后时追赶，避免积压后连拍 */
        do {
            next_sample_us_ += sample_interval_us_;
        } while(next_sample_us_ <= now_us);

        if(jobs_.size() >= kMaxPendingJobs) {
            ++dropped_frames_;
            if((dropped_frames_ % 10u) == 1u) {
                UPC_LOG_MEDIA_SERVICE(Warn,
                    "clean_video queue full drop=%u pending=%zu",
                    dropped_frames_, jobs_.size());
            }
            return;
        }

        current_session_seq = session_seq_;
        current_width = width_;
        current_height = height_;
        current_session_dir = session_dir_;
    }

    if(!src_y || !src_uv || stride_y == 0u ||
       current_width == 0u || current_height == 0u ||
       current_session_dir.empty()) {
        return;
    }
    if(stride_y < current_width) {
        UPC_LOG_MEDIA_SERVICE(
            Warn,
            "clean_video stride smaller than width stride=%u width=%u",
            stride_y, current_width);
        return;
    }

    const size_t need =
        static_cast<size_t>(current_width) * current_height * 3u / 2u;
    CleanFrameJob job{};
    job.session_seq = current_session_seq;
    job.width = current_width;
    job.height = current_height;
    job.session_dir = current_session_dir;
    job.nv12.resize(need);
    upc::copy_nv12_tight(job.nv12.data(), src_y, src_uv, stride_y,
                    current_width, current_height);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || current_session_seq != session_seq_) {
            return;
        }
        if(jobs_.size() >= kMaxPendingJobs) {
            ++dropped_frames_;
            return;
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void CleanVideoRecorder::publish_clean_test_done(
    uint8_t result_code, bool has_mp4, const std::string& mp4_rel_path) const
{
    middleware::CleanTestDonePayload ntf{};
    ntf.result_code = result_code;
    ntf.raw_retained = 0;
    ntf.has_mp4 = has_mp4 ? 1 : 0;
    if(!mp4_rel_path.empty()) {
        std::snprintf(ntf.mp4_rel_path, sizeof(ntf.mp4_rel_path), "%s",
                      mp4_rel_path.c_str());
    }

    middleware::Message msg(
        middleware::MsgType::APP_Clean_Test_Result_Ntf,
        middleware::ModuleId::APP_MEDIA,
        middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    middleware::MsgPort::instance().publish_async(msg);
}

void CleanVideoRecorder::on_session_stop(
    const middleware::CleanVideoSessionStopPayload& req)
{
    std::unique_lock<std::mutex> lock(mtx_);
    if(!session_id_.empty() && req.session_id[0] != '\0' &&
       session_id_ != req.session_id) {
        return;
    }
    stop_locked(lock, req.result_code);
}

void CleanVideoRecorder::stop_locked(std::unique_lock<std::mutex>& lock,
                                     uint8_t result_code)
{
    if(!active_) {
        return;
    }
    active_ = false;

    const uint64_t session_seq = session_seq_;
    wait_session_jobs_drain_locked(lock, session_seq);

    const std::string stop_session_id = session_id_;
    const std::string stop_session_dir = session_dir_;
    const uint32_t stop_width = width_;
    const uint32_t stop_height = height_;
    const uint32_t stop_print_frames = print_frame_count_;
    const uint32_t stop_clean_frames = clean_frame_count_;
    const uint32_t stop_input_frames = input_frame_count_;
    const uint32_t stop_dropped = dropped_frames_;
    std::vector<uint8_t> stop_last_nv12 = std::move(last_clean_nv12_);

    lock.unlock();

    const bool closed_by_user = (result_code == 0xfe);
    const bool internal_stop = (result_code == 0xff);
    const bool mcu_clean_done = !closed_by_user && !internal_stop;
    const uint32_t total_frames = stop_print_frames + stop_clean_frames;
    const uint32_t clean_first =
        stop_clean_frames > 0 ? stop_print_frames + 1u : 0u;
    const uint32_t clean_last =
        stop_clean_frames > 0 ? total_frames : 0u;

    bool thumb_ok = false;
    bool history_ok = false;

    const bool allow_clean_merge = (stop_print_frames > 0u);
    /* 清单主键与会话目录一致：未 finalize 时为 tmp，否则为 job 名 */
    const std::string frames_dir_key =
        std::filesystem::path(stop_session_dir).filename().string();

    if(closed_by_user || internal_stop) {
        if(clean_first > 0) {
            timelapse_util::remove_jpeg_range(stop_session_dir, clean_first,
                                             clean_last);
        }
        const bool dir_removed = timelapse_util::remove_empty_dir(stop_session_dir);
        if(dir_removed) {
            UPC_LOG_MEDIA_SERVICE(Info,
                "clean_video removed empty session dir=%s",
                stop_session_dir.c_str());
        }
        history_ok = update_clean_history_metadata(frames_dir_key, std::string{}, 0u, 0u);
    } else if(!allow_clean_merge) {
        /* 没有打印段 JPEG 时，清粉段不允许参与合成：强制清理并清空清粉范围元数据。 */
        if(clean_first > 0) {
            timelapse_util::remove_jpeg_range(stop_session_dir, clean_first,
                                             clean_last);
        }
        const bool dir_removed = timelapse_util::remove_empty_dir(stop_session_dir);
        if(dir_removed) {
            UPC_LOG_MEDIA_SERVICE(Info,
                "clean_video removed empty session dir=%s",
                stop_session_dir.c_str());
        }
        UPC_LOG_MEDIA_SERVICE(
            Info,
            "clean_video skip merge: print_png=0, cleaned residual clean png");
        history_ok = update_clean_history_metadata(frames_dir_key, std::string{}, 0u, 0u);
    } else if(stop_clean_frames == 0) {
        UPC_LOG_MEDIA_SERVICE(Info,
            "clean_video success with 0 clean frames, keep print png only");
        history_ok = update_clean_history_metadata(frames_dir_key, std::string{}, 0u, 0u);
    } else {
        const std::string thumb_path =
            stop_session_dir + "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;

        if(!stop_last_nv12.empty()) {
            (void)std::remove(thumb_path.c_str());
            thumb_ok = timelapse_util::create_raw_thumb_from_nv12(
                stop_last_nv12, stop_width, stop_height, thumb_path.c_str());
        }

        const std::string thumb_rel =
            thumb_ok ? (frames_dir_key + "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT)
                     : std::string{};
        history_ok = update_clean_history_metadata(
            frames_dir_key, thumb_rel, clean_first, clean_last);
    }

    UPC_LOG_MEDIA_SERVICE(
        Info,
        "clean_video stop id=%s result=%u print_png=%u clean_png=%u input=%u dropped=%u closed_by_user=%d thumb_ok=%d history_ok=%d finalize_pending=1",
        stop_session_id.c_str(),
        static_cast<unsigned>(result_code),
        stop_print_frames,
        stop_clean_frames,
        stop_input_frames,
        stop_dropped,
        closed_by_user ? 1 : 0,
        thumb_ok ? 1 : 0,
        history_ok ? 1 : 0);

    /* 用户关闭/内部收尾不发 Done；MCU 上报结束(00/01/02)统一发 Done。 */
    if(mcu_clean_done) {
        publish_clean_test_done(result_code, false, std::string{});
    }

    lock.lock();
    session_id_.clear();
    job_name_.clear();
    session_dir_.clear();
    print_frame_count_ = 0;
    clean_frame_count_ = 0;
    dropped_frames_ = 0;
    input_frame_count_ = 0;
    next_sample_us_ = 0;
    last_clean_nv12_.clear();
}

} // namespace upc::media
