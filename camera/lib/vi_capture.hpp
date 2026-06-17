#ifndef VI_CAPTURE_HPP
#define VI_CAPTURE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

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

    /**
     * 阻塞取一帧 NV12 并写入 dst（紧凑布局，stride_y = width）。
     * @return 成功 true；超时或错误 false
     */
    bool capture_frame(uint8_t* dst, size_t dst_size, int timeout_ms = 2000);

private:
    bool start_isp(const ViCaptureConfig& cfg);
    void stop_isp();
    bool start_v4l2(const ViCaptureConfig& cfg);
    void stop_v4l2();
    bool copy_nv12_to(uint8_t* dst, size_t dst_size, const uint8_t* src_y,
                      const uint8_t* src_uv, uint32_t src_stride_y,
                      bool uv_separate_plane);

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

    bool     open_        = false;
    uint32_t width_       = 0;
    uint32_t height_      = 0;
    uint32_t stride_y_    = 0;
    size_t   frame_bytes_ = 0;
    uint32_t cap_fail_poll_   = 0;
    uint32_t cap_fail_dqbuf_  = 0;
    uint32_t cap_fail_copy_   = 0;
    uint32_t cap_fail_qbuf_   = 0;
};

} // namespace upc::camera

#endif /* VI_CAPTURE_HPP */
