#include "visual_service.hpp"

#include "rs_log.hpp"
#include "upc_contract.hpp"

#include <chrono>
#include <cstring>

#include <sys/mman.h>
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
    unmap_dma_views();
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
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_FirstLayer_Check_Req,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) { on_first_layer_check_req(msg); }));
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
    unmap_dma_views();
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

        if(frame->buffer_idx < dma_fds_.size() && map_dma_views_if_needed()) {
            update_latest_snapshot(*frame);
        }
        consumer_.release_slot(frame->slot_index);
    }
}

bool VisualService::map_dma_views_if_needed()
{
    if(!dma_maps_.empty()) {
        return true;
    }
    if(dma_fds_.empty() || !pending_start_rsp_valid_ ||
       pending_start_rsp_.stride_y == 0 || pending_start_rsp_.height == 0) {
        return false;
    }
    dma_map_size_ = static_cast<size_t>(pending_start_rsp_.stride_y) *
        static_cast<size_t>(pending_start_rsp_.height) * 3u / 2u;
    dma_maps_.resize(dma_fds_.size(), nullptr);
    for(size_t i = 0; i < dma_fds_.size(); ++i) {
        void* p = ::mmap(nullptr, dma_map_size_, PROT_READ, MAP_SHARED, dma_fds_[i], 0);
        if(p == MAP_FAILED) {
            UPC_LOG_VISUAL_SERVICE(Error, "visual mmap dma failed idx=%zu", i);
            unmap_dma_views();
            return false;
        }
        dma_maps_[i] = static_cast<uint8_t*>(p);
    }
    return true;
}

void VisualService::unmap_dma_views()
{
    if(dma_map_size_ == 0) {
        dma_maps_.clear();
        return;
    }
    for(uint8_t* p : dma_maps_) {
        if(p) {
            ::munmap(p, dma_map_size_);
        }
    }
    dma_maps_.clear();
    dma_map_size_ = 0;
}

void VisualService::copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                    const uint8_t* src_uv, uint32_t src_stride_y,
                                    uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || src_stride_y == 0 || width == 0 || height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }
    for(uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * width,
                    src_y + static_cast<size_t>(y) * src_stride_y, width);
    }
    const uint32_t uv_h = height / 2u;
    for(uint32_t y = 0; y < uv_h; ++y) {
        std::memcpy(dst_uv + static_cast<size_t>(y) * width,
                    src_uv + static_cast<size_t>(y) * src_stride_y, width);
    }
}

void VisualService::update_latest_snapshot(const upc_ipc::IpcDmaFrameView& frame)
{
    const uint32_t idx = frame.buffer_idx;
    if(idx >= dma_maps_.size()) {
        return;
    }
    const uint32_t width = frame.width();
    const uint32_t height = frame.height();
    const uint32_t stride = frame.stride_y();
    if(width == 0 || height == 0 || stride == 0) {
        return;
    }
    const uint8_t* src = dma_maps_[idx];
    if(!src) {
        return;
    }
    const uint8_t* src_y = src;
    const uint8_t* src_uv = src + static_cast<size_t>(stride) * height;
    const size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
    std::lock_guard<std::mutex> lock(frame_mtx_);
    latest_nv12_snapshot_.resize(need);
    copy_nv12_tight(latest_nv12_snapshot_.data(), src_y, src_uv, stride, width, height);
    latest_width_ = width;
    latest_height_ = height;
    latest_frame_seq_ = frame.seq;
    latest_nv12_valid_ = true;
}

bool VisualService::copy_latest_snapshot(std::vector<uint8_t>& out, uint32_t& width,
                                         uint32_t& height, uint32_t& frame_seq)
{
    std::lock_guard<std::mutex> lock(frame_mtx_);
    if(!latest_nv12_valid_ || latest_nv12_snapshot_.empty()) {
        return false;
    }
    out = latest_nv12_snapshot_;
    width = latest_width_;
    height = latest_height_;
    frame_seq = latest_frame_seq_;
    return true;
}

bool VisualService::process_first_layer_image_placeholder(const std::vector<uint8_t>& nv12,
                                                          uint32_t width, uint32_t height,
                                                          uint32_t frame_seq)
{
    /* 预留给视觉算法同学：此处可接 OpenCV/模型推理等真实检测逻辑。 */
    (void)nv12;
    UPC_LOG_VISUAL_SERVICE(Info,
        "first layer placeholder invoked frame=%u %ux%u",
        static_cast<unsigned>(frame_seq),
        static_cast<unsigned>(width),
        static_cast<unsigned>(height));
    return true;
}

void VisualService::on_first_layer_check_req(const middleware::Message& msg)
{
    middleware::VisualFirstLayerCheckReqPayload req{};
    bool has_req = false;
    if(msg.payload.type == middleware::MsgPayload::Type::Inline &&
       msg.payload.length >= sizeof(req)) {
        std::memcpy(&req, msg.payload.data.bytes.data(), sizeof(req));
        has_req = true;
    }

    std::vector<uint8_t> nv12;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_seq = 0;
    bool pass = false;
    if(copy_latest_snapshot(nv12, width, height, frame_seq)) {
        pass = process_first_layer_image_placeholder(nv12, width, height, frame_seq);
    } else {
        UPC_LOG_VISUAL_SERVICE(Warn, "first layer check no snapshot available");
    }

    middleware::VisualFirstLayerCheckRspPayload rsp{};
    rsp.result = pass ? 1 : 0;
    middleware::Message out(
        middleware::MsgType::APP_Visual_FirstLayer_Check_Rsp,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_GLAVO);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    middleware::MsgPort::instance().publish_async(out);

    UPC_LOG_VISUAL_SERVICE(Info,
        "first layer check done req_valid=%d layer_um=%u result=%u",
        has_req ? 1 : 0,
        has_req ? static_cast<unsigned>(req.layer_height_um) : 0u,
        static_cast<unsigned>(rsp.result));
}

uint64_t VisualService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace upc::visual
