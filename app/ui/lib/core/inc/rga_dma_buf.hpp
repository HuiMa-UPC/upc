#ifndef RGA_DMA_BUF_HPP
#define RGA_DMA_BUF_HPP

#include <cstddef>

namespace ui {

/**
 * @brief DMA heap 缓冲（优先 dma32），供 RGA importbuffer_fd 使用。
 *
 * RK3568 RGA2 仅支持物理地址 4G 以下；POSIX SHM / 普通 heap 可能 >4G。
 */
class RgaDmaBuf {
public:
    RgaDmaBuf() = default;
    ~RgaDmaBuf();

    RgaDmaBuf(const RgaDmaBuf&)            = delete;
    RgaDmaBuf& operator=(const RgaDmaBuf&) = delete;

    RgaDmaBuf(RgaDmaBuf&& other) noexcept;
    RgaDmaBuf& operator=(RgaDmaBuf&& other) noexcept;

    /** 从 dma heap 分配；成功返回 true */
    bool alloc(size_t size);
    void free();

    [[nodiscard]] bool valid() const noexcept { return va_ != nullptr; }
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] void* va() noexcept { return va_; }
    [[nodiscard]] const void* va() const noexcept { return va_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] const char* heap_path() const noexcept { return heap_path_; }

private:
    int         fd_        = -1;
    void*       va_        = nullptr;
    size_t      size_      = 0;
    const char* heap_path_ = nullptr;
};

} // namespace ui

#endif
