#include "ipc_dma_frame_ring.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace upc_ipc {

namespace {

constexpr uint32_t kMagic   = 0x44504156u; // "DPAV"
constexpr uint32_t kVersion = 1u;

constexpr uint32_t kStateEmpty    = 0u;
constexpr uint32_t kStateWriting  = 1u;
constexpr uint32_t kStateReady    = 2u;
constexpr uint32_t kStateReading  = 3u;

int shm_open_name(const char* name, bool create)
{
    const int flags = create ? (O_CREAT | O_RDWR | O_EXCL) : O_RDWR;
    return shm_open(name, flags, 0666);
}

} // namespace

// ============================================================
// Producer
// ============================================================
IpcDmaFrameRingProducer::IpcDmaFrameRingProducer() = default;

IpcDmaFrameRingProducer::~IpcDmaFrameRingProducer()
{
    destroy();
}

IpcShmError IpcDmaFrameRingProducer::create(const char* name, uint32_t width,
                                            uint32_t height, uint32_t stride_y,
                                            uint32_t format,
                                            uint32_t slot_count)
{
    if(!name || slot_count == 0 || width == 0 || height == 0 || stride_y == 0) {
        return IpcShmError::INVALID_ARGUMENT;
    }
    if(base_) {
        return IpcShmError::ALREADY_OPEN;
    }

    // 先删除旧 SHM（忽略错误）
    shm_unlink(name);
    name_ = name;

    shm_fd_ = shm_open_name(name, true);
    if(shm_fd_ < 0) {
        return IpcShmError::SHM_OPEN_FAILED;
    }

    const size_t slot_area = sizeof(IpcDmaFrameSlotHeader) * slot_count;
    map_size_ = sizeof(IpcDmaFrameGlobalHeader) + slot_area;
    if(ftruncate(shm_fd_, static_cast<off_t>(map_size_)) != 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        shm_unlink(name);
        return IpcShmError::FTRUNCATE_FAILED;
    }

    void* p = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                   shm_fd_, 0);
    if(p == MAP_FAILED) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        shm_unlink(name);
        return IpcShmError::MMAP_FAILED;
    }

    base_   = static_cast<uint8_t*>(p);
    global_ = reinterpret_cast<IpcDmaFrameGlobalHeader*>(base_);
    slots_  = reinterpret_cast<IpcDmaFrameSlotHeader*>(
        base_ + sizeof(IpcDmaFrameGlobalHeader));

    std::memset(base_, 0, map_size_);

    global_->magic            = kMagic;
    global_->version          = kVersion;
    global_->width            = width;
    global_->height           = height;
    global_->stride_y         = stride_y;
    global_->format           = format;
    global_->slot_count       = slot_count;
    global_->slot_header_size = sizeof(IpcDmaFrameSlotHeader);

    return IpcShmError::OK;
}

void IpcDmaFrameRingProducer::destroy()
{
    if(base_) {
        munmap(base_, map_size_);
        base_   = nullptr;
        global_ = nullptr;
        slots_  = nullptr;
        map_size_ = 0;
    }
    if(shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
    if(!name_.empty()) {
        shm_unlink(name_.c_str());
        name_.clear();
    }
}

std::optional<uint32_t> IpcDmaFrameRingProducer::acquire_write_slot()
{
    if(!slots_) {
        return std::nullopt;
    }
    const uint32_t n = global_->slot_count;
    // 简单轮询：寻找 Empty 并置为 Writing
    for(uint32_t i = 0; i < n; ++i) {
        uint32_t expected = kStateEmpty;
        if(slots_[i].state.compare_exchange_strong(expected, kStateWriting,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
            return i;
        }
    }
    return std::nullopt;
}

void IpcDmaFrameRingProducer::cancel_write_slot(uint32_t idx)
{
    if(!slots_ || idx >= global_->slot_count) {
        return;
    }
    uint32_t expected = kStateWriting;
    slots_[idx].state.compare_exchange_strong(expected, kStateEmpty,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
}

uint32_t IpcDmaFrameRingProducer::commit_slot(uint32_t idx, uint64_t timestamp_us,
                                              uint32_t buffer_idx)
{
    if(!slots_ || idx >= global_->slot_count) {
        return 0;
    }
    auto& slot = slots_[idx];
    const uint32_t cur = slot.state.load(std::memory_order_acquire);
    if(cur != kStateWriting) {
        return 0;
    }

    slot.timestamp_us = timestamp_us;
    slot.buffer_idx   = buffer_idx;
    const uint32_t new_seq = slot.seq.load(std::memory_order_relaxed) + 1u;
    slot.seq.store(new_seq, std::memory_order_release);
    slot.state.store(kStateReady, std::memory_order_release);
    return new_seq;
}

bool IpcDmaFrameRingProducer::wait_slot_empty(uint32_t idx,
                                              int timeout_ms) const
{
    if(!slots_ || idx >= global_->slot_count) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    for(;;) {
        const uint32_t state =
            slots_[idx].state.load(std::memory_order_acquire);
        if(state == kStateEmpty) {
            return true;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
        if(timeout_ms > 0 && elapsed >= timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

uint32_t IpcDmaFrameRingProducer::slot_count() const
{
    return global_ ? global_->slot_count : 0;
}

// ============================================================
// Consumer
// ============================================================
IpcDmaFrameRingConsumer::IpcDmaFrameRingConsumer() = default;

IpcDmaFrameRingConsumer::~IpcDmaFrameRingConsumer()
{
    close();
}

IpcShmError IpcDmaFrameRingConsumer::open(const char* name)
{
    if(!name) {
        return IpcShmError::INVALID_ARGUMENT;
    }
    if(base_) {
        return IpcShmError::ALREADY_OPEN;
    }

    shm_fd_ = shm_open_name(name, false);
    if(shm_fd_ < 0) {
        return IpcShmError::SHM_OPEN_FAILED;
    }

    struct stat st {};
    if(fstat(shm_fd_, &st) != 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        return IpcShmError::SHM_OPEN_FAILED;
    }
    map_size_ = static_cast<size_t>(st.st_size);

    void* p = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                   shm_fd_, 0);
    if(p == MAP_FAILED) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        return IpcShmError::MMAP_FAILED;
    }

    base_   = static_cast<uint8_t*>(p);
    global_ = reinterpret_cast<const IpcDmaFrameGlobalHeader*>(base_);
    slots_  = reinterpret_cast<IpcDmaFrameSlotHeader*>(
        base_ + sizeof(IpcDmaFrameGlobalHeader));

    if(global_->magic != kMagic || global_->version != kVersion) {
        munmap(base_, map_size_);
        base_   = nullptr;
        global_ = nullptr;
        slots_  = nullptr;
        map_size_ = 0;
        ::close(shm_fd_);
        shm_fd_ = -1;
        return IpcShmError::VERSION_MISMATCH;
    }

    last_seq_ = 0;
    return IpcShmError::OK;
}

void IpcDmaFrameRingConsumer::close()
{
    if(base_) {
        munmap(base_, map_size_);
        base_   = nullptr;
        global_ = nullptr;
        slots_  = nullptr;
        map_size_ = 0;
    }
    if(shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
    last_seq_ = 0;
}

/**
 * @brief 消费者取「当前环里最新的一帧」元数据视图。
 *
 * 背景（和像素的关系）：
 *   - SHM 环里只有槽位状态 / seq / buffer_idx 等元数据，没有 NV12 像素。
 *   - 像素在 Camera 通过 IPC 传来的 dma_buf fd 表里；本函数返回的
 *     view.buffer_idx 用来去那张表里找对应 fd。
 *
 * 状态机（槽位 state）：
 *   EMPTY(0) → 空闲，Producer 可写
 *   WRITING(1) → Producer 正在填
 *   READY(2) → 已提交，Consumer 可取
 *   READING(3) → Consumer 正在用（本函数抢到后进入此态）
 *   用完后须调用 release_slot()：READING → EMPTY，否则 Producer 堵死。
 *
 * 「latest」策略：
 *   扫一遍所有 READY 槽，选 seq 最大且大于 last_seq_ 的那一格。
 *   中间更旧的 READY 槽会被直接标回 EMPTY（丢帧换低延迟）。
 *
 * @return 成功返回 IpcDmaFrameView；无可用帧 / 抢锁失败返回 std::nullopt。
 */
std::optional<IpcDmaFrameView> IpcDmaFrameRingConsumer::acquire_latest_frame()
{
    /* 环未 open（mmap 失败或已 close）：slots_ 为空指针。 */
    if(!slots_) {
        return std::nullopt;
    }

    const uint32_t n = global_->slot_count;
    uint32_t best_idx = n;       /* 哨兵：>=n 表示还没找到候选 */
    uint32_t best_seq = last_seq_; /* 只认比「上次已消费 seq」更新的帧 */

    /*
     * [步骤 1] 扫描：在所有 READY 槽里找 seq 最大者。
     * acquire 读 state/seq，保证看见 Producer commit 时 release 写入的内容。
     */
    for(uint32_t i = 0; i < n; ++i) {
        const uint32_t state = slots_[i].state.load(std::memory_order_acquire);
        if(state != kStateReady) {
            continue; /* 空闲 / 正在写 / 已被别人读：跳过 */
        }
        const uint32_t seq = slots_[i].seq.load(std::memory_order_acquire);
        if(seq > best_seq) {
            best_seq = seq;
            best_idx = i;
        }
    }

    /* 没有任何「比 last_seq_ 更新的 READY 帧」：调用方应稍后重试。 */
    if(best_idx >= n) {
        return std::nullopt;
    }

    /*
     * [步骤 2] CAS：READY → READING，独占这一槽。
     * 若失败（另一消费者抢走，或 Producer 状态已变），返回空，让上层重试。
     * compare_exchange_strong：期望值 expected 必须仍是 READY 才改成 READING。
     */
    uint32_t expected = kStateReady;
    if(!slots_[best_idx].state.compare_exchange_strong(
           expected, kStateReading, std::memory_order_acq_rel,
           std::memory_order_relaxed)) {
        return std::nullopt;
    }

    /*
     * [步骤 3] 组装对外视图（只含元数据指针与索引，不含像素拷贝）。
     * buffer_idx：对应 Camera 侧 V4L2/dma 缓冲编号，UI 用 dma_fds_[buffer_idx]。
     */
    auto& slot = slots_[best_idx];
    IpcDmaFrameView view;
    view.global       = global_;
    view.slot         = &slot;
    view.slot_index   = best_idx;
    view.seq          = slot.seq.load(std::memory_order_acquire);
    view.timestamp_us = slot.timestamp_us;
    view.buffer_idx   = slot.buffer_idx;
    last_seq_         = view.seq; /* 下次只接受更新的帧，避免重复消费同一 seq */

    /*
     * [步骤 4] 清扫过期 READY 槽。
     * 预览只要最新一帧：其它 READY 且 seq<=当前帧 的槽直接标 EMPTY，
     * 让 Producer 的 wait_slot_empty / 复用逻辑尽快拿回缓冲。
     * 若不做这一步，旧帧会堆在环里，Camera 侧容易卡在等 EMPTY。
     */
    for(uint32_t i = 0; i < n; ++i) {
        if(i == best_idx) {
            continue; /* 当前正在 READING 的槽留给 release_slot 归还 */
        }
        const uint32_t s = slots_[i].state.load(std::memory_order_acquire);
        if(s != kStateReady) {
            continue;
        }
        const uint32_t seq = slots_[i].seq.load(std::memory_order_acquire);
        if(seq <= view.seq) {
            slots_[i].state.store(kStateEmpty, std::memory_order_release);
        }
    }

    return view;
}

void IpcDmaFrameRingConsumer::release_slot(uint32_t idx)
{
    if(!slots_ || idx >= global_->slot_count) {
        return;
    }
    uint32_t expected = kStateReading;
    slots_[idx].state.compare_exchange_strong(expected, kStateEmpty,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
}

uint32_t IpcDmaFrameRingConsumer::slot_count() const
{
    return global_ ? global_->slot_count : 0;
}

uint32_t IpcDmaFrameRingConsumer::width() const
{
    return global_ ? global_->width : 0;
}

uint32_t IpcDmaFrameRingConsumer::height() const
{
    return global_ ? global_->height : 0;
}

uint32_t IpcDmaFrameRingConsumer::stride_y() const
{
    return global_ ? global_->stride_y : 0;
}

uint32_t IpcDmaFrameRingConsumer::format() const
{
    return global_ ? global_->format : 0;
}

} // namespace upc_ipc
