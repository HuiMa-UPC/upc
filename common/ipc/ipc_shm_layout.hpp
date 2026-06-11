#ifndef UPC_IPC_SHM_LAYOUT_HPP
#define UPC_IPC_SHM_LAYOUT_HPP

#include "ipc_shm_types.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace upc_ipc {

/** 共享内存魔数 'UPHS'（UPC SHM） */
constexpr uint32_t IPC_SHM_MAGIC   = 0x55485053u;
/** 当前共享内存协议版本号 */
constexpr uint32_t IPC_SHM_VERSION = 1u;

/** 共享内存全局头部（紧凑布局，无填充） */
#pragma pack(push, 1)
struct IpcShmGlobalHeader {
    uint32_t magic;             /**< 魔数，应为 IPC_SHM_MAGIC */
    uint32_t version;           /**< 协议版本号 */
    uint32_t slot_count;        /**< 环形缓冲区槽位总数 */
    uint32_t slot_stride_bytes; /**< 单个槽位 payload 字节数 */
    uint32_t width;             /**< 图像宽度（像素） */
    uint32_t height;            /**< 图像高度（像素） */
    uint32_t stride_y;          /**< Y 平面行字节步长 */
    uint32_t format;            /**< 像素格式（IpcShmPixelFormat） */
    uint32_t header_bytes;      /**< 全局头部字节数 */
    uint32_t slots_bytes;       /**< 所有槽位头部总字节数 */
    uint32_t payload_bytes;     /**< 所有槽位 payload 总字节数 */
};
#pragma pack(pop)

/** 单个槽位的头部信息（含原子状态与序列号） */
struct IpcShmSlotHeader {
    std::atomic<uint32_t> state; /**< 槽位状态（IpcShmSlotState） */
    std::atomic<uint32_t> seq;   /**< 帧序列号，用于消费者识别最新帧 */
    uint64_t timestamp_us;       /**< 帧时间戳（微秒） */
    uint32_t reserved;           /**< 保留字段，用于未来扩展 */
};

/**
 * 规范化 Y 平面行步长：若外部未提供有效 stride_y，则回退到图像宽度。
 * @param width    图像宽度（像素）
 * @param stride_y 外部传入的 stride_y，可能为 0
 * @return 有效的 stride_y 值
 */
inline uint32_t ipc_shm_normalize_stride_y(uint32_t width, uint32_t stride_y)
{
    return stride_y > 0 ? stride_y : width;
}

/**
 * 根据像素格式计算单帧图像的 payload 字节数。
 * @param fmt      像素格式
 * @param stride_y Y 平面行步长
 * @param height   图像高度
 * @return 单帧 payload 大小；不支持的格式返回 0
 */
inline size_t ipc_shm_slot_payload_bytes(IpcShmPixelFormat fmt, uint32_t stride_y,
                                         uint32_t height)
{
    switch (fmt) {
    case IpcShmPixelFormat::YUV420SP:
        return static_cast<size_t>(stride_y) * height * 3u / 2u;
    default:
        return 0;
    }
}

/** 共享内存环形缓冲区的整体布局计算结果 */
struct IpcShmLayout {
    size_t total_bytes         = 0; /**< 共享内存总字节数 */
    size_t header_offset       = 0; /**< 全局头部起始偏移 */
    size_t slots_offset        = 0; /**< 槽位头部数组起始偏移 */
    size_t payload_offset      = 0; /**< 图像 payload 区域起始偏移 */
    uint32_t slot_stride_bytes = 0; /**< 单个槽位 payload 字节数 */
};

/**
 * 根据创建参数计算共享内存的整体布局（各区域偏移与总大小）。
 * @param params 创建参数
 * @return 布局信息；参数非法时返回全零结构
 */
IpcShmLayout ipc_shm_compute_layout(const IpcShmRingCreateParams& params);

/**
 * 将创建参数与布局信息填充到全局头部结构中。
 * @param hdr    待填充的全局头部
 * @param params 创建参数
 * @param layout 已计算好的布局
 */
void ipc_shm_fill_global_header(IpcShmGlobalHeader& hdr,
                                const IpcShmRingCreateParams& params,
                                const IpcShmLayout& layout);

/**
 * 校验全局头部的魔数、版本号及关键字段是否合法。
 * @param hdr 待校验的全局头部
 * @return 合法返回 true，否则返回 false
 */
bool ipc_shm_validate_global_header(const IpcShmGlobalHeader& hdr);

/**
 * 从指定槽位构造帧视图（包含 Y/UV 平面指针与大小）。
 * @param hdr         全局头部（含分辨率、格式等）
 * @param payload_base payload 区域基地址
 * @param slot_index  槽位索引
 * @param slot        槽位头部（含 seq、timestamp）
 * @param out         输出帧视图结构
 */
void ipc_shm_frame_view_from_slot(const IpcShmGlobalHeader& hdr,
                                  const uint8_t* payload_base,
                                  uint32_t slot_index,
                                  const IpcShmSlotHeader& slot,
                                  IpcShmFrameView& out);

/**
 * 计算指定槽位在 payload 区域中的可写数据指针。
 * @param payload_base payload 区域基地址
 * @param slot_stride  单个槽位 payload 字节数
 * @param slot_index   槽位索引
 * @return 该槽位的数据指针；payload_base 为空则返回 nullptr
 */
uint8_t* ipc_shm_slot_payload_ptr(uint8_t* payload_base, uint32_t slot_stride,
                                  uint32_t slot_index);

/**
 * 计算指定槽位在 payload 区域中的只读数据指针。
 * @param payload_base payload 区域基地址
 * @param slot_stride  单个槽位 payload 字节数
 * @param slot_index   槽位索引
 * @return 该槽位的数据指针；payload_base 为空则返回 nullptr
 */
const uint8_t* ipc_shm_slot_payload_ptr(const uint8_t* payload_base,
                                        uint32_t slot_stride,
                                        uint32_t slot_index);

} // namespace upc_ipc

#endif
