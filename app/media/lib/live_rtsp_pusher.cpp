#include "live_rtsp_pusher.hpp"

#include "device_config.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
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
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp start fail: encoder not bound");
        return rsp;
    }

    stop_ffmpeg_locked();
    if(!start_ffmpeg_locked(req.push_url)) {
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }

    /* 云端 max_bitrate_kbps→bps；0 表示使用板端默认中等码率。
     * 编码器共用时该值仅在本路是首个 sink 时生效。 */
    H264StreamEncoder::SinkOptions opt{};
    opt.desired_bitrate_bps = req.max_bitrate_kbps > 0
        ? req.max_bitrate_kbps * 1000u
        : 0u;
    sink_id_ = encoder_->add_sink(
        [this](const uint8_t* annexb, size_t len, bool) {
            return write_annexb(annexb, len);
        },
        [this] { on_sink_dropped(); },
        opt);
    if(sink_id_ < 0) {
        stop_ffmpeg_locked();
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "%s", kReasonEncoderError);
        return rsp;
    }

    active_ = true;
    rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Accepted);
    rsp.reason[0] = '\0';
    rsp.push_started_at = unix_ms();
    UPC_LOG_MEDIA_SERVICE(Info, "live_rtsp started sink=%d url=%s",
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
    UPC_LOG_MEDIA_SERVICE(Info, "live_rtsp stopped req_id=%s", req.req_id);
    return rsp;
}

/**
 * 结束会话：先摘编码器 sink，再关 ffmpeg。
 *
 * 顺序不可颠倒也不可合并到一把锁里：remove_sink 会等待编码器 worker 的
 * 本轮分发结束，而分发中的 write_annexb 需要 mtx_；若持 mtx_ 调 remove_sink
 * 就会两边互等。摘 sink 返回后 write_annexb 不会再被调用，此时关 fd 才安全。
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
    std::lock_guard<std::mutex> lock(mtx_);
    stop_ffmpeg_locked();
}

/**
 * 编码器 sink 回调：把一个 Annex-B 包写入 ffmpeg stdin。
 *
 * 返回 false（管道断开等）后编码器会摘掉本路并回调 on_sink_dropped。
 * 取 fd 后即解锁：stop 路径保证摘 sink 先于关 fd，故不会写到已关闭的 fd。
 */
bool LiveRtspPusher::write_annexb(const uint8_t* data, size_t len)
{
    int out_fd = -1;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ || ffmpeg_stdin_fd_ < 0) {
            return false;
        }
        out_fd = ffmpeg_stdin_fd_;
    }
    if(!data || len == 0) {
        return true;
    }
    size_t off = 0;
    while(off < len) {
        const ssize_t wr = ::write(out_fd, data + off, len - off);
        if(wr < 0) {
            if(errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(wr);
    }
    return true;
}

/**
 * 编码器已摘除本路 sink（投递失败或编码器故障）。
 * 此时无需再调 remove_sink，直接关 ffmpeg 结束会话。
 */
void LiveRtspPusher::on_sink_dropped()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(!active_ && sink_id_ < 0) {
        return;
    }
    UPC_LOG_MEDIA_SERVICE(Warn, "live_rtsp sink dropped — stopping session");
    active_ = false;
    sink_id_ = -1;
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
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp pipe fail err=%s",
                              std::strerror(errno));
        return false;
    }

    const pid_t pid = ::fork();
    if(pid < 0) {
        UPC_LOG_MEDIA_SERVICE(Error, "live_rtsp fork fail err=%s",
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
    ffmpeg_stdin_fd_ = pipefd[1];
    ffmpeg_pid_ = pid;
    UPC_LOG_MEDIA_SERVICE(Info, "live_rtsp ffmpeg pid=%d", static_cast<int>(pid));
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
