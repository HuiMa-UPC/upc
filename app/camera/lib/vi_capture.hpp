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

private:
    bool start_isp(const ViCaptureConfig& cfg);
    void stop_isp();
    bool start_v4l2(const ViCaptureConfig& cfg);
    void stop_v4l2();
    bool export_dma_fds();

    void log_capture_fail(const char* stage);

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
};

} // namespace upc::camera

#endif /* VI_CAPTURE_HPP */
