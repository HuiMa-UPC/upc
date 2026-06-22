#include "test_camera_dma_source.hpp"

#include "camera_test_config.hpp"
#include "rs_log.hpp"

namespace {

inline uint32_t align_up(uint32_t v, uint32_t align)
{
    if(align == 0) {
        return v;
    }
    return (v + align - 1u) & ~(align - 1u);
}

} // namespace

bool TestCameraDmaSource::alloc(uint32_t count, uint32_t stride_y,
                                uint32_t height)
{
    shutdown();
    if(count == 0 || stride_y == 0 || height == 0) {
        return false;
    }

    const uint32_t aligned_height =
        align_up(height, TEST_CAMERA_STRIDE_ALIGN);
    const size_t size = static_cast<size_t>(stride_y) * aligned_height * 3u / 2u;
    bufs_.resize(count);
    free_.assign(count, true);

    for(uint32_t i = 0; i < count; ++i) {
        if(!upc_ipc::dma_heap_alloc(size, bufs_[i])) {
            UPC_LOG_TEST_CAMERA(Error,
                "dma buffer alloc failed idx=%u size=%zu", i, size);
            shutdown();
            return false;
        }
    }

    UPC_LOG_TEST_CAMERA(Info,
        "dma source allocated count=%u stride_y=%u height=%u size=%zu",
        count, stride_y, height, size);
    return true;
}

void TestCameraDmaSource::shutdown()
{
    for(auto& b : bufs_) {
        upc_ipc::dma_heap_free(b);
    }
    bufs_.clear();
    free_.clear();
}

std::vector<int> TestCameraDmaSource::ipc_fds() const
{
    std::vector<int> fds;
    fds.reserve(bufs_.size());
    for(const auto& b : bufs_) {
        fds.push_back(b.fd);
    }
    return fds;
}

int TestCameraDmaSource::fd(uint32_t idx) const noexcept
{
    return idx < bufs_.size() ? bufs_[idx].fd : -1;
}

void* TestCameraDmaSource::va(uint32_t idx) const noexcept
{
    return idx < bufs_.size() ? bufs_[idx].va : nullptr;
}

size_t TestCameraDmaSource::size(uint32_t idx) const noexcept
{
    return idx < bufs_.size() ? bufs_[idx].size : 0;
}

int TestCameraDmaSource::acquire()
{
    for(size_t i = 0; i < free_.size(); ++i) {
        if(free_[i]) {
            free_[i] = false;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TestCameraDmaSource::release(uint32_t idx)
{
    if(idx < free_.size()) {
        free_[idx] = true;
    }
}
