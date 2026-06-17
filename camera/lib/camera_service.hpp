#ifndef CAMERA_SERVICE_HPP
#define CAMERA_SERVICE_HPP

#include "device_config.hpp"
#include "msg_port.hpp"
#include "ipc_shm_ring.hpp"
#include "vi_capture.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

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

    bool start_streaming();
    void stop_streaming();
    void stream_thread_fn();

    void send_stream_start_rsp(const middleware::Message& req, bool ok);
    void send_stream_stop_done(const middleware::Message& req);
    void notify_frame(uint32_t seq);

    ViCaptureConfig build_capture_config();
    static uint32_t read_fps_from_env();

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcShmRingProducer producer_;
    ViCapture capture_;

    std::thread stream_thread_;
    std::atomic<bool> stream_running_{false};
    std::atomic<bool> stop_stream_{false};
    bool frame_ntf_enabled_ = (CAMERA_FRAME_NTF_DEFAULT != 0);

    uint32_t stream_width_  = CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t stream_fps_    = CAMERA_FPS_DEFAULT;
    uint32_t shm_stride_y_  = CAMERA_WIDTH_DEFAULT;
};

} // namespace upc::camera

#endif /* CAMERA_SERVICE_HPP */
