#include "dma_heap_buffer.hpp"

#include <linux/dma-heap.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace upc_ipc {

namespace {

constexpr const char* kDmaHeapCandidates[] = {
    "/dev/dma_heap/system-dma32",
    "/dev/dma_heap/system-uncached-dma32",
    "/dev/dma_heap/reserved",
    "/dev/rk_dma_heap/rk-dma-heap-cma",
    "/dev/dma_heap/cma-uncached",
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/system",
};


} // namespace

bool dma_heap_alloc(size_t size, DmaHeapBuffer& out)
{
    dma_heap_free(out);
    if(size == 0) {
        return false;
    }

    for(const char* path : kDmaHeapCandidates) {
        const int heap_fd = open(path, O_RDWR | O_CLOEXEC);
        if(heap_fd < 0) {
            continue;
        }

        dma_heap_allocation_data req{};
        req.len        = size;
        req.fd_flags   = O_RDWR | O_CLOEXEC;
        req.heap_flags = 0;

        if(ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) != 0 || req.fd <= 0) {
            close(heap_fd);
            continue;
        }
        close(heap_fd);

        void* va = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
        if(va == MAP_FAILED) {
            close(req.fd);
            continue;
        }

        out.fd        = req.fd;
        out.va        = va;
        out.size      = size;
        out.heap_path = path;
        return true;
    } 

    return false;
}

void dma_heap_free(DmaHeapBuffer& buf)
{
    if(buf.va && buf.va != MAP_FAILED) {
        munmap(buf.va, buf.size);
    }
    if(buf.fd >= 0) {
        close(buf.fd);
    }
    buf = {};
}

} // namespace upc_ipc
