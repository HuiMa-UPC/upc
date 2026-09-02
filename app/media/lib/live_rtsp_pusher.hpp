#ifndef LIVE_RTSP_PUSHER_HPP
#define LIVE_RTSP_PUSHER_HPP

#include "h264_stream_encoder.hpp"
#include "upc_contract.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>

/** 发送队列上限（字节）：超出则丢弃到下一个 IDR。约合默认码率下 3s 码流。 */
#ifndef UPC_LIVE_PUSH_QUEUE_MAX_BYTES
#define UPC_LIVE_PUSH_QUEUE_MAX_BYTES (1024u * 1024u)
#endif

namespace upc::media {

/**
 * 云端 live RTSP 推流器。
 *
 * 数据路径：
 *   H264StreamEncoder（共用编码器）
 *     → 本类注册的 sink 回调（只入队，立即返回）
 *     → 发送队列 → writer 线程 write(ffmpeg stdin)
 *     → ffmpeg -c copy -f rtsp -rtsp_transport tcp → push_url
 *
 * 设计约束：
 * - 不摘 Camera peer；编码由 H264StreamEncoder 统一完成，与本地 RTSP Server 共用一路码流。
 * - 本类只负责 ffmpeg 子进程与云端契约语义（RtspPushRspPayload），不碰 RKMPP。
 * - 一期仅支持 stream_type="live"；push_url 由云端下发（含 token），本模块原样推送。
 * - **sink 回调绝不阻塞**：编码器 worker 串行调用各 sink，若本路直接阻塞在
 *   write() 上（云端网络不畅时管道会填满），会连带冻结本地 RTSP 那一路。
 *   故写操作全部下沉到 writer 线程，管道设 O_NONBLOCK + poll 等待可写。
 * - 队列积压超上限时整段丢弃到下一个 IDR，而非丢单个包：给 ffmpeg 送残缺
 *   NAL 会直接破坏码流，丢掉半个 GOP（≤1s）反而能干净地恢复。
 */
class LiveRtspPusher {
public:
    LiveRtspPusher() = default;
    ~LiveRtspPusher();

    LiveRtspPusher(const LiveRtspPusher&) = delete;
    LiveRtspPusher& operator=(const LiveRtspPusher&) = delete;

    /** 绑定共用编码器；必须在首次 start() 之前由 MediaService 注入。 */
    void set_encoder(H264StreamEncoder* encoder);

    /**
     * 启动推流。
     * 同步返回：pipeline（ffmpeg + 编码器 sink）拉起即 Accepted；
     * 已在推 / 非 live / 几何未知 / 编码器失败等分别 Rejected 或 Error。
     */
    middleware::RtspPushRspPayload start(const middleware::RtspPushStartReqPayload& req);

    /**
     * 停止 live 推流。
     * 未在推也 Accepted；会摘掉编码器 sink 并关闭 ffmpeg。
     */
    middleware::RtspPushRspPayload stop(const middleware::RtspPushStopReqPayload& req);

    /** 当前是否处于推流会话中。 */
    [[nodiscard]] bool active() const;

private:
    /** 编码器 sink 回调：把 Annex-B 包放进发送队列，绝不阻塞编码 worker。 */
    bool enqueue_packet(const uint8_t* data, size_t len, bool keyframe);
    /** 编码器摘除本路 sink 后的通知：停 writer、关 ffmpeg 并结束会话。 */
    void on_sink_dropped();
    /** 先摘 sink，再停 writer，最后关 ffmpeg（顺序不可颠倒，见实现注释）。 */
    void teardown_session();
    /** writer 线程主体：取队首包写入管道，兼做发送侧心跳统计。 */
    void writer_fn();
    /**
     * 把一个完整包写进管道；EAGAIN 时 poll 等可写后续写。
     * @return false 表示管道已不可用（EPIPE/POLLHUP 等）。
     */
    bool write_packet(int fd, const uint8_t* data, size_t len);
    /** 清空队列、置 writer_fd_ 并拉起 writer 线程；调用前必须持有 mtx_。 */
    void start_writer_locked();
    /** 置停止位并 join writer 线程；调用时**不得**持有 q_mtx_。 */
    void stop_writer();
    /** 距上次心跳满周期则打一行；调用前必须持有 q_mtx_。 */
    void maybe_log_stats_locked();
    /** fork+execl ffmpeg：stdin 收 Annex-B，copy 推 RTSP；调用前必须持有 mtx_。 */
    bool start_ffmpeg_locked(const char* push_url);
    /** 关 stdin、SIGTERM/SIGKILL 等 ffmpeg 退出；调用前必须持有 mtx_。 */
    void stop_ffmpeg_locked();
    /** 墙钟毫秒，写入 push_started_at。 */
    static uint64_t unix_ms();

    mutable std::mutex mtx_;              /**< 保护会话状态与 ffmpeg 句柄。 */
    H264StreamEncoder* encoder_ = nullptr;
    int sink_id_ = -1;                    /**< 编码器 sink 句柄；<0 表示未注册。 */

    bool active_ = false;                 /**< 推流会话是否活跃。 */
    int ffmpeg_stdin_fd_ = -1;            /**< ffmpeg 写端；<0 表示未推流。 */
    pid_t ffmpeg_pid_ = -1;               /**< ffmpeg 子进程。 */

    /* 发送侧。锁序固定为 mtx_ → q_mtx_；writer 线程只取 q_mtx_，不碰 mtx_。 */
    mutable std::mutex q_mtx_;            /**< 保护发送队列与发送侧统计。 */
    std::condition_variable q_cv_;        /**< 通知 writer：有新包或应停止。 */
    std::thread writer_;                  /**< 发送线程（随会话启停）。 */
    std::deque<std::vector<uint8_t>> send_q_; /**< 待写包队列。 */
    size_t q_bytes_ = 0;                  /**< 队列当前字节数。 */
    int writer_fd_ = -1;                  /**< writer 线程用的管道写端副本。 */
    std::atomic<bool> writer_stop_{false};/**< 置位后 writer 退出循环。 */
    bool writer_started_ = false;         /**< writer 线程是否在运行。 */
    bool drop_until_idr_ = false;         /**< 积压丢弃中，等下一个 IDR 恢复。 */
    std::atomic<bool> pipe_broken_{false};/**< 管道已断，下次入队即摘 sink。 */

    /* 运行期心跳统计。 */
    static constexpr int64_t kStatsPeriodUs = 5000000; /**< 心跳周期 5s。 */
    uint64_t stat_pkts_in_ = 0;           /**< 累计入队包数。 */
    uint64_t stat_pkts_out_ = 0;          /**< 累计写出包数。 */
    uint64_t stat_bytes_out_ = 0;         /**< 累计写出字节数。 */
    uint64_t stat_dropped_pkts_ = 0;      /**< 累计因积压丢弃的包数。 */
    uint32_t stat_drop_events_ = 0;       /**< 累计触发"丢到下个 IDR"的次数。 */
    int64_t  stat_last_log_us_ = 0;       /**< 上次心跳打印时刻；0=尚未打过。 */
    uint64_t stat_last_out_ = 0;          /**< 上次心跳时的 stat_pkts_out_。 */
    uint64_t stat_last_bytes_ = 0;        /**< 上次心跳时的 stat_bytes_out_。 */
};

} // namespace upc::media

#endif /* LIVE_RTSP_PUSHER_HPP */
