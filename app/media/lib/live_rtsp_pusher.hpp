#ifndef LIVE_RTSP_PUSHER_HPP
#define LIVE_RTSP_PUSHER_HPP

#include "h264_stream_encoder.hpp"
#include "upc_contract.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>

namespace upc::media {

/**
 * 云端 live RTSP 推流器。
 *
 * 数据路径：
 *   H264StreamEncoder（共用编码器）
 *     → 本类注册的 sink 回调
 *     → write(ffmpeg stdin) → ffmpeg -c copy -f rtsp -rtsp_transport tcp → push_url
 *
 * 设计约束：
 * - 不摘 Camera peer；编码由 H264StreamEncoder 统一完成，与本地 RTSP Server 共用一路码流。
 * - 本类只负责 ffmpeg 子进程与云端契约语义（RtspPushRspPayload），不碰 RKMPP。
 * - 一期仅支持 stream_type="live"；push_url 由云端下发（含 token），本模块原样推送。
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
    /** 编码器 sink 回调：把 Annex-B 写入 ffmpeg stdin，失败返回 false。 */
    bool write_annexb(const uint8_t* data, size_t len);
    /** 编码器摘除本路 sink 后的通知：关 ffmpeg 并结束会话。 */
    void on_sink_dropped();
    /** 先摘 sink 再关 ffmpeg（顺序不可颠倒，见实现注释）。 */
    void teardown_session();
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
};

} // namespace upc::media

#endif /* LIVE_RTSP_PUSHER_HPP */
