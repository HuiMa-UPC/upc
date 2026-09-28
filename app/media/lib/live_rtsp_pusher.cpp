#include "live_rtsp_pusher.hpp"

#include "device_config.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

namespace upc::media {

namespace {
/** 云端 ack reason：已有推流会话，拒绝并发 start。 */
constexpr const char* kReasonPushAlreadyActive = "push_already_active";
/** 云端 ack reason：编码器/ffmpeg/几何等启动失败。 */
constexpr const char* kReasonEncoderError = "encoder_error";
/** 云端 ack reason：stream_type 非 live 等非法状态。 */
constexpr const char* kReasonInvalidState = "invalid_state";
} // namespace

/** 析构：摘 sink 并关闭 ffmpeg。 */
LiveRtspPusher::~LiveRtspPusher()
{
    teardown_session();
}

/** 绑定共用编码器；必须在首次 start() 之前注入。 */
void LiveRtspPusher::set_encoder(H264StreamEncoder* encoder)
{
    std::lock_guard<std::mutex> lock(mtx_);
    encoder_ = encoder;
}

/** 返回当前是否处于推流会话。 */
bool LiveRtspPusher::active() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return active_;
}

/** 墙钟毫秒时间戳，填入 RtspPushRspPayload::push_started_at。 */
uint64_t LiveRtspPusher::unix_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/**
 * 启动 live 推流会话。
 *
 * 校验顺序：stream_type → push_url → 未在推 → 编码器已绑定。
 * 成功路径：fork ffmpeg → 注册编码器 sink → active_=true。
 * sink 注册失败（几何未知 / RKMPP 打不开）会回滚 ffmpeg，
 * 与重构前 open_encoder_locked 失败的云端 ack 语义一致。
 */
middleware::RtspPushRspPayload LiveRtspPusher::start(
    const middleware::RtspPushStartReqPayload& req)
{
    middleware::RtspPushRspPayload rsp{};
    std::snprintf(rsp.req_id, sizeof(rsp.req_id), "%s", req.req_id);
    std::snprintf(rsp.stream_type, sizeof(rsp.stream_type), "%s",
                  req.stream_type[0] ? req.stream_type : "live");

    const std::string stream_type =
        req.stream_type[0] ? std::string(req.stream_type) : "live";
    /* 一期仅 live；其它类型直接 Rejected，避免误开推流。 */
    if(stream_type != "live") {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Rejected);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonInvalidState);
        return rsp;
    }
    if(!req.push_url[0]) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    if(active_) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Rejected);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonPushAlreadyActive);
        return rsp;
    }
    if(!encoder_) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "live_rtsp start fail: encoder not bound");
        return rsp;
    }

    stop_ffmpeg_locked();
    if(!start_ffmpeg_locked(req.push_url)) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }
    /* writer 必须先于 sink 就绪：sink 一注册就可能有包入队。 */
    start_writer_locked();

    /* 云端 max_bitrate_kbps 不再参与编码配置：直播与录像共用一路 MPP，
     * 码率固定取板端 UPC_LIVE_H264_BITRATE。仅在下发值不同时记一笔，
     * 便于对账云端策略与板端实际码率的差异。 */
    if(req.max_bitrate_kbps > 0 &&
       req.max_bitrate_kbps * 1000u != UPC_LIVE_H264_BITRATE) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info,
            "live_rtsp cloud max_bitrate=%ukbps ignored — encoder fixed at %ubps",
            req.max_bitrate_kbps, UPC_LIVE_H264_BITRATE);
    }
    H264StreamEncoder::SinkOptions opt{};
    sink_id_ = encoder_->add_sink(
        [this](const uint8_t* annexb, size_t len, bool keyframe, int64_t) {
            return enqueue_packet(annexb, len, keyframe);
        },
        [this] { on_sink_dropped(); },
        opt);
    if(sink_id_ < 0) {
        stop_writer();
        stop_ffmpeg_locked();
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }

    active_ = true;
    rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Accepted);
    rsp.reason[0] = '\0';
    rsp.push_started_at = unix_ms();
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "live_rtsp started sink=%d url=%s",
                          sink_id_, req.push_url);
    return rsp;
}

/**
 * 停止 live 推流。
 * 未在推流时仍返回 Accepted（幂等）。
 */
middleware::RtspPushRspPayload LiveRtspPusher::stop(
    const middleware::RtspPushStopReqPayload& req)
{
    middleware::RtspPushRspPayload rsp{};
    std::snprintf(rsp.req_id, sizeof(rsp.req_id), "%s", req.req_id);
    std::snprintf(rsp.stream_type, sizeof(rsp.stream_type), "%s",
                  req.stream_type[0] ? req.stream_type : "live");

    const std::string stream_type =
        req.stream_type[0] ? std::string(req.stream_type) : "live";
    if(stream_type != "live") {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Rejected);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonInvalidState);
        return rsp;
    }

    teardown_session();

    rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Accepted);
    rsp.reason[0] = '\0';
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "live_rtsp stopped req_id=%s", req.req_id);
    return rsp;
}

/**
 * 结束会话：先摘编码器 sink，再停 writer，最后关 ffmpeg。
 *
 * 三步顺序都不可颠倒，也不可合并到一把锁里：
 * - remove_sink 会等待编码器 worker 的本轮分发结束，而分发中的
 *   enqueue_packet 需要 q_mtx_；若持锁调 remove_sink 就会两边互等。
 * - 摘 sink 返回后才保证不再有新包入队，此时停 writer 才不会漏包。
 * - writer 可能正卡在 ffmpeg_stdin_fd_ 的 poll 上，必须等它退出再关 fd。
 */
void LiveRtspPusher::teardown_session()
{
    H264StreamEncoder* encoder = nullptr;
    int sink_id = -1;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        encoder = encoder_;
        sink_id = sink_id_;
        sink_id_ = -1;
        active_ = false;
    }
    if(encoder && sink_id >= 0) {
        encoder->remove_sink(sink_id);
    }
    stop_writer();
    std::lock_guard<std::mutex> lock(mtx_);
    stop_ffmpeg_locked();
}

/**
 * 编码器 sink 回调：把一个 Annex-B 包放进发送队列。
 *
 * 只做拷贝入队，不碰 write()——编码器 worker 串行调用各 sink，这里一旦阻塞
 * 就会连带冻结本地 RTSP 那一路。返回 false（管道已断）后编码器会摘掉本路
 * 并回调 on_sink_dropped。
 */
bool LiveRtspPusher::enqueue_packet(const uint8_t* data, size_t len, bool keyframe)
{
    if(pipe_broken_.load()) {
        return false;
    }
    if(!data || len == 0) {
        return true;
    }

    std::lock_guard<std::mutex> lock(q_mtx_);
    if(writer_fd_ < 0) {
        return false;
    }

    /* 积压恢复期：整段丢到下一个 IDR。丢单包会给 ffmpeg 送去残缺 NAL，
     * 反而污染码流；GOP=1s，最多丢 1s 画面就能干净地续上。 */
    if(drop_until_idr_) {
        if(!keyframe) {
            ++stat_dropped_pkts_;
            return true;
        }
        drop_until_idr_ = false;
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "live_rtsp resumed at IDR dropped=%llu",
                              static_cast<unsigned long long>(stat_dropped_pkts_));
    }

    if(q_bytes_ + len > UPC_LIVE_PUSH_QUEUE_MAX_BYTES) {
        send_q_.clear();
        q_bytes_ = 0;
        drop_until_idr_ = true;
        ++stat_drop_events_;
        ++stat_dropped_pkts_;
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn,
            "live_rtsp send queue full (%u bytes) — dropping to next IDR events=%u",
            UPC_LIVE_PUSH_QUEUE_MAX_BYTES, stat_drop_events_);
        return true;
    }

    send_q_.emplace_back(data, data + len);
    q_bytes_ += len;
    ++stat_pkts_in_;
    q_cv_.notify_one();
    return true;
}

/**
 * writer 线程主体：取队首包写进管道。
 *
 * 用 wait_for 而非 wait，保证队列长期为空时也能按周期打心跳。
 * 管道故障后不退出循环（留给 stop_writer 统一 join），只清空队列并置
 * pipe_broken_，由下一次入队把 sink 摘掉。
 */
void LiveRtspPusher::writer_fn()
{
    for(;;) {
        std::vector<uint8_t> pkt;
        int fd = -1;
        {
            std::unique_lock<std::mutex> lock(q_mtx_);
            q_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return writer_stop_.load() || !send_q_.empty();
            });
            maybe_log_stats_locked();
            if(writer_stop_.load()) {
                break;
            }
            if(send_q_.empty() || pipe_broken_.load()) {
                continue;
            }
            pkt.swap(send_q_.front());
            send_q_.pop_front();
            q_bytes_ -= pkt.size();
            fd = writer_fd_;
        }
        if(fd < 0 || pkt.empty()) {
            continue;
        }

        if(!write_packet(fd, pkt.data(), pkt.size())) {
            pipe_broken_.store(true);
            std::lock_guard<std::mutex> lock(q_mtx_);
            send_q_.clear();
            q_bytes_ = 0;
            UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn,
                "live_rtsp writer pipe broken err=%s — session will stop",
                std::strerror(errno));
            continue;
        }

        std::lock_guard<std::mutex> lock(q_mtx_);
        ++stat_pkts_out_;
        stat_bytes_out_ += pkt.size();
    }
}

/**
 * 把一个完整包写进管道。
 *
 * 管道是 O_NONBLOCK 的：写满时 poll 等可写再续写，超时回到循环顶部
 * 复查停止位，因此既不会永久阻塞、也不会把包写成半截。
 */
bool LiveRtspPusher::write_packet(int fd, const uint8_t* data, size_t len)
{
    size_t off = 0;
    while(off < len) {
        if(writer_stop_.load()) {
            /* 停止中：剩余数据丢弃，但会话即将拆除，无需报错。 */
            return true;
        }
        const ssize_t wr = ::write(fd, data + off, len - off);
        if(wr > 0) {
            off += static_cast<size_t>(wr);
            continue;
        }
        if(wr < 0 && errno == EINTR) {
            continue;
        }
        if(wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, 100);
            if(pr < 0) {
                if(errno == EINTR) continue;
                return false;
            }
            if(pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

/** 清空队列、置 writer_fd_ 并拉起 writer 线程；调用前必须持有 mtx_。 */
void LiveRtspPusher::start_writer_locked()
{
    stop_writer();
    {
        std::lock_guard<std::mutex> lock(q_mtx_);
        send_q_.clear();
        q_bytes_ = 0;
        drop_until_idr_ = false;
        writer_fd_ = ffmpeg_stdin_fd_;
        stat_pkts_in_ = 0;
        stat_pkts_out_ = 0;
        stat_bytes_out_ = 0;
        stat_dropped_pkts_ = 0;
        stat_drop_events_ = 0;
        stat_last_log_us_ = 0;
        stat_last_out_ = 0;
        stat_last_bytes_ = 0;
    }
    pipe_broken_.store(false);
    writer_stop_.store(false);
    writer_ = std::thread(&LiveRtspPusher::writer_fn, this);
    writer_started_ = true;
}

/**
 * 停 writer 线程。
 * 必须在关闭 ffmpeg_stdin_fd_ 之前调用：writer 可能正卡在该 fd 的 poll 上，
 * 先关 fd 会让它 poll 到一个已回收（甚至被复用）的描述符。
 */
void LiveRtspPusher::stop_writer()
{
    if(!writer_started_) {
        return;
    }
    writer_stop_.store(true);
    q_cv_.notify_all();
    if(writer_.joinable()) {
        writer_.join();
    }
    writer_started_ = false;
    std::lock_guard<std::mutex> lock(q_mtx_);
    writer_fd_ = -1;
    send_q_.clear();
    q_bytes_ = 0;
}

/**
 * 每 5 秒打一行发送侧心跳。
 *
 * queue 持续接近上限说明云端吃不下这个码率；out 不涨而 in 在涨说明
 * 写端卡住了。调用前必须持有 q_mtx_。
 *
 * 由 UPC_MEDIA_LOG_STREAM_STAT 控制；关闭时本函数为空，计数照常累加。
 */
void LiveRtspPusher::maybe_log_stats_locked()
{
#if UPC_MEDIA_LOG_STREAM_STAT
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if(stat_last_log_us_ == 0) {
        stat_last_log_us_ = now;
        return;
    }
    const int64_t elapsed = now - stat_last_log_us_;
    if(elapsed < kStatsPeriodUs) {
        return;
    }
    /* 全程无数据时不刷屏：会话在但相机没送帧属于正常静默。 */
    if(stat_pkts_out_ == stat_last_out_ && send_q_.empty() && stat_pkts_in_ == 0) {
        stat_last_log_us_ = now;
        return;
    }

    const double secs = static_cast<double>(elapsed) / 1000000.0;
    const uint64_t d_out = stat_pkts_out_ - stat_last_out_;
    const uint64_t d_bytes = stat_bytes_out_ - stat_last_bytes_;
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info,
        "live_rtsp stat in=%llu out=%llu(%.1f/s) kbps=%.0f "
        "queue=%zu/%zuB dropped=%llu drop_events=%u",
        static_cast<unsigned long long>(stat_pkts_in_),
        static_cast<unsigned long long>(stat_pkts_out_),
        static_cast<double>(d_out) / secs,
        static_cast<double>(d_bytes) * 8.0 / secs / 1000.0,
        send_q_.size(), q_bytes_,
        static_cast<unsigned long long>(stat_dropped_pkts_),
        stat_drop_events_);

    stat_last_log_us_ = now;
    stat_last_out_ = stat_pkts_out_;
    stat_last_bytes_ = stat_bytes_out_;
#endif /* UPC_MEDIA_LOG_STREAM_STAT */
}

/**
 * 编码器已摘除本路 sink（投递失败或编码器故障）。
 * 此时无需再调 remove_sink，直接关 ffmpeg 结束会话。
 */
void LiveRtspPusher::on_sink_dropped()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ && sink_id_ < 0) {
            return;
        }
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "live_rtsp sink dropped — stopping session");
        active_ = false;
        sink_id_ = -1;
    }
    /* 与 teardown_session 同理：join writer 必须在关 fd 之前、且不持 mtx_。 */
    stop_writer();
    std::lock_guard<std::mutex> lock(mtx_);
    stop_ffmpeg_locked();
}

/**
 * 启动 ffmpeg 子进程做 RTSP 复用推流。
 *
 * 管道：父写 Annex-B → 子 stdin；子进程：
 *   -f h264 -i pipe:0 -c copy -f rtsp -rtsp_transport tcp <push_url>
 * stdout/stderr 重定向到 /dev/null，避免刷屏占满父进程无关 fd。
 * 调用前必须持有 mtx_。
 */
bool LiveRtspPusher::start_ffmpeg_locked(const char* push_url)
{
    if(!push_url || !push_url[0]) {
        return false;
    }
    stop_ffmpeg_locked();

    int pipefd[2] = {-1, -1};
    if(::pipe(pipefd) != 0) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "live_rtsp pipe fail err=%s",
                              std::strerror(errno));
        return false;
    }

    const pid_t pid = ::fork();
    if(pid < 0) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Error, "live_rtsp fork fail err=%s",
                              std::strerror(errno));
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }
    if(pid == 0) {
        ::close(pipefd[1]);
        if(::dup2(pipefd[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        ::close(pipefd[0]);
        /* 丢掉 ffmpeg 噪声，避免占满父进程无关 fd */
        int null_fd = ::open("/dev/null", O_WRONLY);
        if(null_fd >= 0) {
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            ::close(null_fd);
        }
        ::execl(CAMERA_FFMPEG_BIN, CAMERA_FFMPEG_BIN,
                "-loglevel", "error",
                "-fflags", "+genpts",
                "-f", "h264",
                "-i", "pipe:0",
                "-c", "copy",
                "-f", "rtsp",
                "-rtsp_transport", "tcp",
                push_url,
                static_cast<char*>(nullptr));
        _exit(127);
    }

    ::close(pipefd[0]);
    /* 写端设非阻塞：writer 线程靠 EAGAIN+poll 让出，避免 ffmpeg 读得慢时
     * 长期卡死在 write 上（子进程侧 stdin 仍是阻塞的，不受影响）。 */
    const int flags = ::fcntl(pipefd[1], F_GETFL, 0);
    if(flags < 0 || ::fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) < 0) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "live_rtsp set O_NONBLOCK fail err=%s",
                              std::strerror(errno));
    }
    ffmpeg_stdin_fd_ = pipefd[1];
    ffmpeg_pid_ = pid;
    UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "live_rtsp ffmpeg pid=%d", static_cast<int>(pid));
    return true;
}

/**
 * 停止 ffmpeg：先关 stdin（促使子进程读 EOF），再 SIGTERM 等待，
 * 超时 SIGKILL。调用前必须持有 mtx_。
 */
void LiveRtspPusher::stop_ffmpeg_locked()
{
    if(ffmpeg_stdin_fd_ >= 0) {
        ::close(ffmpeg_stdin_fd_);
        ffmpeg_stdin_fd_ = -1;
    }
    if(ffmpeg_pid_ > 0) {
        (void)::kill(ffmpeg_pid_, SIGTERM);
        int status = 0;
        for(int i = 0; i < 50; ++i) {
            const pid_t w = ::waitpid(ffmpeg_pid_, &status, WNOHANG);
            if(w == ffmpeg_pid_ || (w < 0 && errno == ECHILD)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(::waitpid(ffmpeg_pid_, &status, WNOHANG) == 0) {
            (void)::kill(ffmpeg_pid_, SIGKILL);
            (void)::waitpid(ffmpeg_pid_, &status, 0);
        }
        ffmpeg_pid_ = -1;
    }
}

} // namespace upc::media
