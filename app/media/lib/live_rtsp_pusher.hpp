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
 * 云端 live RTSP 推流：
 * - DMA 消费线程只拷贝最新 NV12 入队（depth=1，latest-wins）
 * - worker：RKMPP 编 Annex-B → ffmpeg stdin（-c copy -f rtsp -rtsp_transport tcp）
 * - 不摘 Camera peer；与 PrintH264Recorder 独立会话/独立编码器实例
 */
class LiveRtspPusher {
public:
    LiveRtspPusher() = default;
    ~LiveRtspPusher();

    LiveRtspPusher(const LiveRtspPusher&) = delete;
    LiveRtspPusher& operator=(const LiveRtspPusher&) = delete;

    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);

    /** 启动推流；同步返回结果（pipeline 拉起即 Accepted）。 */
    middleware::RtspPushRspPayload start(const middleware::RtspPushStartReqPayload& req);

    /** 停止 live 推流；未在推也 Accepted。 */
    middleware::RtspPushRspPayload stop(const middleware::RtspPushStopReqPayload& req);

    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);

    [[nodiscard]] bool active() const;

private:
    void stop_worker();
    void worker_fn();
    bool open_encoder_locked();
    void close_encoder_locked();
    bool start_ffmpeg_locked(const char* push_url);
    void stop_ffmpeg_locked();
    bool encode_and_write(const std::vector<uint8_t>& nv12, uint32_t w, uint32_t h);
    static void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                const uint8_t* src_uv, uint32_t src_stride_y,
                                uint32_t width, uint32_t height);
    static uint64_t unix_ms();

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool worker_stop_ = false;
    bool worker_started_ = false;

    bool active_ = false;
    uint32_t width_ = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_ = CAMERA_FPS_DEFAULT;
    uint32_t bitrate_bps_ = CAMERA_PRINT_H264_BITRATE;

    bool has_pending_ = false;
    std::vector<uint8_t> pending_nv12_;
    uint32_t pending_w_ = 0;
    uint32_t pending_h_ = 0;
    uint32_t dropped_ = 0;

    int ffmpeg_stdin_fd_ = -1;
    pid_t ffmpeg_pid_ = -1;

    void* mpp_ctx_ = nullptr;
    void* mpp_mpi_ = nullptr;
    void* mpp_cfg_ = nullptr;
    bool mpp_ready_ = false;
};

} // namespace upc::media

#endif /* LIVE_RTSP_PUSHER_HPP */
