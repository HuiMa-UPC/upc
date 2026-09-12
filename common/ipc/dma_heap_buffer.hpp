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
 * @brief 分配用途，决定候选堆范围。
 *
 * RK3568 的 RGA2 没有 IOMMU，只能寻址「物理连续且 <4G」的内存。而 dma-heap 里
 * `system` / `system-uncached` 是页式堆，既不连续也无 <4G 保证——从它们拿到的
 * buffer 虽然 mmap 得到，但 `importbuffer_fd()` 必然失败。
 *
 * 因此凡是要交给 RGA 的分配都必须用 RgaDevice：候选堆只取连续/dma32 那几个，
 * 装不下就明确失败，绝不静默回退到页式堆——否则调用方要等到 RGA 导入那一步
 * 才发现内存不可用，问题现场早已丢失。
 */
enum class DmaHeapUsage {
    Generic,   /**< 任意堆均可，允许回退到 system 页式堆 */
    RgaDevice, /**< 必须 RGA2 可寻址：仅连续/dma32 堆，装不下即失败 */
};

/**
 * 从 dma-heap 分配 size 字节，得到 CPU 可 mmap 的 fd 与映射地址。
 * @param size  请求字节数（>0）
 * @param out   输出句柄
 * @param usage 分配用途，见 DmaHeapUsage；默认 Generic 保持既有调用方行为
 * @return 成功返回 true；失败时会打印试过的堆与各自 errno
 */
bool dma_heap_alloc(size_t size, DmaHeapBuffer& out,
                    DmaHeapUsage usage = DmaHeapUsage::Generic);

/**
 * 释放并复位 DmaHeapBuffer。
 */
void dma_heap_free(DmaHeapBuffer& buf);

} // namespace upc_ipc

#endif /* UPC_DMA_HEAP_BUFFER_HPP */
