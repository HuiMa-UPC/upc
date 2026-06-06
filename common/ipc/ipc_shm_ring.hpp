#ifndef UPC_IPC_SHM_RING_HPP
#define UPC_IPC_SHM_RING_HPP

#include "ipc_shm_region.hpp"
#include "ipc_shm_layout.hpp"
#include "ipc_shm_types.hpp"

#include <cstdint>
#include <optional>

namespace upc_ipc {

/** 写入端：camera 进程创建并生产帧 */
class IpcShmRingProducer {
public:
    IpcShmRingProducer() = default;
    ~IpcShmRingProducer();

    IpcShmRingProducer(const IpcShmRingProducer&) = delete;
    IpcShmRingProducer& operator=(const IpcShmRingProducer&) = delete;

    IpcShmError create(const IpcShmRingCreateParams& params);
    void destroy();

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const IpcShmGlobalHeader* header() const noexcept
    {
        return hdr_;
    }

    std::optional<uint32_t> acquire_write_slot();
    void cancel_write_slot(uint32_t slot_index);
    uint32_t commit_slot(uint32_t slot_index, uint64_t timestamp_us);
    [[nodiscard]] uint8_t* slot_payload(uint32_t slot_index);

private:
    IpcShmRegion region_;
    IpcShmLayout layout_{};
    IpcShmRingCreateParams params_{};
    IpcShmGlobalHeader* hdr_ = nullptr;
    IpcShmSlotHeader* slots_ = nullptr;
    uint8_t* payload_base_ = nullptr;
    bool active_ = false;
};

/** 读取端：UI 进程打开并消费帧 */
class IpcShmRingConsumer {
public:
    IpcShmRingConsumer() = default;
    ~IpcShmRingConsumer();

    IpcShmRingConsumer(const IpcShmRingConsumer&) = delete;
    IpcShmRingConsumer& operator=(const IpcShmRingConsumer&) = delete;

    IpcShmError open(const char* name);
    void close();

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const IpcShmGlobalHeader* header() const noexcept
    {
        return hdr_;
    }

    std::optional<IpcShmFrameView> acquire_latest_frame();
    void release_slot(uint32_t slot_index);

private:
    IpcShmRegion region_;
    IpcShmGlobalHeader* hdr_ = nullptr;
    IpcShmSlotHeader* slots_ = nullptr;
    const uint8_t* payload_base_ = nullptr;
    bool active_ = false;
    uint32_t last_seen_seq_ = 0;
};

} // namespace upc_ipc

#endif
