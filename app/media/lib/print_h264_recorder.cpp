#include "print_h264_recorder.hpp"

#include "nv12_copy.hpp"
#include "rs_log.hpp"
#include "timelapse_util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#endif

namespace upc::media {

namespace {
/** 边录边封装输出文件名：固定为 print_h264.mp4。 */
constexpr const char* kH264Mp4Name = "print_h264.mp4";
/** 允许最多积压的未编码帧数；超过后丢帧，防止编码跟不上导致内存膨胀。 */
constexpr size_t kMaxPendingFrameJobs = 3u;
/** 等待封装进程退出的轮询间隔（ms）。 */
constexpr uint32_t kMuxerPollIntervalMs = 10u;
/** 队列满丢帧的汇总日志周期（ms）：逐帧打会反过来拖慢取流线程。 */
constexpr uint64_t kQueueFullLogPeriodMs = 5000u;

/** 单调时钟毫秒；仅用于日志节流与超时判定。 */
uint64_t monotonic_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool write_all(int fd, const uint8_t* data, size_t len) {
    if(fd < 0 || !data) return false;
    size_t off = 0;
    while(off < len) {
        const ssize_t wr = ::write(fd, data + off, len - off);
        if(wr < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(wr);
    }
    return true;
}

using upc::copy_nv12_tight;

/**
 * 若目录存在且为空，则删除该目录。
 *
 * @param dir 待检查目录。
 * @return 是否成功删除（或本来就不需要删除）。
 */
bool remove_empty_dir_if_exists(const std::string& dir) {
    if(dir.empty()) return false;
    std::error_code ec;
    if(!std::filesystem::is_directory(dir, ec) || !std::filesystem::is_empty(dir, ec)) return false;
    return std::filesystem::remove(dir, ec);
}

/**
 * 构建视频临时目录路径。
 *
 * @return 临时目录路径，由 UPC_VIDEO_TMP 宏定义。
 */
std::string build_video_tmp_dir() {
    return UPC_VIDEO_TMP;
}

} // namespace

/**
 * 统计任务队列中尚未处理的 WriteFrame 任务数量。
 */
size_t PrintH264Recorder::count_pending_write_frames(const std::deque<Job>& jobs) {
    size_t n = 0;
    for(const auto& j : jobs) if(j.kind == JobKind::WriteFrame) ++n;
    return n;
}

/** 析构时停止 worker 并关闭编码器。 */
PrintH264Recorder::~PrintH264Recorder() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        active_ = false;
        (void)close_encoder_locked();
    }
    stop_worker();
}

/**
 * 设置编码流参数。仅接受非零值，避免把相机未初始化时的 0 覆盖默认值。
 */
void PrintH264Recorder::set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(width > 0) width_ = width;
    if(height > 0) height_ = height;
    if(fps > 0) fps_ = fps;
}

/** 设置编码完成回调。 */
void PrintH264Recorder::set_finalize_done_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    finalize_done_cb_ = std::move(cb);
}

/** 设置存储预算检查回调。 */
void PrintH264Recorder::set_storage_budget_guard(std::function<bool()> cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    storage_budget_guard_ = std::move(cb);
}

void PrintH264Recorder::set_storage_exhausted_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    storage_exhausted_cb_ = std::move(cb);
}

/** 返回当前活跃会话目录。 */
std::string PrintH264Recorder::current_session_dir() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if(!active_) {
        return {};
    }
    return session_dir_;
}

/** 返回是否处于活跃会话。 */
bool PrintH264Recorder::session_active() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return active_;
}

/**
 * 生成打印录像缩略图。
 *
 * 取本对象最后一次成功编码的 NV12 帧，交给 timelapse_util 生成缩略图。
 *
 * @param out_path 输出缩略图路径。
 * @return 是否成功生成。
 */
bool PrintH264Recorder::create_final_thumb(const char* out_path) const {
    std::vector<uint8_t> nv12;
    uint32_t width = 0;
    uint32_t height = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        nv12 = last_nv12_;
        width = width_;
        height = height_;
    }
    return timelapse_util::create_raw_thumb_from_nv12(
        nv12, width, height, out_path);
}

/**
 * 清空会话路径字符串。
 * 调用前必须持有 mtx_。
 */
void PrintH264Recorder::clear_session_paths_locked() {
    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    h264_mp4_path_.clear();
}

/**
 * 重置一次会话的运行时状态。
 * 调用前必须持有 mtx_。
 */
void PrintH264Recorder::reset_session_state_locked() {
    active_ = false;
    paused_ = false;
    had_write_error_ = false;
    frame_count_ = 0;
    dropped_frames_ = 0;
    drop_log_last_ms_ = 0;
    last_nv12_.clear();
    bitrate_bps_ = CAMERA_PRINT_H264_BITRATE;
    clear_session_paths_locked();
}

/**
 * 如果后台线程未启动，则创建新线程。
 * 调用前必须持有 mtx_。
 */
void PrintH264Recorder::start_worker_if_needed_locked() {
    if(worker_.joinable()) return;
    worker_stop_ = false;
    worker_ = std::thread(&PrintH264Recorder::worker_fn, this);
}

/**
 * 通知 worker 停止并等待其结束。
 * 会先置停止标志，再唤醒条件变量，最后 join 线程。
 */
void PrintH264Recorder::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        worker_stop_ = true;
    }
    cv_.notify_all();
    if(worker_.joinable()) worker_.join();
}

/**
 * 打开 RKMPP 编码器。
 *
 * 如果编码器已经打开则直接返回 true。否则创建新的 MPP 上下文，初始化为 H.264
 * 编码器，并配置分辨率、码率、帧率、GOP 等参数。
 *
 * 调用前必须持有 mtx_。
 *
 * @return 是否成功打开。
 */
bool PrintH264Recorder::open_encoder_locked() {
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    had_write_error_ = true;
    UPC_LOG_MEDIA_SERVICE(Error, "print_h264 RKMPP disabled in this build");
    return false;
#else
    if(mpp_ready_) return true;
    (void)close_encoder_locked();
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppEncCfg cfg = nullptr;
    if(mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mpp_create fail");
        return false;
    }
    if(mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mpp_init fail");
        mpp_destroy(ctx);
        return false;
    }
    mpp_enc_cfg_init(&cfg);
    if(!cfg) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 mpp_enc_cfg_init fail");
        mpp_destroy(ctx);
        return false;
    }

    // 读取默认配置，然后按需修改。
    (void)mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
    const int fps = static_cast<int>(std::max(1u, fps_));
    const uint32_t use_bps =
        bitrate_bps_ > 0 ? bitrate_bps_ : CAMERA_PRINT_H264_BITRATE;
    const int bps = static_cast<int>(use_bps);
    // 设置图像参数：分辨率、stride、格式（NV12/YUV420SP）。
    mpp_enc_cfg_set_s32(cfg, "prep:width", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:height", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", static_cast<int>(width_));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", static_cast<int>(height_));
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    // 设置码率控制：CBR 模式，目标码率，输入/输出帧率，GOP 2 秒。
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
    if(!start_muxer_locked()) {
        had_write_error_ = true;
        UPC_LOG_MEDIA_SERVICE(Error, "print_h264 edge mux start failed");
        return false;
    }
    UPC_LOG_MEDIA_SERVICE(Info,
        "print_h264 encoder open %ux%u@%d bps=%d",
        width_, height_, fps, bps);
    return true;
#endif
}

bool PrintH264Recorder::start_muxer_locked() {
    if(muxer_stdin_fd_ >= 0 || muxer_pid_ > 0) return true;
    if(h264_mp4_path_.empty()) return false;

    int pipefd[2] = {-1, -1};
    if(::pipe(pipefd) != 0) {
        return false;
    }
    const pid_t pid = ::fork();
    if(pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }
    if(pid == 0) {
        ::close(pipefd[1]);
        if(::dup2(pipefd[0], STDIN_FILENO) < 0) _exit(127);
        ::close(pipefd[0]);
        int null_fd = ::open("/dev/null", O_WRONLY);
        if(null_fd >= 0) {
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }
        const uint32_t use_fps = fps_ > 0 ? fps_ : 1u;
        char fps_str[16] = {};
        std::snprintf(fps_str, sizeof(fps_str), "%u", use_fps);
        ::execl(CAMERA_FFMPEG_BIN, CAMERA_FFMPEG_BIN,
                "-y",
                "-loglevel", "error",
                "-fflags", "+genpts",
                "-framerate", fps_str,
                "-f", "h264",
                "-i", "pipe:0",
                "-c:v", "copy",
                h264_mp4_path_.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(pipefd[0]);
    muxer_stdin_fd_ = pipefd[1];
    muxer_pid_ = pid;
    return true;
}

bool PrintH264Recorder::stop_muxer_locked() {
    bool ok = true;
    if(muxer_stdin_fd_ >= 0) {
        ::close(muxer_stdin_fd_);
        muxer_stdin_fd_ = -1;
    }
    if(muxer_pid_ > 0) {
        int status = 0;
        bool exited = false;
        /* stdin 已关，ffmpeg 读到 EOF 后还要写 mp4 尾部的 moov 索引才会退出，
         * 耗时随帧数增长（几万帧远超百毫秒级）。这里必须等够：提前强杀留下的是
         * 没有 moov、播放器直接打不开的文件，而调用方只看文件非空会误判成功。 */
        for(uint32_t waited = 0;
            waited < UPC_PRINT_H264_MUX_FLUSH_TIMEOUT_MS;
            waited += kMuxerPollIntervalMs) {
            const pid_t w = ::waitpid(muxer_pid_, &status, WNOHANG);
            if(w == muxer_pid_) {
                exited = true;
                break;
            }
            if(w < 0 && errno == ECHILD) {
                exited = true;
                status = 0;
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kMuxerPollIntervalMs));
        }
        if(!exited && ::waitpid(muxer_pid_, &status, WNOHANG) == muxer_pid_) {
            exited = true;
        }
        if(!exited) {
            /* 超时与"退出码非 0"是两种故障，日志分开才能判断 mp4 是否可信。 */
            UPC_LOG_MEDIA_SERVICE(Error,
                "print_h264 muxer flush timeout after %ums — killing, mp4 may lack moov",
                static_cast<unsigned>(UPC_PRINT_H264_MUX_FLUSH_TIMEOUT_MS));
            (void)::kill(muxer_pid_, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if(::waitpid(muxer_pid_, &status, WNOHANG) == 0) {
                (void)::kill(muxer_pid_, SIGKILL);
                (void)::waitpid(muxer_pid_, &status, 0);
            }
            ok = false;
        } else if(!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            UPC_LOG_MEDIA_SERVICE(Error,
                "print_h264 muxer exit abnormal status=%d", status);
            ok = false;
        }
        muxer_pid_ = -1;
    }
    return ok;
}

/**
 * 关闭 RKMPP 编码器并冲刷残码流到文件。
 *
 * 先发送 EOS 帧，循环拉取编码器剩余 packet 写入文件，然后关闭文件、释放
 * MPP 资源。无论成败都会将 mpp_ready_ 置 false。
 *
 * 调用前必须持有 mtx_。
 *
 * @return 非 0 表示写盘过程中发生错误。
 */
int PrintH264Recorder::close_encoder_locked() {
#if !(defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP)
    return 0;
#else
    int rc = 0;
    bool mux_write_error = false;
    if(mpp_ready_) {
        auto* mpi = reinterpret_cast<MppApi*>(mpp_mpi_);
        auto ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
        MppFrame eos = nullptr;
        if(mpp_frame_init(&eos) == MPP_OK && eos) {
            mpp_frame_set_eos(eos, 1);
            (void)mpi->encode_put_frame(ctx, eos);
            mpp_frame_deinit(&eos);
        }
        // 最多等待 200ms，拉取编码器剩余的 packet。
        for(int i = 0; i < 200; ++i) {
            MppPacket pkt = nullptr;
            if(mpi->encode_get_packet(ctx, &pkt) != MPP_OK || !pkt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const void* ptr = mpp_packet_get_pos(pkt);
            const size_t len = static_cast<size_t>(mpp_packet_get_length(pkt));
            if(ptr && len > 0) {
                if(muxer_stdin_fd_ >= 0 &&
                   !write_all(muxer_stdin_fd_, static_cast<const uint8_t*>(ptr), len)) {
                    mux_write_error = true;
                }
            }
            const bool eos_hit = (mpp_packet_get_eos(pkt) != 0);
            mpp_packet_deinit(&pkt);
            if(eos_hit) break;
        }
    }
    if(mux_write_error || !stop_muxer_locked()) {
        rc = -1;
    }
    // 释放编码配置。
    if(mpp_cfg_) {
        mpp_enc_cfg_deinit(reinterpret_cast<MppEncCfg>(mpp_cfg_));
        mpp_cfg_ = nullptr;
    }
    // 销毁编码器上下文。
    if(mpp_ctx_) {
        mpp_destroy(reinterpret_cast<MppCtx>(mpp_ctx_));
        mpp_ctx_ = nullptr;
        mpp_mpi_ = nullptr;
    }
    mpp_ready_ = false;
    return rc;
#endif
}

/**
 * 处理一次打印期会话开始。
 *
 * 1. 等待 worker 处理完上一个会话的任务，避免 session 重叠。
 * 2. 关闭上一次编码器，清空状态。
 * 3. 若 h264_continuous_on 为 0 则直接返回，不启用连续录像。
 * 4. 创建临时目录，准备输出文件路径。
 */
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
    h264_mp4_path_ = session_dir_ + "/" + kH264Mp4Name;
    if(info.width > 0) width_ = info.width;
    if(info.height > 0) height_ = info.height;
    // 编码帧率以相机开流时 set_stream_geometry 为准，不使用 layer_fps（延时摄影）。
    bitrate_bps_ = info.h264_bitrate > 0 ? info.h264_bitrate
                                         : CAMERA_PRINT_H264_BITRATE;
    storage_exhausted_reported_ = false;

    // 与 PrintVideoRecorder 共用 tmp/ 目录：这里不做 remove_all，避免误删层间 PNG。
    // Media 模块已在 Session Start 前完成 recover 和清空 tmp。
    std::error_code ec;
    std::filesystem::create_directories(session_dir_, ec);
    if(ec) return;
    (void)std::remove(h264_mp4_path_.c_str());
    active_ = true;
    UPC_LOG_MEDIA_SERVICE(Info,
        "print_h264 session start id=%s bps=%u %ux%u@%u",
        session_id_.c_str(), bitrate_bps_, width_, height_, fps_);
}

/** 设置暂停状态。暂停期间到达的帧会被直接丢弃。 */
void PrintH264Recorder::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(mtx_);
    paused_ = paused;
}

/**
 * 正常结束打印期会话。
 * 仅当会话处于活跃状态且 session_id 匹配时入队 Finalize 任务。
 */
void PrintH264Recorder::on_session_finish(const middleware::PrintVideoSessionPayload& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(active_ && (info.session_id[0] == 0 || session_id_ == info.session_id)) {
        enqueue_finalize_locked(false);
    }
}

/**
 * 异常中止打印期会话。
 * 仅当会话处于活跃状态且 session_id 匹配时入队 Finalize 任务。
 */
void PrintH264Recorder::on_session_abort(const middleware::PrintVideoSessionPayload& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(active_ && (info.session_id[0] == 0 || session_id_ == info.session_id)) {
        enqueue_finalize_locked(true);
    }
}

/**
 * 每帧 NV12 回调入口。
 *
 * 设计要点：
 * - 仅做最小限度的检查：会话活跃、未暂停、无错误、数据有效。
 * - 若写队列已满，直接丢弃本帧，不阻塞上游。
 * - 在锁外完成 NV12 数据拷贝，减少锁持有时间。
 * - 入队前会再次校验会话状态，避免把旧会话的帧写入新会话。
 */
void PrintH264Recorder::on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y) {
    uint64_t seq = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || paused_ || had_write_error_ || !src_y || !src_uv || width_ == 0 || height_ == 0) return;
        if(count_pending_write_frames(jobs_) >= kMaxPendingFrameJobs) {
            ++dropped_frames_;
            maybe_log_queue_full_locked();
            return;
        }
        seq = session_seq_;
        w = width_;
        h = height_;
    }

    if(stride_y == 0) stride_y = w;
    if(stride_y < w) return;

    // 构造 WriteFrame 任务，并在锁外拷贝数据。
    Job job{};
    job.kind = JobKind::WriteFrame;
    job.session_seq = seq;
    job.width = w;
    job.height = h;
    job.nv12.resize(static_cast<size_t>(w) * h * 3u / 2u);
    copy_nv12_tight(job.nv12.data(), src_y, src_uv, stride_y, w, h);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        // 再次检查会话是否已切换或已停止，若已切换则丢弃本帧。
        if(!active_ || paused_ || had_write_error_ || seq != session_seq_) return;
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

/**
 * 队列满丢帧的汇总日志（kQueueFullLogPeriodMs 一行，首次丢帧立即打）。
 *
 * 丢帧判定发生在取流线程上，逐帧打日志会反过来拖慢采集，所以只做周期汇总。
 * 没有这行日志时，整场打印只能在 finalize 那一条看到丢帧总数，
 * 无法判断丢帧是均匀分布还是跟着层拍尖峰走。
 *
 * 调用前必须持有 mtx_。
 */
void PrintH264Recorder::maybe_log_queue_full_locked() {
    const uint64_t now_ms = monotonic_ms();
    if(drop_log_last_ms_ != 0 &&
       now_ms - drop_log_last_ms_ < kQueueFullLogPeriodMs) {
        return;
    }
    drop_log_last_ms_ = now_ms;
    UPC_LOG_MEDIA_SERVICE(Warn,
        "print_h264 queue full drop=%u enc=%u session=%s",
        dropped_frames_, frame_count_, session_id_.c_str());
}

/**
 * 入队清理临时目录任务。
 * 删除 print_h264.mp4 残留文件，并尝试删除空目录。
 */
void PrintH264Recorder::enqueue_cleanup_tmp() {
    std::lock_guard<std::mutex> lock(mtx_);
    start_worker_if_needed_locked();
    Job job{};
    job.kind = JobKind::Cleanup;
    job.cleanup_dir = build_video_tmp_dir();
    jobs_.push_back(std::move(job));
    cv_.notify_one();
}

/**
 * 入队 Finalize 任务。
 * 调用前必须持有 mtx_。
 *
 * @param aborted true 表示异常中止，false 表示正常结束。
 */
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

/**
 * 后台 worker 主循环。
 *
 * 从任务队列中弹出任务，标记当前忙碌状态，然后分发给对应的处理器。
 * 处理完成后通知 idle_cv_，让等待的 on_session_start 可以继续。
 */
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

/**
 * 处理 WriteFrame 任务：编码并写入一帧。
 *
 * 流程：
 * 1. 检查会话序号和错误状态是否仍然有效。
 * 2. 调用存储预算检查函数，若空间不足则跳过。
 * 3. 打开 RKMPP 编码器。
 * 4. 将 NV12 数据拷贝到 MPP buffer，构造 MPP frame 送入编码器。
 * 5. 循环从编码器拉取 H.264 packet 写入文件。
 * 6. 若成功，更新帧计数并接管 job.nv12 作为缩略图源帧。
 */
void PrintH264Recorder::run_write_frame_job(Job& job) {
    const size_t need = static_cast<size_t>(job.width) * job.height * 3u / 2u;
    void* ctx = nullptr;
    void* mpi = nullptr;
    int mux_fd = -1;
    std::function<bool()> guard;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq != session_seq_ || had_write_error_ || job.nv12.size() < need) return;
        guard = storage_budget_guard_;
    }
    if(guard && !guard()) {
        UPC_LOG_MEDIA_SERVICE(
            Warn, "STORCHK_SKIP_WRITE kind=print_h264 reason=budget_unavailable session=%s",
            job.session_id.c_str());
        std::function<void()> exhausted_cb;
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if(job.session_seq == session_seq_) {
                if(!storage_exhausted_reported_) {
                    storage_exhausted_reported_ = true;
                    should_notify = true;
                    exhausted_cb = storage_exhausted_cb_;
                }
                if(active_ && !finalize_in_progress_) {
                    UPC_LOG_MEDIA_SERVICE(
                        Warn, "print_h264 stop recording early due to storage budget session=%s",
                        job.session_id.c_str());
                    enqueue_finalize_locked(false);
                }
            }
        }
        if(should_notify && exhausted_cb) {
            exhausted_cb();
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(job.session_seq != session_seq_ || had_write_error_ || job.nv12.size() < need) return;
        if(!open_encoder_locked()) return;
#if defined(UPC_MEDIA_HAS_MPP) && UPC_MEDIA_HAS_MPP
        ctx = mpp_ctx_;
        mpi = mpp_mpi_;
#endif
        mux_fd = muxer_stdin_fd_;
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

    // 从编码器取出所有 packet 并写入文件。
    for(;;) {
        MppPacket pkt = nullptr;
        if(mpp_mpi->encode_get_packet(mpp_ctx, &pkt) != MPP_OK || !pkt) break;
        const void* p = mpp_packet_get_pos(pkt);
        const size_t n = static_cast<size_t>(mpp_packet_get_length(pkt));
        if(p && n > 0) {
            if(mux_fd >= 0 &&
               !write_all(mux_fd, static_cast<const uint8_t*>(p), n)) {
                write_error = true;
            }
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

    // 编码写盘成功，更新统计和最后一帧缓存。
    // job 随后即被 worker 丢弃，这里直接接管它的缓冲，省掉每帧一次 2MB 拷贝。
    std::lock_guard<std::mutex> lock(mtx_);
    if(job.session_seq == session_seq_ && !had_write_error_) {
        ++frame_count_;
        last_nv12_ = std::move(job.nv12);
    }
}

/**
 * 处理 Finalize 任务：结束会话收尾。
 *
 * 流程：
 * 1. 在锁内记录统计信息并关闭编码器。
 * 2. 若本次会话没有任何成功帧，删除生成的空文件和空目录。
 * 3. 输出日志。
 * 4. 触发 finalize 完成回调。
 */
void PrintH264Recorder::run_finalize_job(Job job) {
    std::string h264_mp4;
    std::string dir;
    uint32_t frames = 0;
    bool err = false;
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        h264_mp4 = h264_mp4_path_;
        dir = session_dir_;
        frames = frame_count_;
        err = had_write_error_;
        dropped = dropped_frames_;
        if(job.session_seq == session_seq_) {
            if(close_encoder_locked() != 0) {
                err = true;
            }
            clear_session_paths_locked();
            frame_count_ = 0;
            dropped_frames_ = 0;
            had_write_error_ = false;
            paused_ = false;
        }
    }

    // 仅当完全没有帧时清理封装文件，避免空壳 mp4。
    if(frames == 0) {
        (void)std::remove(h264_mp4.c_str());
        (void)remove_empty_dir_if_exists(dir);
    }

    UPC_LOG_MEDIA_SERVICE(Info,
        "print_h264 finalize id=%s aborted=%d frames=%u dropped=%u err=%d",
        job.session_id.c_str(), job.aborted ? 1 : 0, frames, dropped, err ? 1 : 0);

    std::function<void()> done_cb;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        finalize_in_progress_ = false;
        done_cb = finalize_done_cb_;
        finalize_cv_.notify_all();
        idle_cv_.notify_all();
    }
    if(done_cb) {
        done_cb();
    }
}

/**
 * 处理 Cleanup 任务：删除临时目录下的 print_h264.mp4。
 */
void PrintH264Recorder::run_cleanup_job(const Job& job) {
    (void)std::remove((job.cleanup_dir + "/print_h264.mp4").c_str());
    (void)remove_empty_dir_if_exists(job.cleanup_dir);
}

/**
 * 将字符串转换为可用作文件名的安全字符串。
 *
 * 仅保留字母、数字、下划线、连字符和点号，其它字符替换为下划线。
 * 若输入为空，返回默认字符串 "print"。
 */
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
