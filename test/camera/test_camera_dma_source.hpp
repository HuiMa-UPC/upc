#ifndef TEST_CAMERA_DMA_SOURCE_HPP
#define TEST_CAMERA_DMA_SOURCE_HPP

#include "dma_heap_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief test_camera 的 dma fd 源：从 dma-heap 分配 NV12 buffer，
 *        fd 经 MsgPort IPC SCM_RIGHTS 随 Start_Rsp 送达 UI。
 */
class TestCameraDmaSource {
public:
    TestCameraDmaSource() = default;
    ~TestCameraDmaSource() { shutdown(); }

    TestCameraDmaSource(const TestCameraDmaSource&) = delete;
    TestCameraDmaSource& operator=(const TestCameraDmaSource&) = delete;

    bool alloc(uint32_t count, uint32_t stride_y, uint32_t height);
    void shutdown();

    [[nodiscard]] std::vector<int> ipc_fds() const;
    [[nodiscard]] uint32_t count() const noexcept
    {
        return static_cast<uint32_t>(bufs_.size());
    }
    [[nodiscard]] int fd(uint32_t idx) const noexcept;
    [[nodiscard]] void* va(uint32_t idx) const noexcept;
    [[nodiscard]] size_t size(uint32_t idx) const noexcept;

    [[nodiscard]] int acquire();
    void release(uint32_t idx);

private:
    std::vector<upc_ipc::DmaHeapBuffer> bufs_;
    std::vector<bool>                   free_;
};

#endif /* TEST_CAMERA_DMA_SOURCE_HPP */
