#include "visual_service.hpp"

#include "rs_log.hpp"
#include "upc_contract.hpp"

#include <chrono>
#include <cstring>

#include <unistd.h>

namespace upc::visual {

namespace {

/* 起流重试间隔（ms）：避免 Camera 未就绪时高频打消息。 */
constexpr uint64_t kStartRetryMs = 1000;

void close_dma_fds(std::vector<int>& fds)
{
    for(int fd : fds) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
    fds.clear();
}

} // namespace

VisualService::~VisualService()
{
    shutdown();
}

bool VisualService::init()
{
    /* 先订阅响应，再发起起流请求，避免首包竞态丢失。 */
    subscribe_messages();
    send_stream_start_req();
    return true;
}

void VisualService::shutdown()
{
    /* 关闭顺序：先通知对端停流，再停本地线程并回收资源。 */
    send_stream_stop_req();
    stop_worker();
    consumer_.close();
    close_dma_fds(dma_fds_);
    subs_.clear();
}

void VisualService::tick()
{
    const uint64_t now = monotonic_ms();
    std::lock_guard<std::mutex> lock(state_mtx_);
    if(stream_up_) {
        return;
    }
    if(start_req_sent_ && now - last_start_req_ms_ < kStartRetryMs) {
        return;
    }
    /* 未起流成功时，按节流周期重试。 */
    send_stream_start_req();
}

void VisualService::subscribe_messages()
{
    auto& bus = middleware::MsgPort::instance();
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) { on_stream_start_rsp(msg); }));
}

void VisualService::send_stream_start_req()
{
    middleware::Message msg(
        middleware::MsgType::VISUAL_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_CAMERA);
    middleware::MsgPort::instance().publish_async(msg);
    start_req_sent_ = true;
    last_start_req_ms_ = monotonic_ms();
}

void VisualService::send_stream_stop_req()
{
    middleware::Message msg(
        middleware::MsgType::VISUAL_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_CAMERA);
    middleware::MsgPort::instance().publish_async(msg);
}

bool VisualService::read_inline_start_rsp(const middleware::Message& msg,
                                          middleware::CameraStreamInfoPayload& out)
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

void VisualService::on_stream_start_rsp(const middleware::Message& msg)
{
    middleware::CameraStreamInfoPayload info{};
    if(!read_inline_start_rsp(msg, info)) {
        return;
    }
    info.shm_name[sizeof(info.shm_name) - 1] = '\0';

    /* 收到新起流响应时，先清理旧会话，保证 fd/ring 一致。 */
    stop_worker();
    consumer_.close();
    close_dma_fds(dma_fds_);

    if(info.ok == 0 || info.dma_mode == 0) {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }

    std::vector<int> fds;
    if(!middleware::MsgPort::instance().take_peer_dma_fds(
           middleware::ModuleId::APP_CAMERA, fds)) {
        UPC_LOG_VISUAL_SERVICE(Error, "take dma fds failed");
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }
    if(fds.size() != info.dma_buf_count) {
        close_dma_fds(fds);
        UPC_LOG_VISUAL_SERVICE(Error,
            "visual dma fd count mismatch got=%zu expected=%u",
            fds.size(), info.dma_buf_count);
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }
    if(consumer_.open(info.shm_name) != upc_ipc::IpcShmError::OK) {
        close_dma_fds(fds);
        UPC_LOG_VISUAL_SERVICE(Error, "open shm failed %s", info.shm_name);
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }

    dma_fds_ = std::move(fds);
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = true;
        start_req_sent_ = false;
        pending_start_rsp_ = info;
        pending_start_rsp_valid_ = true;
    }
    /* 起流成功后拉起后台占位线程。 */
    stop_worker_.store(false);
    worker_ = std::thread(&VisualService::worker_thread_fn, this);
}

void VisualService::stop_worker()
{
    stop_worker_.store(true);
    if(worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(state_mtx_);
    stream_up_ = false;
    pending_start_rsp_valid_ = false;
}

void VisualService::worker_thread_fn()
{
    while(!stop_worker_.load(std::memory_order_relaxed)) {
        /* 占位进程：仅消费最新帧，维持拉流通路健康。 */
        auto frame = consumer_.acquire_latest_frame();
        if(!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const uint32_t idx = frame->buffer_idx;
        if(idx >= dma_fds_.size()) {
            consumer_.release_slot(frame->slot_index);
            continue;
        }
        consumer_.release_slot(frame->slot_index);
    }
}

uint64_t VisualService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace upc::visual
