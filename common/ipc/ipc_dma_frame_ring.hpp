#ifndef UPC_IPC_DMA_FRAME_RING_HPP
#define UPC_IPC_DMA_FRAME_RING_HPP

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace upc_ipc {

/** 统一使用 NV12 fourcc（与 V4L2_PIX_FMT_NV12 数值相同） */
constexpr uint32_t kDmaFrameFormatNv12 = 0x3231564eu;

enum class IpcShmError : int {
    OK = 0,
    INVALID_ARGUMENT = -1,
    ALREADY_OPEN = -2,
    SHM_OPEN_FAILED = -3,
    FTRUNCATE_FAILED = -4,
    MMAP_FAILED = -5,
    FORMAT_MISMATCH = -6,
    VERSION_MISMATCH = -7,
    NOT_OPEN = -8,
    TIMEOUT = -9,
};

struct IpcDmaFrameGlobalHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride_y;
    uint32_t format;      // fourcc
    uint32_t slot_count;
    uint32_t slot_header_size;
    uint64_t reserved[2];
};

struct IpcDmaFrameSlotHeader {
    std::atomic<uint32_t> state;
    std::atomic<uint32_t> seq;
    uint64_t timestamp_us;
    uint32_t buffer_idx;
    uint32_t reserved;
};

struct IpcDmaFrameView {
    const IpcDmaFrameGlobalHeader* global {nullptr};
    const IpcDmaFrameSlotHeader* slot {nullptr};
    uint32_t slot_index {0};
    uint32_t seq {0};
    uint64_t timestamp_us {0};
    uint32_t buffer_idx {0};

    uint32_t width() const { return global ? global->width : 0; }
    uint32_t height() const { return global ? global->height : 0; }
    uint32_t stride_y() const { return global ? global->stride_y : 0; }
    uint32_t format() const { return global ? global->format : 0; }
};

class IpcDmaFrameRingProducer {
public:
    IpcDmaFrameRingProducer();
    ~IpcDmaFrameRingProducer();

    IpcShmError create(const char* name, uint32_t width, uint32_t height,
                       uint32_t stride_y, uint32_t format,
                       uint32_t slot_count);
    void destroy();
    bool is_open() const { return base_ != nullptr; }

    std::optional<uint32_t> acquire_write_slot();
    void cancel_write_slot(uint32_t idx);
    /**
     * @brief 提交 Writing→Ready。
     * @return 该 slot 新 seq（从 1 起增）；失败返回 0。
     */
    uint32_t commit_slot(uint32_t idx, uint64_t timestamp_us, uint32_t buffer_idx);

    /**
     * @brief 非阻塞查询某个 slot 是否已被 Consumer 释放为 Empty。
     *
     * 供 Producer 做"在飞帧"回收：不必逐帧阻塞等待，可先扫一遍已完成的
     * 帧把底层 buffer 收回，只在窗口耗尽时才退化为 wait_slot_empty。
     * @return true 已是 Empty；false 仍被消费者占用或 slot 索引非法。
     */
    bool slot_is_empty(uint32_t idx) const;

    /**
     * @brief 等待某个 slot 被 Consumer 释放为 Empty（带超时）。
     * @return true 成功等到，false 超时或 slot 索引非法。
     */
    bool wait_slot_empty(uint32_t idx, int timeout_ms) const;

    uint32_t slot_count() const;

private:
    int shm_fd_ {-1};
    IpcDmaFrameGlobalHeader* global_ {nullptr};
    IpcDmaFrameSlotHeader* slots_ {nullptr};
    uint8_t* base_ {nullptr};
    size_t map_size_ {0};
    std::string name_;
};

class IpcDmaFrameRingConsumer {
public:
    IpcDmaFrameRingConsumer();
    ~IpcDmaFrameRingConsumer();

    IpcShmError open(const char* name);
    void close();
    bool is_open() const { return base_ != nullptr; }

    std::optional<IpcDmaFrameView> acquire_latest_frame();
    void release_slot(uint32_t idx);

    uint32_t slot_count() const;
    uint32_t width() const;
    uint32_t height() const;
    uint32_t stride_y() const;
    uint32_t format() const;

private:
    int shm_fd_ {-1};
    const IpcDmaFrameGlobalHeader* global_ {nullptr};
    IpcDmaFrameSlotHeader* slots_ {nullptr};
    uint8_t* base_ {nullptr};
    size_t map_size_ {0};
    uint32_t last_seq_ {0};
};

} // namespace upc_ipc

#endif /* UPC_IPC_DMA_FRAME_RING_HPP */
