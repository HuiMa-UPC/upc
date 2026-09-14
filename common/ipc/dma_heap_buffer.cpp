#include "dma_heap_buffer.hpp"

#include <linux/dma-heap.h>

#include <cerrno>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace upc_ipc {

namespace {

/*
 * RGA2 可寻址的堆：dma32 受 <4G 约束，reserved/cma 为物理连续 carveout。
 * 顺序即优先级——先试受约束但廉价的 dma32，再退到连续 carveout。
 */
constexpr const char* kRgaSafeHeaps[] = {
    "/dev/dma_heap/system-dma32",
    "/dev/dma_heap/system-uncached-dma32",
    "/dev/dma_heap/reserved",
    "/dev/rk_dma_heap/rk-dma-heap-cma",
    "/dev/dma_heap/cma-uncached",
};

/*
 * 仅 Generic 用途可用的兜底堆：页式分配，无连续性与 <4G 保证。
 * 交给 RGA 会在 importbuffer_fd() 处失败，故 RgaDevice 不得使用。
 */
constexpr const char* kPagedFallbackHeaps[] = {
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/system",
};

/** 尝试从单个堆分配并 mmap；成功返回 true 并填充 out。 */
bool try_alloc_from_heap(const char* path, size_t size, DmaHeapBuffer& out)
{
    const int heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if(heap_fd < 0) {
        return false;
    }

    dma_heap_allocation_data req{};
    req.len        = size;
    req.fd_flags   = O_RDWR | O_CLOEXEC;
    req.heap_flags = 0;

    if(ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) != 0 || req.fd <= 0) {
        close(heap_fd);
        return false;
    }
    close(heap_fd);

    void* va = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);
    if(va == MAP_FAILED) {
        close(req.fd);
        return false;
    }

    out.fd        = req.fd;
    out.va        = va;
    out.size      = size;
    out.heap_path = path;
    return true;
}

} // namespace

/*
 * 这里刻意不打日志：本文件属于 IPC 专题，而 UPC_MOD_IPC 默认为 0，
 * UPC_LOG_IPC 会被整条编译掉，写在这里的诊断永远不会出现在日志里。
 * 失败信息由调用方负责记录——调用方也更清楚这块内存是干什么用的。
 */
bool dma_heap_alloc(size_t size, DmaHeapBuffer& out, DmaHeapUsage usage)
{
    dma_heap_free(out);
    if(size == 0) {
        return false;
    }

    for(const char* path : kRgaSafeHeaps) {
        if(try_alloc_from_heap(path, size, out)) {
            return true;
        }
    }

    /*
     * RgaDevice 到此为止：宁可让调用方立刻拿到 false 并回退到软件路径，
     * 也不要交出一块 RGA 用不了的页式内存。
     */
    if(usage == DmaHeapUsage::RgaDevice) {
        return false;
    }

    for(const char* path : kPagedFallbackHeaps) {
        if(try_alloc_from_heap(path, size, out)) {
            return true;
        }
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
