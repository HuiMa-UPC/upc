#ifndef UPC_DMA_HEAP_BUFFER_HPP
#define UPC_DMA_HEAP_BUFFER_HPP

#include <cstddef>
#include <cstdint>

namespace upc_ipc {

/**
 * @brief 单个 dma-heap 缓冲区句柄。
 *
 * 不持有所有权语义，调用方负责调用 dma_heap_free 释放。
 */
struct DmaHeapBuffer {
    int          fd        = -1;
    void*        va        = nullptr;
    size_t       size      = 0;
    const char*  heap_path = nullptr;
};

/**
 * 从 dma-heap 分配 size 字节，得到 CPU 可 mmap 的 fd 与映射地址。
 * @param size 请求字节数（>0）
 * @param out  输出句柄
 * @return 成功返回 true
 */
bool dma_heap_alloc(size_t size, DmaHeapBuffer& out);

/**
 * 释放并复位 DmaHeapBuffer。
 */
void dma_heap_free(DmaHeapBuffer& buf);

} // namespace upc_ipc

#endif /* UPC_DMA_HEAP_BUFFER_HPP */
