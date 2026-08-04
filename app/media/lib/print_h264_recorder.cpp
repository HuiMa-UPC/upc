#include "print_h264_recorder.hpp"

#include "rs_log.hpp"
#include "timelapse_util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#endif

namespace upc::media {

namespace {
constexpr const char* kH264Name = "print.h264";
constexpr size_t kMaxPendingFrameJobs = 3u;

void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y, const uint8_t* src_uv,
                     uint32_t src_stride_y, uint32_t width, uint32_t height) {
    const size_t y_size = static_cast<size_t>(width) * height;
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }
    for(uint32_t r = 0; r < height; ++r) {
        std::memcpy(dst + static_cast<size_t>(r) * width, src_y + static_cast<size_t>(r) * src_stride_y, width);
    }
    for(uint32_t r = 0; r < height / 2u; ++r) {
        std::memcpy(dst_uv + static_cast<size_t>(r) * width, src_uv + static_cast<size_t>(r) * src_stride_y, width);
    }
}

bool remove_empty_dir_if_exists(const std::string& dir) {
    if(dir.empty()) return false;
    std::error_code ec;
    if(!std::filesystem::is_directory(dir, ec) || !std::filesystem::is_empty(dir, ec)) return false;
    return std::filesystem::remove(dir, ec);
}

std::string build_video_tmp_dir() {
    return UPC_VIDEO_TMP;
}

} // namespace

size_t PrintH264Recorder::count_pending_write_frames(const std::deque<Job>& jobs) {
    size_t n = 0;
    for(const auto& j : jobs) if(j.kind == JobKind::WriteFrame) ++n;
    return n;
}

PrintH264Recorder::~PrintH264Recorder() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        active_ = false;
        (void)close_encoder_locked();
    }
    stop_worker();
}

void PrintH264Recorder::set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(width > 0) width_ = width;
    if(height > 0) height_ = height;
    if(fps > 0) fps_ = fps;
}

void PrintH264Recorder::clear_session_paths_locked() {
    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    h264_path_.clear();
}

void PrintH264Recorder::reset_session_state_locked() {
    active_ = false;
    paused_ = false;
    had_write_error_ = false;
    frame_count_ = 0;
    dropped_frames_ = 0;
    clear_session_paths_locked();
}

void PrintH264Recorder::start_worker_if_needed_locked() {
    if(worker_.joinable()) return;
    worker_stop_ = false;
    worker_ = std::thread(&PrintH264Recorder::worker_fn, this);
}

void PrintH264Recorder::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        worker_stop_ = true;
    }
    cv_.notify_all();
    if(worker_.joinable()) worker_.join();
}

bool PrintH264Recorder::open_encoder_locked() {
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    had_write_error_ = true;
    UPC_LOG_MEDIA_SERVICE(Error, "print_h264 RKMPP disabled in this build");
    return false;
#else
    if(mpp_ready_) return true;
    (void)close_encoder_locked();
    h264_fp_ = std::fopen(h264_path_.c_str(), "ab");
    if(!h264_fp_) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 open h264 fail path=%s err=%s",
                              h264_path_.c_str(), std::strerror(errno));
        return false;
    }

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppEncCfg cfg = nullptr;
    if(mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mpp_create fail");
        if(h264_fp_) {
            std::fclose(h264_fp_);
            h264_fp_ = nullptr;
        }
        return false;
    }
    if(mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mpp_init fail");
        mpp_destroy(ctx);
        if(h264_fp_) {
            std::fclose(h264_fp_);
            h264_fp_ = nullptr;
        }
        return false;
    }
    mpp_enc_cfg_init(&cfg);
    if(!cfg) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mpp_enc_cfg_init fail");
        mpp_destroy(ctx);
        if(h264_fp_) {
            std::fclose(h264_fp_);
            h264_fp_ = nullptr;
        }
        return false;
    }

    (void)mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
    const int fps = static_cast<int>(std::max(1u, fps_));
    const int bps = static_cast<int>(CAMERA_PRINT_H264_BITRATE);
    mpp_enc_cfg_set_s32(cfg, "prep:width", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:height", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 2);
    (void)mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

    mpp_ctx_ = ctx;
    mpp_mpi_ = mpi;
    mpp_cfg_ = cfg;
    mpp_ready_ = true;
    return true;
#endif
}

int PrintH264Recorder::close_encoder_locked() {
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    return 0;
#else
    int rc = 0;
    if(mpp_ready_) {
        auto* mpi = reinterpret_cast<MppApi*>(mpp_mpi_);
        auto ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
        MppFrame eos = nullptr;
        if(mpp_frame_init(&eos) == MPP_OK && eos) {
            mpp_frame_set_eos(eos, 1);
            (void)mpi->encode_put_frame(ctx, eos);
            mpp_frame_deinit(&eos);
        }
        for(int i = 0; i < 200; ++i) {
            MppPacket pkt = nullptr;
            if(mpi->encode_get_packet(ctx, &pkt) != MPP_OK || !pkt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const void* ptr = mpp_packet_get_pos(pkt);
            const size_t len = static_cast<size_t>(mpp_packet_get_length(pkt));
            if(ptr && len > 0 && h264_fp_ && std::fwrite(ptr, 1, len, h264_fp_) != len) rc = -1;
            const bool eos_hit = (mpp_packet_get_eos(pkt) != 0);
            mpp_packet_deinit(&pkt);
            if(eos_hit) break;
        }
    }
    if(h264_fp_) {
        std::fflush(h264_fp_);
        std::fclose(h264_fp_);
        h264_fp_ = nullptr;
    }
    if(mpp_cfg_) {
        mpp_enc_cfg_deinit(reinterpret_cast<MppEncCfg>(mpp_cfg_));
        mpp_cfg_ = nullptr;
    }
    if(mpp_ctx_) {
        mpp_destroy(reinterpret_cast<MppCtx>(mpp_ctx_));
        mpp_ctx_ = nullptr;
        mpp_mpi_ = nullptr;
    }
    mpp_ready_ = false;
    return rc;
#endif
}

void PrintH264Recorder::on_session_start(const middleware::PrintVideoSessionPayload& info) {
    std::unique_lock<std::mutex> lock(mtx_);
    while(finalize_in_progress_ || worker_busy_ || !jobs_.empty()) {
        idle_cv_.wait(lock);
    }

    (void)close_encoder_locked();
    reset_session_state_locked();

    if(info.h264_continuous_on == 0) return;

    start_worker_if_needed_locked();
    ++session_seq_;
    session_id_ = info.session_id;
    session_name_ = sanitize_filename(info.job_name);
    session_dir_ = build_video_tmp_dir();
    h264_path_ = session_dir_ + "/" + kH264Name;
    if(info.width > 0) width_ = info.width;
    if(info.height > 0) height_ = info.height;
    if(info.layer_fps > 0) fps_ = info.layer_fps;

    /* 与 PrintVideoRecorder 共用 tmp/：不在此 remove_all，避免清掉层间 PNG。
     * Media 在 Session Start 前已 recover+清空 tmp。 */
    std::error_code ec;
    std::filesystem::create_directories(session_dir_, ec);
    if(ec) return;
    active_ = true;
}

void PrintH264Recorder::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(mtx_);
    paused_ = paused;
}

void PrintH264Recorder::on_session_finish(const middleware::PrintVideoSessionPayload& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(active_ && (info.session_id[0] == 0 || session_id_ == info.session_id)) {
        enqueue_finalize_locked(false);
    }
}

void PrintH264Recorder::on_session_abort(const middleware::PrintVideoSessionPayload& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(active_ && (info.session_id[0] == 0 || session_id_ == info.session_id)) {
        enqueue_finalize_locked(true);
    }
}

void PrintH264Recorder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y) {
    uint64_t seq = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || paused_ || had_write_error_ || !src_y || !src_uv || width_ == 0 || height_ == 0) return;
        if(count_pending_write_frames(jobs_) >= kMaxPendingFrameJobs) {
            ++dropped_frames_;
            return;
        }
        seq = session_seq_;
        w = width_;
        h = height_;
    }

    if(stride_y == 0) stride_y = w;
    if(stride_y < w) return;

    Job job{};
    job.kind = JobKind::WriteFrame;
    job.session_seq = seq;
    job.width = w;
    job.height = h;
    job.nv12.resize(static_cast<size_t>(w) * h * 3u / 2u);
    copy_nv12_tight(job.nv12.data(), src_y, src_uv, stride_y, w, h);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || paused_ || had_write_error_ || seq != session_seq_) return;
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void PrintH264Recorder::enqueue_cleanup_tmp() {
    std::lock_guard<std::mutex> lock(mtx_);
    start_worker_if_needed_locked();
    Job job{};
    job.kind = JobKind::Cleanup;
    job.cleanup_dir = build_video_tmp_dir();
    jobs_.push_back(std::move(job));
    cv_.notify_one();
}

void PrintH264Recorder::enqueue_finalize_locked(bool aborted) {
    start_worker_if_needed_locked();
    active_ = false;
    finalize_in_progress_ = true;
    Job job{};
    job.kind = JobKind::Finalize;
    job.session_seq = session_seq_;
    job.aborted = aborted;
    job.session_id = session_id_;
    jobs_.push_back(std::move(job));
    cv_.notify_one();
}

void PrintH264Recorder::worker_fn() {
    for(;;) {
        Job job{};
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] { return worker_stop_ || !jobs_.empty(); });
            if(worker_stop_ && jobs_.empty()) break;
            if(jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            worker_busy_ = true;
            worker_busy_session_ = job.session_seq;
        }
        if(job.kind == JobKind::WriteFrame) run_write_frame_job(job);
        else if(job.kind == JobKind::Finalize) run_finalize_job(std::move(job));
        else run_cleanup_job(job);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            worker_busy_ = false;
            worker_busy_session_ = 0;
        }
        idle_cv_.notify_all();
    }
}

void PrintH264Recorder::run_write_frame_job(const Job& job) {
    const size_t need = static_cast<size_t>(job.width) * job.height * 3u / 2u;
    void* ctx = nullptr;
    void* mpi = nullptr;
    FILE* out_fp = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq != session_seq_ || had_write_error_ || job.nv12.size() < need) return;
        if(!open_encoder_locked()) return;
#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
        ctx = mpp_ctx_;
        mpi = mpp_mpi_;
#endif
        out_fp = h264_fp_;
    }

#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
    auto mpp_ctx = reinterpret_cast<MppCtx>(ctx);
    auto* mpp_mpi = reinterpret_cast<MppApi*>(mpi);
    MppBuffer buf = nullptr;
    if(mpp_buffer_get(nullptr, &buf, need) != MPP_OK || !buf) {
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq == session_seq_) {
            had_write_error_ = true;
        }
        return;
    }
    void* ptr = mpp_buffer_get_ptr(buf);
    if(!ptr) {
        mpp_buffer_put(buf);
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq == session_seq_) {
            had_write_error_ = true;
        }
        return;
    }
    std::memcpy(ptr, job.nv12.data(), need);

    MppFrame frame = nullptr;
    if(mpp_frame_init(&frame) != MPP_OK || !frame) {
        mpp_buffer_put(buf);
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq == session_seq_) {
            had_write_error_ = true;
        }
        return;
    }
    mpp_frame_set_width(frame, job.width);
    mpp_frame_set_height(frame, job.height);
    mpp_frame_set_hor_stride(frame, job.width);
    mpp_frame_set_ver_stride(frame, job.height);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buf);

    bool write_error = (mpp_mpi->encode_put_frame(mpp_ctx, frame) != MPP_OK);
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buf);

    for(;;) {
        MppPacket pkt = nullptr;
        if(mpp_mpi->encode_get_packet(mpp_ctx, &pkt) != MPP_OK || !pkt) break;
        const void* p = mpp_packet_get_pos(pkt);
        const size_t n = static_cast<size_t>(mpp_packet_get_length(pkt));
        if(p && n > 0 && out_fp && std::fwrite(p, 1, n, out_fp) != n) {
            write_error = true;
        }
        mpp_packet_deinit(&pkt);
    }
    if(write_error) {
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq == session_seq_) {
            had_write_error_ = true;
        }
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(mtx_);
    if(job.session_seq == session_seq_ && !had_write_error_) {
        ++frame_count_;
    }
}

void PrintH264Recorder::run_finalize_job(Job job) {
    std::string h264;
    std::string dir;
    uint32_t frames = 0;
    bool err = false;
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        h264 = h264_path_;
        dir = session_dir_;
        frames = frame_count_;
        err = had_write_error_;
        dropped = dropped_frames_;
        if(job.session_seq == session_seq_) {
            (void)close_encoder_locked();
            clear_session_paths_locked();
            frame_count_ = 0;
            dropped_frames_ = 0;
            had_write_error_ = false;
            paused_ = false;
        }
    }

    /* 终止打印也保留连续录像裸流；仅当完全无帧时才清理。 */
    if(frames == 0) {
        (void)std::remove(h264.c_str());
        (void)remove_empty_dir_if_exists(dir);
    }

    UPC_LOG_MEDIA_SERVICE(Info,
        "print_h264 finalize id=%s aborted=%d frames=%u dropped=%u err=%d",
        job.session_id.c_str(), job.aborted ? 1 : 0, frames, dropped, err ? 1 : 0);

    std::lock_guard<std::mutex> lock(mtx_);
    finalize_in_progress_ = false;
    finalize_cv_.notify_all();
    idle_cv_.notify_all();
}

void PrintH264Recorder::run_cleanup_job(const Job& job) {
    (void)std::remove((job.cleanup_dir + "/print.h264").c_str());
    (void)std::remove((job.cleanup_dir + "/print_h264.mp4").c_str());
    (void)remove_empty_dir_if_exists(job.cleanup_dir);
}

std::string PrintH264Recorder::sanitize_filename(const char* name) {
    if(!name || !name[0]) return "print";
    std::string out(name);
    for(char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '_' && c != '-' && c != '.') c = '_';
    }
    if(out.empty()) out = "print";
    return out;
}

} // namespace upc::media
