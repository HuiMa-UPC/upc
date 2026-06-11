#include "ipc_shm_layout.hpp"

#include <cstring>

namespace upc_ipc {

namespace {

/** 将 value 向上对齐到 alignment 的整数倍（alignment 须为 2 的幂） */
size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

/**
 * 计算共享内存布局：
 * 1. 根据格式计算单帧 payload 大小；
 * 2. 按对齐要求依次排布 GlobalHeader、SlotHeader[]、Payload[]；
 * 3. 汇总总字节数。
 */
IpcShmLayout ipc_shm_compute_layout(const IpcShmRingCreateParams& params)
{
    IpcShmLayout layout{};
    if (!params.name || params.width == 0 || params.height == 0 ||
        params.slot_count == 0) {
        return layout;
    }

    const uint32_t stride_y =
        ipc_shm_normalize_stride_y(params.width, params.stride_y);
    layout.slot_stride_bytes = static_cast<uint32_t>(
        ipc_shm_slot_payload_bytes(params.format, stride_y, params.height));
    if (layout.slot_stride_bytes == 0) {
        return layout;
    }

    layout.header_offset = 0;
    layout.slots_offset =
        align_up(sizeof(IpcShmGlobalHeader), alignof(IpcShmSlotHeader));
    layout.payload_offset =
        layout.slots_offset + sizeof(IpcShmSlotHeader) * params.slot_count;
    layout.payload_offset = align_up(layout.payload_offset, 64u);
    layout.total_bytes =
        layout.payload_offset +
        static_cast<size_t>(layout.slot_stride_bytes) * params.slot_count;

    return layout;
}

/**
 * 用创建参数与布局结果填充全局头部，供消费者读取以重建帧信息。
 * 调用前会先将 hdr 清零。
 */
void ipc_shm_fill_global_header(IpcShmGlobalHeader& hdr,
                                const IpcShmRingCreateParams& params,
                                const IpcShmLayout& layout)
{
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.magic             = IPC_SHM_MAGIC;
    hdr.version           = IPC_SHM_VERSION;
    hdr.slot_count        = params.slot_count;
    hdr.slot_stride_bytes = layout.slot_stride_bytes;
    hdr.width             = params.width;
    hdr.height            = params.height;
    hdr.stride_y =
        ipc_shm_normalize_stride_y(params.width, params.stride_y);
    hdr.format       = static_cast<uint32_t>(params.format);
    hdr.header_bytes = static_cast<uint32_t>(sizeof(IpcShmGlobalHeader));
    hdr.slots_bytes =
        static_cast<uint32_t>(sizeof(IpcShmSlotHeader) * params.slot_count);
    hdr.payload_bytes =
        static_cast<uint32_t>(layout.slot_stride_bytes * params.slot_count);
}

/**
 * 校验全局头部：检查魔数、版本、分辨率、slot 数量及支持的像素格式。
 */
bool ipc_shm_validate_global_header(const IpcShmGlobalHeader& hdr)
{
    if (hdr.magic != IPC_SHM_MAGIC || hdr.version != IPC_SHM_VERSION) {
        return false;
    }
    if (hdr.slot_count == 0 || hdr.slot_stride_bytes == 0) {
        return false;
    }
    if (hdr.width == 0 || hdr.height == 0 || hdr.stride_y == 0) {
        return false;
    }
    return static_cast<IpcShmPixelFormat>(hdr.format) ==
           IpcShmPixelFormat::YUV420SP;
}

/**
 * 根据槽位信息填充帧视图：解析 Y/UV 平面地址与大小，并拷贝元数据（seq、timestamp 等）。
 * 若 payload_base 无效，则仅填充元数据，平面指针保持为空。
 */
void ipc_shm_frame_view_from_slot(const IpcShmGlobalHeader& hdr,
                                  const uint8_t* payload_base,
                                  uint32_t slot_index,
                                  const IpcShmSlotHeader& slot,
                                  IpcShmFrameView& out)
{
    out = {};
    out.slot_index   = slot_index;
    out.seq          = slot.seq.load(std::memory_order_acquire);
    out.timestamp_us = slot.timestamp_us;
    out.width        = hdr.width;
    out.height       = hdr.height;
    out.stride_y     = hdr.stride_y;
    out.format       = static_cast<IpcShmPixelFormat>(hdr.format);

    const uint8_t* base =
        ipc_shm_slot_payload_ptr(payload_base, hdr.slot_stride_bytes, slot_index);
    if (!base) {
        return;
    }

    const size_t y_size = static_cast<size_t>(hdr.stride_y) * hdr.height;
    out.y_plane          = base;
    out.y_size           = y_size;
    out.uv_plane         = base + y_size;
    out.uv_size          = y_size / 2u;
}

/** 计算指定槽位在 payload 区域中的可写地址 */
uint8_t* ipc_shm_slot_payload_ptr(uint8_t* payload_base, uint32_t slot_stride,
                                  uint32_t slot_index)
{
    if (!payload_base) {
        return nullptr;
    }
    return payload_base + static_cast<size_t>(slot_stride) * slot_index;
}

/** 计算指定槽位在 payload 区域中的只读地址 */
const uint8_t* ipc_shm_slot_payload_ptr(const uint8_t* payload_base,
                                        uint32_t slot_stride,
                                        uint32_t slot_index)
{
    if (!payload_base) {
        return nullptr;
    }
    return payload_base + static_cast<size_t>(slot_stride) * slot_index;
}

} // namespace upc_ipc
