#ifndef UPC_IPC_SHM_TYPES_HPP
#define UPC_IPC_SHM_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace upc_ipc {

/** 与 Rockchip RK_FMT_YUV420SP 对齐（NV12：Y 平面 + 交错 UV） */
enum class IpcShmPixelFormat : uint32_t {
    Unknown   = 0,
    YUV420SP  = 1,
};

enum class IpcShmSlotState : uint32_t {
    Empty    = 0,
    Writing  = 1,
    Ready    = 2,
    Reading  = 3,
};

enum class IpcShmError : int32_t {
    Ok              = 0,
    InvalidArgument = -1,
    IoError         = -2,
    MapFailed       = -3,
    BadMagic        = -4,
    BadVersion      = -5,
    NoSlot          = -6,
    NotReady        = -7,
    AlreadyExists   = -8,
};

/** 只读帧视图：指针指向共享内存，不拥有内存 */
struct IpcShmFrameView {
    uint32_t slot_index = 0;
    uint32_t seq        = 0;
    uint64_t timestamp_us = 0;

    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t stride_y = 0;
    IpcShmPixelFormat format = IpcShmPixelFormat::Unknown;

    const uint8_t* y_plane  = nullptr;
    const uint8_t* uv_plane = nullptr;
    size_t y_size           = 0;
    size_t uv_size          = 0;
};

struct IpcShmRingCreateParams {
    const char* name = nullptr;
    uint32_t width   = 0;
    uint32_t height  = 0;
    /** 0 表示 stride_y = width（紧凑） */
    uint32_t stride_y = 0;
    uint32_t slot_count = 3;
    IpcShmPixelFormat format = IpcShmPixelFormat::YUV420SP;
};

} // namespace upc_ipc

#endif
