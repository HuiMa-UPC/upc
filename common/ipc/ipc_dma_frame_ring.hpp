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
    /**
     * 全环帧号发号器：已发出的帧号总数，commit_slot 每次 fetch_add(1)。
     *
     * **seq 必须全环单调，不能按槽各自计数。** 消费者要跨槽比较 seq 才能
     * 挑出最新一帧，又要拿单个 last_seq_ 水位过滤已消费的帧——两者都建立在
     * "seq 在整个环里可比"这个前提上。曾经按槽计数，环只轮转一个槽时恰好
     * 等价、看不出问题；一旦轮转两个槽，两个计数器停在同一数值，
     * `seq > last_seq_` 就会把其中一路整个拒掉，**实测正好丢一半帧**，
     * 而且没有任何错误计数（被拒的帧由步骤 4 当作过期槽扫成 EMPTY）。
     */
    std::atomic<uint32_t> seq_next;
    uint32_t reserved0;
    uint64_t reserved[1];
};

struct IpcDmaFrameSlotHeader {
    std::atomic<uint32_t> state;
    /** 本槽当前持有帧的全环帧号（由 IpcDmaFrameGlobalHeader::seq_next 发号） */
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
     * @return 本帧的全环帧号（从 1 起增，全环单调）；失败返回 0。
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

    /**
     * @brief 把仍停在 Ready、还没有消费者读的槽收成 Empty。
     *
     * 只 CAS Ready→Empty。已经是 Reading 的槽表示消费者正拿着这块
     * isolation buffer，失败并留给调用方继续等，不能提前还池。
     * @return true 槽已收成 Empty；false 槽不是 Ready 或索引非法。
     */
    bool abandon_unread_slot(uint32_t idx);

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
