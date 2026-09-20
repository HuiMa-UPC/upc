#ifndef CAMERA_DMA_FRAME_SOURCE_HPP
#define CAMERA_DMA_FRAME_SOURCE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace upc::camera {

/**
 * @brief 相机 dma fd 帧源：V4L2 采集缓冲 → 隔离 pool（dma32/CMA）→ 分发给消费者。
 *
 * 始终拷贝到隔离 pool，再经 MsgPort IPC（/tmp/upc_camera.sock）SCM_RIGHTS
 * 随 Start_Rsp 发给 UI/Media/Visual。消费者从不持有 ISP 正在 map 的 V4L2 buf，
 * 避免超时/看门狗路径上 rkisp IOMMU 与 RGA/mmap 交叉。
 *
 * 隔离 pool 用 DmaHeapUsage::RgaDevice，保证 UI 侧 importbuffer_fd 可用。
 */
class CameraDmaFrameSource {
public:
    CameraDmaFrameSource() = default;
    ~CameraDmaFrameSource() { shutdown(); }

    CameraDmaFrameSource(const CameraDmaFrameSource&) = delete;
    CameraDmaFrameSource& operator=(const CameraDmaFrameSource&) = delete;

    bool prepare(uint32_t width, uint32_t height, uint32_t capture_stride_y,
                 const std::vector<int>& capture_dma_fds);
    void shutdown();

    [[nodiscard]] const std::vector<int>& ipc_fds() const noexcept {
        return fds_to_send_;
    }
    /**
     * 采集侧 stride 是否已满足 CAMERA_STRIDE_ALIGN（仅诊断）。
     * 交付路径始终走隔离拷贝，与此无关。
     */
    [[nodiscard]] bool capture_stride_aligned() const noexcept {
        return capture_stride_aligned_;
    }
    [[nodiscard]] uint32_t dma_stride_y() const noexcept { return aligned_stride_; }
    [[nodiscard]] uint32_t buffer_count() const noexcept;
    [[nodiscard]] int fd_for_buffer_idx(uint32_t idx) const;

    [[nodiscard]] int acquire_padded();
    void release_padded(uint32_t idx);

    void copy_padded_frame(uint32_t dst_idx, const uint8_t* src_y,
                           const uint8_t* src_uv, uint32_t src_stride_y,
                           uint32_t width, uint32_t height);

private:
    struct PaddedBuffer {
        int      fd        = -1;
        void*    va        = nullptr;
        size_t   size      = 0;
        uint32_t stride_y  = 0;
        uint32_t height    = 0;
    };

    bool alloc_padded_pool(uint32_t count, uint32_t stride_y, uint32_t height);
    void free_padded_pool();

    static uint32_t align_up(uint32_t v, uint32_t align);

    std::vector<PaddedBuffer> padded_bufs_;
    std::vector<bool>         padded_free_;
    std::vector<int>          capture_fds_;
    std::vector<int>          fds_to_send_;

    bool     capture_stride_aligned_ = false;
    uint32_t aligned_stride_         = 0;
    uint32_t aligned_height_         = 0;
    uint32_t width_                  = 0;
    uint32_t height_                 = 0;
};

} // namespace upc::camera

#endif /* CAMERA_DMA_FRAME_SOURCE_HPP */
