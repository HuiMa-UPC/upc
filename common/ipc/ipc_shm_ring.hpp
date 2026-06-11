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
    /** 析构函数：若仍激活则自动销毁共享内存 */
    ~IpcShmRingProducer();

    IpcShmRingProducer(const IpcShmRingProducer&) = delete;
    IpcShmRingProducer& operator=(const IpcShmRingProducer&) = delete;

    /**
     * 创建共享内存环形缓冲区并初始化所有槽位。
     * @param params 创建参数（名称、分辨率、slot 数量等）
     * @return 成功返回 Ok，否则返回对应错误码
     */
    IpcShmError create(const IpcShmRingCreateParams& params);
    /** 销毁共享内存区域，若为创建者则同时执行 unlink */
    void destroy();

    /** 判断当前生产者是否处于可用状态 */
    [[nodiscard]] bool active() const noexcept { return active_; }
    /** 获取全局头指针（包含分辨率、slot 数量等元信息） */
    [[nodiscard]] const IpcShmGlobalHeader* header() const noexcept
    {
        return hdr_;
    }

    /**
     * 从所有 Empty 状态的槽位中抢占一个，并将其置为 Writing。
     * @return 成功返回槽位索引，否则返回 std::nullopt
     */
    std::optional<uint32_t> acquire_write_slot();
    /**
     * 取消对指定槽位的写操作，将其状态回滚为 Empty。
     * @param slot_index 槽位索引
     */
    void cancel_write_slot(uint32_t slot_index);
    /**
     * 提交指定槽位的数据，写入时间戳并递增序列号，将状态置为 Ready。
     * @param slot_index 槽位索引
     * @param timestamp_us 帧时间戳（微秒）
     * @return 该槽位的新序列号
     */
    uint32_t commit_slot(uint32_t slot_index, uint64_t timestamp_us);
    /**
     * 获取指定槽位的有效负载（图像数据）指针。
     * @param slot_index 槽位索引
     * @return 指向该槽位 payload 的指针；不可用则返回 nullptr
     */
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
    /** 析构函数：若仍激活则自动关闭共享内存 */
    ~IpcShmRingConsumer();

    IpcShmRingConsumer(const IpcShmRingConsumer&) = delete;
    IpcShmRingConsumer& operator=(const IpcShmRingConsumer&) = delete;

    /**
     * 打开已存在的共享内存环形缓冲区。
     * @param name 共享内存对象名称
     * @return 成功返回 Ok，否则返回对应错误码
     */
    IpcShmError open(const char* name);
    /** 关闭共享内存映射 */
    void close();

    /** 判断当前消费者是否处于可用状态 */
    [[nodiscard]] bool active() const noexcept { return active_; }
    /** 获取全局头指针（包含分辨率、slot 数量等元信息） */
    [[nodiscard]] const IpcShmGlobalHeader* header() const noexcept
    {
        return hdr_;
    }

    /**
     * 扫描所有 Ready 状态的槽位，选取序列号最新的一帧并将其置为 Reading。
     * @return 成功返回帧视图，否则返回 std::nullopt
     */
    std::optional<IpcShmFrameView> acquire_latest_frame();
    /**
     * 释放已读取的槽位，将其状态重置为 Empty，供生产者再次使用。
     * @param slot_index 槽位索引
     */
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
