#ifndef BUFFER_POOL_HPP
#define BUFFER_POOL_HPP

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace middleware {

using BufferId = uint32_t;
constexpr BufferId INVALID_BUFFER_ID = static_cast<BufferId>(-1);

// 缓冲区条目（内存由 MemPool 固定档块提供，避免堆上变长 new[] 造成碎片）
struct BufferEntry {
    uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t ref_count = 0;
    bool used = false;
};

// 缓冲区池
class BufferPool {
public:
    struct Stats {
        size_t capacity = 0;
        size_t used_count = 0;
        size_t bytes_used = 0;
    };

    static BufferPool& instance();
    
    bool init(size_t max_buffers = MEM_POOL_MAX_BUFFERS);
    void deinit();
    
    // 分配缓冲区
    BufferId allocate(size_t size);
    
    // 引用计数管理
    void acquire(BufferId id);
    void release(BufferId id);
    
    // 访问
    [[nodiscard]] uint8_t* get_ptr(BufferId id);
    [[nodiscard]] size_t get_size(BufferId id);
    [[nodiscard]] uint32_t get_ref_count(BufferId id);
    [[nodiscard]] bool is_valid(BufferId id) const;
    [[nodiscard]] Stats get_stats() const;
    
private:
    BufferPool() = default;
    ~BufferPool() = default;
    
    std::vector<BufferEntry> buffers_;
    mutable std::mutex mutex_;
    bool initialized_ = false;

    Stats get_stats_locked() const;
};

// RAII 缓冲区句柄
class BufferHandle {
public:
    BufferHandle() = default;
    explicit BufferHandle(BufferId id);
    ~BufferHandle();
    
    // 移动语义
    BufferHandle(BufferHandle&& other) noexcept;
    BufferHandle& operator=(BufferHandle&& other) noexcept;
    
    // 禁止拷贝
    BufferHandle(const BufferHandle&) = delete;
    BufferHandle& operator=(const BufferHandle&) = delete;
    
    [[nodiscard]] BufferId id() const noexcept;
    [[nodiscard]] uint8_t* data() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] bool valid() const noexcept;
    
    void reset(BufferId id = INVALID_BUFFER_ID);
    BufferId detach() noexcept;
    
private:
    BufferId id_ = INVALID_BUFFER_ID;
};

} // namespace middleware

#endif