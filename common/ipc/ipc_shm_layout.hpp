#ifndef UPC_IPC_SHM_LAYOUT_HPP
#define UPC_IPC_SHM_LAYOUT_HPP

#include "ipc_shm_types.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace upc_ipc {

constexpr uint32_t IPC_SHM_MAGIC   = 0x55485053u; /* 'UPHS' */
constexpr uint32_t IPC_SHM_VERSION = 1u;

#pragma pack(push, 1)
struct IpcShmGlobalHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_stride_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t stride_y;
    uint32_t format; /**< IpcShmPixelFormat */
    uint32_t header_bytes;
    uint32_t slots_bytes;
    uint32_t payload_bytes;
};
#pragma pack(pop)

struct IpcShmSlotHeader {
    std::atomic<uint32_t> state; /**< IpcShmSlotState */
    std::atomic<uint32_t> seq;
    uint64_t timestamp_us;
    uint32_t reserved;
};

inline uint32_t ipc_shm_normalize_stride_y(uint32_t width, uint32_t stride_y)
{
    return stride_y > 0 ? stride_y : width;
}

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

struct IpcShmLayout {
    size_t total_bytes    = 0;
    size_t header_offset  = 0;
    size_t slots_offset   = 0;
    size_t payload_offset = 0;
    uint32_t slot_stride_bytes = 0;
};

IpcShmLayout ipc_shm_compute_layout(const IpcShmRingCreateParams& params);

void ipc_shm_fill_global_header(IpcShmGlobalHeader& hdr,
                                const IpcShmRingCreateParams& params,
                                const IpcShmLayout& layout);

bool ipc_shm_validate_global_header(const IpcShmGlobalHeader& hdr);

void ipc_shm_frame_view_from_slot(const IpcShmGlobalHeader& hdr,
                                  const uint8_t* payload_base,
                                  uint32_t slot_index,
                                  const IpcShmSlotHeader& slot,
                                  IpcShmFrameView& out);

uint8_t* ipc_shm_slot_payload_ptr(uint8_t* payload_base, uint32_t slot_stride,
                                  uint32_t slot_index);

const uint8_t* ipc_shm_slot_payload_ptr(const uint8_t* payload_base,
                                        uint32_t slot_stride,
                                        uint32_t slot_index);

} // namespace upc_ipc

#endif
