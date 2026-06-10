#include "test_camera_service.hpp"
#include "file_frame_source.hpp"
#include "camera_test_config.hpp"
#include "ipc_shm_region.hpp"
#include "msg_port.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

uint64_t monotonic_us()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now().time_since_epoch())
            .count());
}

bool read_start_req_name(const middleware::MsgPayload& payload, char* out, size_t out_size)
{
    if(!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if(payload.type != middleware::MsgPayload::Type::Inline ||
       payload.length < sizeof(middleware::CameraStreamStartReqPayload)) {
        return true;
    }
    const auto* req = reinterpret_cast<const middleware::CameraStreamStartReqPayload*>(
        payload.data.bytes.data());
    std::snprintf(out, out_size, "%s", req->video_name);
    return true;
}

} // namespace

TestCameraService::TestCameraService() = default;

TestCameraService::~TestCameraService()
{
    shutdown();
}

bool TestCameraService::init()
{
    const char* loop_env = std::getenv(TEST_CAMERA_ENV_LOOP);
    if(loop_env && loop_env[0] == '0') {
        loop_video_ = false;
    }

    stream_fps_ = read_fps_from_env();
    subscribe_messages();
    UPC_LOG_TEST_CAMERA(Info, "init loop=%d fps=%u",
                        loop_video_ ? 1 : 0, stream_fps_);
    return true;
}

void TestCameraService::shutdown()
{
    stop_streaming();
    subs_.clear();
}

void TestCameraService::subscribe_messages()
{
    auto& port = middleware::MsgPort::instance();

    subs_.push_back(port.subscribe(
        middleware::MsgType::UI_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::UI_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
}

void TestCameraService::handle_message(const middleware::Message& msg)
{
    switch(msg.type) {
    case middleware::MsgType::UI_Camera_Stream_Start_Req:
        handle_stream_start(msg);
        break;
    case middleware::MsgType::UI_Camera_Stream_Stop_Req:
        handle_stream_stop(msg);
        break;
    default:
        break;
    }
}

std::string TestCameraService::resolve_video_path(const middleware::MsgPayload& payload)
{
    const char* env_path = std::getenv(TEST_CAMERA_ENV_VIDEO_PATH);
    if(env_path && env_path[0]) {
        return std::string(env_path);
    }

    char name[sizeof(middleware::CameraStreamStartReqPayload::video_name)] = {};
    read_start_req_name(payload, name, sizeof(name));

    if(name[0]) {
        return std::string(TEST_CAMERA_VIDEO_DIR_DEFAULT) + "/" + name;
    }

    return std::string(TEST_CAMERA_VIDEO_DIR_DEFAULT) + "/" +
           TEST_CAMERA_VIDEO_NAME_DEFAULT;
}

uint32_t TestCameraService::read_fps_from_env()
{
    const char* env = std::getenv(TEST_CAMERA_ENV_FPS);
    if(!env || !env[0]) {
        return TEST_CAMERA_FPS_DEFAULT;
    }
    const long v = std::strtol(env, nullptr, 10);
    if(v <= 0 || v > 120) {
        return TEST_CAMERA_FPS_DEFAULT;
    }
    return static_cast<uint32_t>(v);
}

void TestCameraService::handle_stream_start(const middleware::Message& msg)
{
    if(stream_running_.load()) {
        stop_streaming();
    }

    const std::string path = resolve_video_path(msg.payload);
    const bool ok          = start_streaming(path);
    send_stream_start_rsp(msg, ok);

    if(!ok) {
        UPC_LOG_TEST_CAMERA(Error, "stream start failed path=%s", path.c_str());
    } else {
        UPC_LOG_TEST_CAMERA(Info,
            "streaming %s %ux%u @ %u fps shm=%s",
            path.c_str(), stream_width_, stream_height_, stream_fps_,
            TEST_CAMERA_SHM_NAME_DEFAULT);
    }
}

void TestCameraService::handle_stream_stop(const middleware::Message& msg)
{
    stop_streaming();
    send_stream_stop_done(msg);
    UPC_LOG_TEST_CAMERA(Info, "stream stopped");
}

bool TestCameraService::start_streaming(const std::string& video_path)
{
    if(!source_.open(video_path, stream_width_, stream_height_)) {
        UPC_LOG_TEST_CAMERA(Error, "cannot open video: %s", video_path.c_str());
        return false;
    }

    producer_.destroy();

    const upc_ipc::IpcShmRingCreateParams params{
        TEST_CAMERA_SHM_NAME_DEFAULT,
        stream_width_,
        stream_height_,
        0,
        TEST_CAMERA_SHM_SLOT_COUNT,
        upc_ipc::IpcShmPixelFormat::YUV420SP,
    };

    upc_ipc::IpcShmError err = producer_.create(params);
    if(err == upc_ipc::IpcShmError::AlreadyExists) {
        upc_ipc::IpcShmRegion tmp;
        if(tmp.open(TEST_CAMERA_SHM_NAME_DEFAULT, false) == upc_ipc::IpcShmError::Ok) {
            tmp.unlink_name();
            tmp.close();
        }
        err = producer_.create(params);
    }
    if(err != upc_ipc::IpcShmError::Ok) {
        source_.close();
        UPC_LOG_TEST_CAMERA(Error, "shm create failed err=%d",
                            static_cast<int>(err));
        return false;
    }

    stop_stream_.store(false);
    stream_running_.store(true);
    stream_thread_ = std::thread(&TestCameraService::stream_thread_fn, this);
    return true;
}

void TestCameraService::stop_streaming()
{
    stop_stream_.store(true);
    if(stream_thread_.joinable()) {
        stream_thread_.join();
    }
    stream_running_.store(false);
    producer_.destroy();
    source_.close();
}

void TestCameraService::stream_thread_fn()
{
    const size_t frame_bytes = source_.frame_bytes();
    const auto frame_period =
        std::chrono::microseconds(1000000u / (stream_fps_ > 0 ? stream_fps_ : 1u));

    using clock = std::chrono::steady_clock;
    auto stats_since   = clock::now();
    uint32_t frames_1s = 0;
    uint32_t last_seq  = 0;
    uint32_t last_slot = 0;

    while(!stop_stream_.load(std::memory_order_relaxed)) {
        const auto now = clock::now();
        if(now - stats_since >= std::chrono::seconds(1)) {
            UPC_LOG_TEST_CAMERA(Debug,
                "stream 1s: committed=%u last_seq=%u last_slot=%u bytes=%zu",
                frames_1s, last_seq, last_slot, frame_bytes);
            frames_1s  = 0;
            stats_since = now;
        }

        const auto tick_start = clock::now();

        std::optional<uint32_t> slot = producer_.acquire_write_slot();
        if(!slot) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        uint8_t* payload = producer_.slot_payload(*slot);
        if(!payload ||
           !source_.read_frame(payload, frame_bytes, loop_video_)) {
            producer_.cancel_write_slot(*slot);
            if(stop_stream_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const uint32_t seq =
            producer_.commit_slot(*slot, monotonic_us());
        notify_frame(seq);
        frames_1s++;
        last_seq  = seq;
        last_slot = *slot;

        const auto elapsed = clock::now() - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }
}

void TestCameraService::send_stream_start_rsp(const middleware::Message& req, bool ok)
{
    middleware::CameraStreamInfoPayload info{};
    std::snprintf(info.shm_name, sizeof(info.shm_name), "%s",
                  TEST_CAMERA_SHM_NAME_DEFAULT);
    info.width    = stream_width_;
    info.height   = stream_height_;
    info.stride_y = 0;
    info.format   = static_cast<uint32_t>(upc_ipc::IpcShmPixelFormat::YUV420SP);
    info.fps      = stream_fps_;
    info.ok       = ok ? 1 : 0;

    middleware::Message rsp(
        middleware::MsgType::APP_Camera_Stream_Start_Rsp,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&info), sizeof(info));
    middleware::MsgPort::instance().publish_async(rsp);
}

void TestCameraService::send_stream_stop_done(const middleware::Message& req)
{
    middleware::Message done(
        middleware::MsgType::APP_Camera_Stream_Stop_Done,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    middleware::MsgPort::instance().publish_async(done);
}

void TestCameraService::notify_frame(uint32_t seq)
{
    middleware::CameraFrameNtfPayload ntf{};
    ntf.seq = seq;

    middleware::Message msg(
        middleware::MsgType::APP_Camera_Frame_Ntf,
        middleware::ModuleId::APP_CAMERA,
        middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    middleware::MsgPort::instance().publish_async(msg);
}
