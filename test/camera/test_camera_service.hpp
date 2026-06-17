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

/**
 * test_camera 进程核心服务：订阅 UI 的流控消息，从 NV12 文件读帧并写入 SHM ring，
 * 供 UI CameraPreviewPlayer 轮询或（可选）通过 Frame_Ntf 推送序号。
 */
class TestCameraService {
public:
    TestCameraService();
    ~TestCameraService();

    TestCameraService(const TestCameraService&) = delete;
    TestCameraService& operator=(const TestCameraService&) = delete;

    /** 读取环境变量并注册 IPC 订阅 */
    bool init();
    /** 停止推流并释放订阅 */
    void shutdown();

private:
    void subscribe_messages();
    void handle_message(const middleware::Message& msg);
    void handle_stream_start(const middleware::Message& msg);
    void handle_stream_stop(const middleware::Message& msg);

    /** 打开视频、创建 SHM producer 并启动推流线程 */
    bool start_streaming(const std::string& video_path);
    void stop_streaming();
    /** 按 stream_fps_ 节拍 acquire → read_frame → commit，可选 notify_frame */
    void stream_thread_fn();

    /** 回复 UI：携带 shm 名、分辨率与 ok 标志 */
    void send_stream_start_rsp(const middleware::Message& req, bool ok);
    void send_stream_stop_done(const middleware::Message& req);
    /** 向 UI 发布 APP_Camera_Frame_Ntf（仅 seq，帧数据在 SHM） */
    void notify_frame(uint32_t seq);

    /** 路径优先级：环境变量 > StartReq.video_name > 默认 demo.nv12 */
    static std::string resolve_video_path(const middleware::MsgPayload& payload);
    static uint32_t read_fps_from_env();

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcShmRingProducer producer_;
    FileFrameSource source_;

    std::thread stream_thread_;
    std::atomic<bool> stream_running_{false};
    std::atomic<bool> stop_stream_{false};
    bool loop_video_ = true;       /**< 文件读完是否从头循环（UPC_TEST_CAMERA_LOOP） */
    bool frame_ntf_enabled_ =
        (TEST_CAMERA_FRAME_NTF_DEFAULT != 0); /**< 是否每帧发 Frame_Ntf */
    uint32_t stream_width_  = TEST_CAMERA_WIDTH_DEFAULT;
    uint32_t stream_height_ = TEST_CAMERA_HEIGHT_DEFAULT;
    uint32_t stream_fps_    = TEST_CAMERA_FPS_DEFAULT;
};

#endif /* TEST_CAMERA_SERVICE_HPP */
