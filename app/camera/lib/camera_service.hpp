#ifndef CAMERA_SERVICE_HPP
#define CAMERA_SERVICE_HPP

#include "camera_dma_frame_source.hpp"
#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "print_video_recorder.hpp"
#include "vi_capture.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cstring>

namespace upc::camera {

class CameraService {
public:
    CameraService() = default;
    ~CameraService();

    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    bool init();
    void shutdown();

private:
    void subscribe_messages();
    void handle_message(const middleware::Message& msg);
    void handle_stream_start(const middleware::Message& msg);
    void handle_stream_stop(const middleware::Message& msg);
    void handle_print_video(const middleware::Message& msg);

    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out)
    {
        if(payload.type != middleware::MsgPayload::Type::Inline ||
           payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, payload.data.bytes.data(), sizeof(T));
        return true;
    }

    bool start_streaming();
    void stop_streaming();
    void stream_thread_fn();
    void update_latest_nv12_snapshot(const uint8_t* src_y, const uint8_t* src_uv,
                                     uint32_t src_stride_y, uint32_t width,
                                     uint32_t height);
    bool copy_latest_nv12_snapshot(std::vector<uint8_t>& out) const;

    void send_stream_start_rsp(const middleware::Message& req, bool ok);
    void send_stream_stop_done(const middleware::Message& req);
    void notify_frame(uint32_t seq);

    ViCaptureConfig build_capture_config();
    static uint32_t read_fps_from_env();
    static uint32_t read_warmup_frames_from_env();

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcDmaFrameRingProducer producer_;
    ViCapture capture_;
    CameraDmaFrameSource dma_source_;
    PrintVideoRecorder print_video_;

    std::thread stream_thread_;
    std::atomic<bool> stream_running_{false};
    std::atomic<bool> stop_stream_{false};
    bool frame_ntf_enabled_ = (CAMERA_FRAME_NTF_DEFAULT != 0);

    uint32_t stream_width_         = CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_        = CAMERA_HEIGHT_DEFAULT;
    uint32_t stream_fps_           = CAMERA_FPS_DEFAULT;
    uint32_t stream_warmup_frames_ = CAMERA_WARMUP_FRAMES_DEFAULT;

    mutable std::mutex latest_nv12_mtx_;
    std::vector<uint8_t> latest_nv12_snapshot_;
    bool latest_nv12_valid_ = false;
};

} // namespace upc::camera

#endif /* CAMERA_SERVICE_HPP */
