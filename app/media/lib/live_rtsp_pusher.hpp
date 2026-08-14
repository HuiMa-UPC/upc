#ifndef LIVE_RTSP_PUSHER_HPP
#define LIVE_RTSP_PUSHER_HPP

#include "device_config.hpp"
#include "upc_contract.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace upc::media {

/**
 * 云端 live RTSP 推流器。
 *
 * 数据路径：
 *   Media DMA 消费线程 on_frame_nv12()
 *     → 单槽 pending_nv12_（depth=1，latest-wins；积压则丢旧帧）
 *     → worker：RKMPP 编 Annex-B H.264
 *     → write(ffmpeg stdin) → ffmpeg -c copy -f rtsp -rtsp_transport tcp → push_url
 *
 * 设计约束：
 * - 不摘 Camera peer；与 PrintH264Recorder 独立会话、独立 RKMPP 编码器实例。
 * - 采帧路径只做紧凑 NV12 拷贝，编码与写盘在 worker，避免拖慢 DMA 环。
 * - 一期仅支持 stream_type="live"；push_url 由云端下发（含 token），本模块原样推送。
 */
class LiveRtspPusher {
public:
    LiveRtspPusher() = default;
    ~LiveRtspPusher();

    LiveRtspPusher(const LiveRtspPusher&) = delete;
    LiveRtspPusher& operator=(const LiveRtspPusher&) = delete;

    /** 设置编码分辨率/帧率；任一项为 0 时保持当前值。开流后由 MediaService 同步。 */
    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);

    /**
     * 启动推流。
     * 同步返回：pipeline（编码器 + ffmpeg）拉起即 Accepted；
     * 已在推 / 非 live / 几何未知 / 编码器失败等分别 Rejected 或 Error。
     */
    middleware::RtspPushRspPayload start(const middleware::RtspPushStartReqPayload& req);

    /**
     * 停止 live 推流。
     * 未在推也 Accepted；会关 ffmpeg、冲刷并销毁 RKMPP。
     */
    middleware::RtspPushRspPayload stop(const middleware::RtspPushStopReqPayload& req);

    /**
     * 每帧 NV12 入口（DMA 消费线程调用）。
     * 活跃时拷贝入单槽 pending；若上一帧尚未被 worker 取走则覆盖并 ++dropped_。
     */
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);

    /** 当前是否处于推流会话中。 */
    [[nodiscard]] bool active() const;

private:
    /** join worker 并清 worker_started_；调用前须已置 worker_stop_ 并 notify。 */
    void stop_worker();
    /** 后台主循环：取 pending → encode_and_write；失败则自停会话。 */
    void worker_fn();
    /** 打开/复用 RKMPP H.264 编码器；调用前必须持有 mtx_。 */
    bool open_encoder_locked();
    /** 冲刷 EOS 并销毁 RKMPP；调用前必须持有 mtx_。 */
    void close_encoder_locked();
    /** fork+execl ffmpeg：stdin 收 Annex-B，copy 推 RTSP；调用前必须持有 mtx_。 */
    bool start_ffmpeg_locked(const char* push_url);
    /** 关 stdin、SIGTERM/SIGKILL 等 ffmpeg 退出；调用前必须持有 mtx_。 */
    void stop_ffmpeg_locked();
    /**
     * 编码一帧并写入 ffmpeg stdin。
     * 锁内只取句柄，锁外做 MPP put/get 与 write，缩短持锁时间。
     */
    bool encode_and_write(const std::vector<uint8_t>& nv12, uint32_t w, uint32_t h);
    /** 将带 stride 的 NV12 拷成紧凑布局（Y 平面后接 UV）。 */
    static void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                const uint8_t* src_uv, uint32_t src_stride_y,
                                uint32_t width, uint32_t height);
    /** 墙钟毫秒，写入 push_started_at。 */
    static uint64_t unix_ms();

    mutable std::mutex mtx_;              /**< 保护会话状态、pending、MPP/ffmpeg 句柄。 */
    std::condition_variable cv_;          /**< 通知 worker：有新帧或应停止。 */
    std::thread worker_;                  /**< 编码+写 stdin 后台线程。 */
    bool worker_stop_ = false;            /**< 析构时置位，worker 退出循环。 */
    bool worker_started_ = false;         /**< worker 是否已启动（进程内复用同一线程）。 */

    bool active_ = false;                 /**< 推流会话是否活跃。 */
    uint32_t width_ = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_ = CAMERA_FPS_DEFAULT;
    uint32_t bitrate_bps_ = CAMERA_PRINT_H264_BITRATE; /**< 目标码率；可由 max_bitrate_kbps 覆盖。 */

    bool has_pending_ = false;            /**< pending_nv12_ 是否有待编码帧。 */
    std::vector<uint8_t> pending_nv12_;   /**< 单槽最新帧（紧凑 NV12）。 */
    uint32_t pending_w_ = 0;
    uint32_t pending_h_ = 0;
    uint32_t dropped_ = 0;                /**< 因单槽覆盖而丢弃的旧帧计数。 */

    int ffmpeg_stdin_fd_ = -1;            /**< ffmpeg 写端；<0 表示未推流。 */
    pid_t ffmpeg_pid_ = -1;               /**< ffmpeg 子进程。 */

    /* RKMPP 用 void* 封装，避免头文件依赖 rk_mpi.h。 */
    void* mpp_ctx_ = nullptr;
    void* mpp_mpi_ = nullptr;
    void* mpp_cfg_ = nullptr;
    bool mpp_ready_ = false;
};

} // namespace upc::media

#endif /* LIVE_RTSP_PUSHER_HPP */
