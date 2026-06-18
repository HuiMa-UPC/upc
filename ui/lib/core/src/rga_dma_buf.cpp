#include "rga_dma_buf.hpp"
#include "rs_log.hpp"

#include <linux/dma-heap.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ui {

namespace {

constexpr const char* kDmaHeapCandidates[] = {
    "/dev/dma_heap/system-dma32",
    "/dev/dma_heap/system-uncached-dma32",
    "/dev/rk_dma_heap/rk-dma-heap-cma",
    "/dev/dma_heap/cma-uncached",
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/system",
};

bool dma_heap_alloc_once(const char* path, size_t size, int* out_fd, void** out_va)
{
    if(!path || !out_fd || !out_va || size == 0) {
        return false;
    }

    const int heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if(heap_fd < 0) {
        return false;
    }

    dma_heap_allocation_data req{};
    req.len       = size;
    req.fd_flags  = O_RDWR | O_CLOEXEC;
    req.heap_flags = 0;

    if(ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) != 0) {
        close(heap_fd);
        return false;
    }
    close(heap_fd);

    if(req.fd == 0) {
        return false;
    }

    void* va = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if(va == MAP_FAILED) {
        close(req.fd);
        return false;
    }

    *out_fd = req.fd;
    *out_va = va;
    return true;
}

} // namespace

RgaDmaBuf::~RgaDmaBuf()
{
    free();
}

RgaDmaBuf::RgaDmaBuf(RgaDmaBuf&& other) noexcept
    : fd_(other.fd_)
    , va_(other.va_)
    , size_(other.size_)
    , heap_path_(other.heap_path_)
{
    other.fd_        = -1;
    other.va_        = nullptr;
    other.size_      = 0;
    other.heap_path_ = nullptr;
}

RgaDmaBuf& RgaDmaBuf::operator=(RgaDmaBuf&& other) noexcept
{
    if(this != &other) {
        free();
        fd_        = other.fd_;
        va_        = other.va_;
        size_      = other.size_;
        heap_path_ = other.heap_path_;
        other.fd_        = -1;
        other.va_        = nullptr;
        other.size_      = 0;
        other.heap_path_ = nullptr;
    }
    return *this;
}

bool RgaDmaBuf::alloc(size_t size)
{
    free();
    if(size == 0) {
        return false;
    }

    for(const char* path : kDmaHeapCandidates) {
        int fd = -1;
        void* va = nullptr;
        if(!dma_heap_alloc_once(path, size, &fd, &va)) {
            continue;
        }

        fd_        = fd;
        va_        = va;
        size_      = size;
        heap_path_ = path;

        static bool logged{false};
        if(!logged) {
            logged = true;
            UPC_LOG_CAMERA_PREVIEW(Info, "RGA dma heap using %s", path);
        }
        return true;
    }

    UPC_LOG_CAMERA_PREVIEW(Error,
        "RGA dma heap alloc failed size=%zu (%s)",
        size, std::strerror(errno));
    return false;
}

void RgaDmaBuf::free()
{
    if(va_ && va_ != MAP_FAILED) {
        munmap(va_, size_);
    }
    va_ = nullptr;

    if(fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;

    size_      = 0;
    heap_path_ = nullptr;
}

} // namespace ui
