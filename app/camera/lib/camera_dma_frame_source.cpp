#include "camera_dma_frame_source.hpp"

#include "device_config.hpp"
#include "dma_heap_buffer.hpp"
#include "rs_log.hpp"

#include <cstring>

#include <sys/ioctl.h>
#include <linux/dma-buf.h>

namespace upc::camera {

namespace {

void dma_buf_sync_end_write(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

} // namespace

uint32_t CameraDmaFrameSource::align_up(uint32_t v, uint32_t align)
{
    if(align == 0) {
        return v;
    }
    return (v + align - 1u) & ~(align - 1u);
}

bool CameraDmaFrameSource::alloc_padded_pool(uint32_t count, uint32_t stride_y,
                                             uint32_t height)
{
    free_padded_pool();
    if(count == 0 || stride_y == 0 || height == 0) {
        return false;
    }

    const size_t size = static_cast<size_t>(stride_y) * height * 3u / 2u;
    padded_bufs_.resize(count);
    padded_free_.assign(count, true);

    for(uint32_t i = 0; i < count; ++i) {
        upc_ipc::DmaHeapBuffer hb{};
        if(!upc_ipc::dma_heap_alloc(size, hb)) {
            UPC_LOG_CAMERA_SERVICE(Error,
                "DmaFrameSource padding alloc failed idx=%u size=%zu", i, size);
            free_padded_pool();
            return false;
        }
        padded_bufs_[i].fd       = hb.fd;
        padded_bufs_[i].va       = hb.va;
        padded_bufs_[i].size     = hb.size;
        padded_bufs_[i].stride_y = stride_y;
        padded_bufs_[i].height   = height;
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "DmaFrameSource padding pool count=%u stride=%u height=%u size=%zu",
        count, stride_y, height, size);
    return true;
}

void CameraDmaFrameSource::free_padded_pool()
{
    for(auto& b : padded_bufs_) {
        upc_ipc::DmaHeapBuffer hb{};
        hb.fd   = b.fd;
        hb.va   = b.va;
        hb.size = b.size;
        upc_ipc::dma_heap_free(hb);
        b = {};
    }
    padded_bufs_.clear();
    padded_free_.clear();
}

bool CameraDmaFrameSource::prepare(uint32_t width, uint32_t height,
                                   uint32_t capture_stride_y,
                                   const std::vector<int>& capture_dma_fds)
{
    shutdown();

    if(width == 0 || height == 0 || capture_stride_y == 0 ||
       capture_dma_fds.empty()) {
        return false;
    }

    width_          = width;
    height_         = height;
    capture_fds_    = capture_dma_fds;
    stride_aligned_ = (capture_stride_y % CAMERA_STRIDE_ALIGN) == 0;
    aligned_stride_ = stride_aligned_
                          ? capture_stride_y
                          : align_up(capture_stride_y, CAMERA_STRIDE_ALIGN);
    aligned_height_ = align_up(height, CAMERA_STRIDE_ALIGN);

    fds_to_send_.clear();
    fds_to_send_.reserve(capture_fds_.size());

    if(stride_aligned_) {
        for(int fd : capture_fds_) {
            if(fd < 0) {
                shutdown();
                return false;
            }
            fds_to_send_.push_back(fd);
        }
    } else {
        if(!alloc_padded_pool(static_cast<uint32_t>(capture_fds_.size()),
                              aligned_stride_, aligned_height_)) {
            shutdown();
            return false;
        }
        for(uint32_t i = 0; i < padded_bufs_.size(); ++i) {
            const int fd = padded_bufs_[i].fd;
            if(fd < 0) {
                shutdown();
                return false;
            }
            fds_to_send_.push_back(fd);
        }
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "DmaFrameSource ready aligned=%d stride=%u height=%u bufs=%zu",
        stride_aligned_ ? 1 : 0, aligned_stride_, aligned_height_,
        fds_to_send_.size());
    return true;
}

void CameraDmaFrameSource::shutdown()
{
    free_padded_pool();
    capture_fds_.clear();
    fds_to_send_.clear();
    stride_aligned_ = false;
    aligned_stride_ = 0;
    aligned_height_ = 0;
    width_          = 0;
    height_         = 0;
}

uint32_t CameraDmaFrameSource::buffer_count() const noexcept
{
    return stride_aligned_
               ? static_cast<uint32_t>(capture_fds_.size())
               : static_cast<uint32_t>(padded_bufs_.size());
}

int CameraDmaFrameSource::fd_for_buffer_idx(uint32_t idx) const
{
    if(stride_aligned_) {
        return idx < capture_fds_.size() ? capture_fds_[idx] : -1;
    }
    return idx < padded_bufs_.size() ? padded_bufs_[idx].fd : -1;
}

int CameraDmaFrameSource::acquire_padded()
{
    if(stride_aligned_ || padded_free_.empty()) {
        return -1;
    }
    for(size_t i = 0; i < padded_free_.size(); ++i) {
        if(padded_free_[i]) {
            padded_free_[i] = false;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void CameraDmaFrameSource::release_padded(uint32_t idx)
{
    if(idx < padded_free_.size()) {
        padded_free_[idx] = true;
    }
}

void CameraDmaFrameSource::copy_padded_frame(uint32_t dst_idx,
                                             const uint8_t* src_y,
                                             const uint8_t* src_uv,
                                             uint32_t src_stride_y,
                                             uint32_t width, uint32_t height)
{
    if(stride_aligned_ || !src_y || width == 0 || height == 0) {
        return;
    }
    if(dst_idx >= padded_bufs_.size()) {
        return;
    }

    const auto& dst = padded_bufs_[dst_idx];
    if(!dst.va || dst.stride_y == 0) {
        return;
    }

    const uint8_t* src_uv_eff = src_uv
                                    ? src_uv
                                    : src_y + static_cast<size_t>(src_stride_y) * height;

    uint8_t* dst_y  = static_cast<uint8_t*>(dst.va);
    uint8_t* dst_uv = dst_y + static_cast<size_t>(dst.stride_y) * height;

    const uint32_t uv_h = height / 2u;
    for(uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst_y + static_cast<size_t>(row) * dst.stride_y,
                    src_y + static_cast<size_t>(row) * src_stride_y,
                    width);
    }
    for(uint32_t row = 0; row < uv_h; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * dst.stride_y,
                    src_uv_eff + static_cast<size_t>(row) * src_stride_y,
                    width);
    }

    dma_buf_sync_end_write(dst.fd);
}

} // namespace upc::camera
