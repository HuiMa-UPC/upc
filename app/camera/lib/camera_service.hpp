#ifndef CAMERA_SERVICE_HPP
#define CAMERA_SERVICE_HPP

#include "camera_dma_frame_source.hpp"
#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "vi_capture.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
    void handle_visual_stream_start(const middleware::Message& msg);
    void handle_visual_stream_stop(const middleware::Message& msg);
    void handle_media_stream_start(const middleware::Message& msg);
    void handle_media_stream_stop(const middleware::Message& msg);

    bool start_streaming();
    void stop_streaming();
    bool ensure_stream_peer(const middleware::Message& req,
                            const char* shm_name,
                            middleware::MsgType rsp_type);
    void remove_stream_peer(middleware::ModuleId peer);
    bool has_stream_peers() const;
    void stream_thread_fn();
    void update_latest_nv12_snapshot(const uint8_t* src_y, const uint8_t* src_uv,
                                     uint32_t src_stride_y, uint32_t width,
                                     uint32_t height);
    bool copy_latest_nv12_snapshot(std::vector<uint8_t>& out) const;

    void send_stream_start_rsp(const middleware::Message& req, bool ok);
    void send_stream_stop_done(const middleware::Message& req);
    void send_stream_start_rsp_with_type(const middleware::Message& req, bool ok,
                                         middleware::MsgType rsp_type,
                                         const char* shm_name);
    void send_stream_stop_done_with_type(const middleware::Message& req,
                                         middleware::MsgType done_type);
    void notify_frame(uint32_t seq);

    ViCaptureConfig build_capture_config();
    static uint32_t read_fps_from_env();
    static uint32_t read_warmup_frames_from_env();

    std::vector<middleware::Subscription> subs_;
    struct StreamPeerSession {
        middleware::ModuleId peer_id{middleware::ModuleId{0}};
        std::string shm_name;
        upc_ipc::IpcDmaFrameRingProducer producer;
    };

    mutable std::mutex stream_peers_mtx_;
    std::unordered_map<uint32_t, std::shared_ptr<StreamPeerSession>> stream_peers_;

    ViCapture capture_;
    CameraDmaFrameSource dma_source_;
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
