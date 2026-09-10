#ifndef VI_CAPTURE_HPP
#define VI_CAPTURE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace upc::camera {

struct ViCaptureConfig {
    std::string video_dev;
    std::string iq_dir;
    std::string iq_file;
    uint32_t    width  = 0;
    uint32_t    height = 0;
    uint32_t    fps    = 0;
    uint32_t    buf_count = 0;
};

/**
 * 取帧延迟诊断（预览端到端延迟分段量化的第一段）。
 *
 * V4L2 是 FIFO：DQBUF 拿到的是驱动队列里最老的一帧。若采集线程曾停顿，
 * 驱动会趁机填满多个 buffer，而"每轮取一帧还一帧"的固定节奏无法把积压
 * 排掉——积压会固化成恒定的画面延迟。`latency_us` 正是用来量化它的：
 * 取 buf.timestamp（驱动填完该帧的时刻）与当前单调时钟之差。
 *
 * 只有驱动声明时间戳为 MONOTONIC 时该值才可比，否则 timestamp_monotonic
 * 为 false、samples 为 0，需改用其他手段判断积压。
 *
 * dequeue_frame 已排空到最新帧，所以 latency_us 稳态应贴在一帧周期附近；
 * 若它仍持续爬升，说明排空没生效或另有阻塞点。
 */
struct ViCaptureLatencyStats {
    uint64_t samples        = 0; /**< 有效滞留样本数（只统计交付帧） */
    uint64_t latency_us_sum = 0; /**< 滞留时间累计（微秒） */
    uint64_t latency_us_max = 0; /**< 单帧最大滞留（微秒） */
    uint32_t seq_gap        = 0; /**< sequence 跳号累计（驱动侧丢帧数，不含 drained） */
    uint32_t drained        = 0; /**< 排空丢弃的过期帧数（>0 即积压被清掉过） */
    bool timestamp_monotonic = false; /**< 驱动时间戳是否可与 CLOCK_MONOTONIC 比较 */
};

/** rkaiq ISP + V4L2 NV12 采集（无 Rockit/VENC） */
class ViCapture {
public:
    ViCapture() = default;
    ~ViCapture();

    ViCapture(const ViCapture&) = delete;
    ViCapture& operator=(const ViCapture&) = delete;

    bool open(const ViCaptureConfig& cfg);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] size_t frame_bytes() const noexcept { return frame_bytes_; }
    [[nodiscard]] uint32_t stride_y() const noexcept { return stride_y_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

    /** V4L2 buffer 导出的 dma_buf fd 表（与 buffer 索引一一对应） */
    [[nodiscard]] const std::vector<int>& dma_fds() const noexcept { return dma_fds_; }

    /**
     * 阻塞取一帧，返回 V4L2 buffer 索引。
     * @return 成功返回 buffer 索引；超时/错误返回 nullopt
     */
    [[nodiscard]] std::optional<uint32_t> dequeue_frame(int timeout_ms = 2000);

    /**
     * 将指定 V4L2 buffer 归还驱动。
     * @param index buffer 索引
     * @return 成功 true
     */
    bool queue_frame(uint32_t index);

    /** 获取指定 V4L2 buffer 的 Y 平面 CPU 地址（mmap 所得） */
    [[nodiscard]] const uint8_t* buffer_y(uint32_t idx) const;
    /** 获取指定 V4L2 buffer 的 UV 平面 CPU 地址 */
    [[nodiscard]] const uint8_t* buffer_uv(uint32_t idx) const;

    /** 取走并清零取帧延迟统计（供采集线程并进 5s 聚合日志）。 */
    ViCaptureLatencyStats take_latency_stats();

private:
    bool start_isp(const ViCaptureConfig& cfg);
    void stop_isp();
    bool start_v4l2(const ViCaptureConfig& cfg);
    void stop_v4l2();
    bool export_dma_fds();

    void log_capture_fail(const char* stage);
    /** 累积 sequence 跳号（诊断用）。每帧 DQBUF 都要调，含被排空丢弃的帧。 */
    void note_dequeued_sequence(uint32_t sequence);
    /** 累积交付帧的驱动滞留时间（诊断用，不影响取帧行为）。 */
    void accumulate_delivered_latency(uint32_t flags, int64_t ts_us);

    void*  aiq_ctx_     = nullptr;
    bool   isp_started_ = false;

    int    v4l2_fd_     = -1;
    bool   use_mplane_  = false;
    uint32_t v4l2_buf_count_ = 0;

    struct MappedBuffer {
        void*  start  = nullptr;
        size_t length = 0;
        void*  uv     = nullptr;
        size_t uv_len = 0;
    };
    MappedBuffer* buffers_ = nullptr;
    std::vector<int> dma_fds_;

    bool     open_        = false;
    uint32_t width_       = 0;
    uint32_t height_      = 0;
    uint32_t stride_y_    = 0;
    size_t   frame_bytes_ = 0;
    uint32_t cap_fail_poll_   = 0;
    uint32_t cap_fail_dqbuf_  = 0;
    uint32_t cap_fail_qbuf_   = 0;

    /* 取帧延迟诊断累积量，由 take_latency_stats() 取走并清零。 */
    ViCaptureLatencyStats lat_stats_{};
    uint32_t lat_last_sequence_ = 0;
    bool     lat_have_sequence_ = false;
};

} // namespace upc::camera

#endif /* VI_CAPTURE_HPP */
