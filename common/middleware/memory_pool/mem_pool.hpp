#ifndef MEM_POOL_HPP
#define MEM_POOL_HPP

#include <cstdint>
#include <cstddef>
#include <memory>
#include <array>
#include <vector>
#include <mutex>

namespace middleware {

// 内存块大小类别
enum class BlockClass : uint8_t {
    B64   = 0,
    B256  = 1,
    B1024 = 2,
    B4096 = 3,
    B16K  = 4
};

// 固定块分配器
class FixedBlockAllocator {
public:
    FixedBlockAllocator(size_t block_size, size_t num_blocks);
    ~FixedBlockAllocator();
    
    // 禁止拷贝，允许移动
    FixedBlockAllocator(const FixedBlockAllocator&) = delete;
    FixedBlockAllocator& operator=(const FixedBlockAllocator&) = delete;
    FixedBlockAllocator(FixedBlockAllocator&&) noexcept;
    FixedBlockAllocator& operator=(FixedBlockAllocator&&) noexcept;
    
    void* allocate();
    void deallocate(void* ptr);
    
    [[nodiscard]] size_t block_size() const noexcept;
    [[nodiscard]] size_t free_count() const noexcept;
    [[nodiscard]] size_t used_count() const noexcept;
    [[nodiscard]] size_t total_blocks() const noexcept;
    [[nodiscard]] bool owns(void* ptr) const noexcept;
    
private:
    struct Block {
        alignas(64) uint8_t data[1];  // 实际大小由 block_size_ 决定
    };
    
    size_t block_size_;
    size_t num_blocks_;
    std::unique_ptr<uint8_t[]> pool_;
    std::vector<bool> used_map_;
};

// 内存池
class MemPool {
public:
    struct ClassStats {
        size_t block_size = 0;
        size_t total_blocks = 0;
        size_t used_blocks = 0;
        size_t free_blocks = 0;
    };

    static MemPool& instance();
    
    bool init();
    void deinit();
    
    // 分配/释放
    void* allocate(size_t size);
    void deallocate(void* ptr);
    
    // 按类别分配
    void* allocate(BlockClass cls);
    
    [[nodiscard]] size_t get_block_size(void* ptr) const;
    [[nodiscard]] std::array<ClassStats, 5> get_stats() const;
    
private:
    MemPool() = default;
    ~MemPool() = default;
    
    std::array<std::unique_ptr<FixedBlockAllocator>, 5> allocators_;
    mutable std::mutex mutex_;
    bool initialized_ = false;
};

// RAII 内存块包装
class MemBlock {
public:
    MemBlock() = default;
    explicit MemBlock(void* ptr);
    ~MemBlock();
    
    // 移动语义
    MemBlock(MemBlock&& other) noexcept;
    MemBlock& operator=(MemBlock&& other) noexcept;
    
    // 禁止拷贝
    MemBlock(const MemBlock&) = delete;
    MemBlock& operator=(const MemBlock&) = delete;
    
    [[nodiscard]] void* get() const noexcept;
    [[nodiscard]] uint8_t* data() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    void release();
    
private:
    void* ptr_ = nullptr;
};

} // namespace middleware

#endif