#ifndef TEST_CAMERA_SERVICE_HPP
#define TEST_CAMERA_SERVICE_HPP

#include "msg_port.hpp"
#include "ipc_shm_ring.hpp"
#include "camera_test_config.hpp"
#include "file_frame_source.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>
class TestCameraService {
public:
    TestCameraService();
    ~TestCameraService();

    TestCameraService(const TestCameraService&) = delete;
    TestCameraService& operator=(const TestCameraService&) = delete;

    bool init();
    void shutdown();

private:
    void subscribe_messages();
    void handle_message(const middleware::Message& msg);
    void handle_stream_start(const middleware::Message& msg);
    void handle_stream_stop(const middleware::Message& msg);

    bool start_streaming(const std::string& video_path);
    void stop_streaming();
    void stream_thread_fn();

    void send_stream_start_rsp(const middleware::Message& req, bool ok);
    void send_stream_stop_done(const middleware::Message& req);
    void notify_frame(uint32_t seq);

    static std::string resolve_video_path(const middleware::MsgPayload& payload);
    static uint32_t read_fps_from_env();

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcShmRingProducer producer_;
    FileFrameSource source_;

    std::thread stream_thread_;
    std::atomic<bool> stream_running_{false};
    std::atomic<bool> stop_stream_{false};
    bool loop_video_ = true;
    uint32_t stream_width_  = TEST_CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_ = TEST_CAMERA_HEIGHT_DEFAULT;
    uint32_t stream_fps_    = TEST_CAMERA_FPS_DEFAULT;
};

#endif /* TEST_CAMERA_SERVICE_HPP */
