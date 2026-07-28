#include "visual_service.hpp"

#include "rs_log.hpp"
#include "upc_contract.hpp"

#if defined(UPC_VISUAL_HAS_OPENCV) && UPC_VISUAL_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace upc::visual {

namespace {

/* 起流重试间隔（ms）：避免 Camera 未就绪时高频打消息。 */
constexpr uint64_t kStartRetryMs = 1000;
/* 结果推送最小间隔（ms）：降低 UI 抖动与 IPC 压力。 */
constexpr uint64_t kEmitMinIntervalMs = 120;

void close_dma_fds(std::vector<int>& fds)
{
    for(int fd : fds) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
    fds.clear();
}

#if defined(UPC_VISUAL_HAS_OPENCV) && UPC_VISUAL_HAS_OPENCV
void dma_buf_sync_start_read(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void dma_buf_sync_end_read(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

std::array<cv::Point, 4> sort_quad_clockwise(const std::vector<cv::Point>& in)
{
    std::array<cv::Point, 4> out{};
    if(in.size() != 4) {
        return out;
    }
    cv::Point2f center(0.f, 0.f);
    for(const auto& p : in) {
        center.x += static_cast<float>(p.x);
        center.y += static_cast<float>(p.y);
    }
    center.x /= 4.f;
    center.y /= 4.f;

    std::array<std::pair<float, cv::Point>, 4> ang{};
    for(size_t i = 0; i < 4; ++i) {
        const float a = std::atan2(static_cast<float>(in[i].y) - center.y,
                                   static_cast<float>(in[i].x) - center.x);
        ang[i] = {a, in[i]};
    }
    std::sort(ang.begin(), ang.end(),
              [](const auto& l, const auto& r) { return l.first < r.first; });
    for(size_t i = 0; i < 4; ++i) {
        out[i] = ang[i].second;
    }
    return out;
}

/**
 * 轻量矩形检测：
 * RGB -> Gray -> Blur -> Canny -> Contours -> 四边形逼近，
 * 最终取“面积最大且凸”的四边形作为输出。
 */
bool detect_best_rect(const cv::Mat& rgb, int32_t pts[8])
{
    if(rgb.empty() || !pts) {
        return false;
    }

    cv::Mat gray;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

    cv::Mat edges;
    cv::Canny(gray, edges, 80.0, 180.0);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

    double best_area = 0.0;
    std::array<cv::Point, 4> best{};
    const double area_thresh = static_cast<double>(rgb.cols) *
                               static_cast<double>(rgb.rows) * 0.02;

    for(const auto& c : contours) {
        const double peri = cv::arcLength(c, true);
        if(peri < 20.0) {
            continue;
        }
        std::vector<cv::Point> approx;
        cv::approxPolyDP(c, approx, 0.02 * peri, true);
        if(approx.size() != 4 || !cv::isContourConvex(approx)) {
            continue;
        }
        const double area = std::fabs(cv::contourArea(approx));
        if(area < area_thresh || area <= best_area) {
            continue;
        }
        best_area = area;
        best = sort_quad_clockwise(approx);
    }

    if(best_area <= 0.0) {
        return false;
    }
    for(size_t i = 0; i < 4; ++i) {
        pts[i * 2] = best[i].x;
        pts[i * 2 + 1] = best[i].y;
    }
    return true;
}
#endif

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
    /* 起流成功后拉起后台检测线程。 */
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
        /* 只消费最新帧，降低累积延迟。 */
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
        const uint32_t w = frame->width();
        const uint32_t h = frame->height();

        bool found = false;
        int32_t pts[8] = {0};

#if defined(UPC_VISUAL_HAS_OPENCV) && UPC_VISUAL_HAS_OPENCV
        /* dma-buf 读同步 + mmap：直接在共享 NV12 上做零拷贝读取。 */
        const int src_fd = dma_fds_[idx];
        const uint32_t stride = frame->stride_y() > 0 ? frame->stride_y() : w;
        const size_t src_size = static_cast<size_t>(stride) * h * 3u / 2u;
        dma_buf_sync_start_read(src_fd);
        void* src_va = mmap(nullptr, src_size, PROT_READ, MAP_SHARED, src_fd, 0);
        if(src_va != MAP_FAILED) {
            cv::Mat yuv_full(static_cast<int>(h * 3u / 2u),
                             static_cast<int>(stride),
                             CV_8UC1,
                             src_va);
            cv::Mat yuv_roi = yuv_full(
                cv::Rect(0, 0, static_cast<int>(w), static_cast<int>(h * 3u / 2u)));
            cv::Mat rgb;
            cv::cvtColor(yuv_roi, rgb, cv::COLOR_YUV2RGB_NV12);
            found = detect_best_rect(rgb, pts);
            munmap(src_va, src_size);
        }
        dma_buf_sync_end_read(src_fd);
#endif

        /* 不论是否识别到目标都发布，UI 可据 valid 控制显隐。 */
        publish_rect(found, w, h, frame->seq, pts);
        consumer_.release_slot(frame->slot_index);
    }
}

void VisualService::publish_rect(bool valid,
                                 uint32_t frame_w,
                                 uint32_t frame_h,
                                 uint32_t frame_seq,
                                 const int32_t pts[8])
{
    const uint64_t now = monotonic_ms();
    if(now - last_emit_ms_ < kEmitMinIntervalMs && valid == last_valid_) {
        return;
    }
    last_emit_ms_ = now;
    last_valid_ = valid;

    middleware::VisualRectNtfPayload payload{};
    payload.valid = valid ? 1 : 0;
    payload.frame_width = frame_w;
    payload.frame_height = frame_h;
    payload.frame_seq = frame_seq;
    if(valid && pts) {
        for(size_t i = 0; i < 4; ++i) {
            payload.pts[i].x = pts[i * 2];
            payload.pts[i].y = pts[i * 2 + 1];
        }
    }

    /* 单播到 UI，由 page_user_home 做坐标映射和线框叠加。 */
    middleware::Message msg(
        middleware::MsgType::APP_Visual_Rect_Ntf,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    middleware::MsgPort::instance().publish_async(msg);
}

uint64_t VisualService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace upc::visual
