#include "mem_pool.hpp"
#include "mw_debug.hpp"
#include <cstring>

namespace middleware {

// ============================================
// FixedBlockAllocator
// ============================================
FixedBlockAllocator::FixedBlockAllocator(size_t block_size, size_t num_blocks)
    : block_size_(std::max(block_size, sizeof(void*)))
    , num_blocks_(num_blocks)
    , pool_(std::make_unique<uint8_t[]>(block_size_ * num_blocks))
    , used_map_(num_blocks, false) {}

FixedBlockAllocator::~FixedBlockAllocator() = default;

FixedBlockAllocator::FixedBlockAllocator(FixedBlockAllocator&& other) noexcept
    : block_size_(other.block_size_)
    , num_blocks_(other.num_blocks_)
    , pool_(std::move(other.pool_))
    , used_map_(std::move(other.used_map_)) {
    other.block_size_ = 0;
    other.num_blocks_ = 0;
}

FixedBlockAllocator& FixedBlockAllocator::operator=(FixedBlockAllocator&& other) noexcept {
    if (this != &other) {
        block_size_ = other.block_size_;
        num_blocks_ = other.num_blocks_;
        pool_ = std::move(other.pool_);
        used_map_ = std::move(other.used_map_);
        other.block_size_ = 0;
        other.num_blocks_ = 0;
    }
    return *this;
}

void* FixedBlockAllocator::allocate() {
    for (size_t i = 0; i < num_blocks_; ++i) {
        if (!used_map_[i]) {
            used_map_[i] = true;
            return pool_.get() + (i * block_size_);
        }
    }
    return nullptr;
}

void FixedBlockAllocator::deallocate(void* ptr) {
    if (!ptr || !owns(ptr)) return;
    
    size_t idx = (static_cast<uint8_t*>(ptr) - pool_.get()) / block_size_;
    used_map_[idx] = false;
}

size_t FixedBlockAllocator::block_size() const noexcept {
    return block_size_;
}

size_t FixedBlockAllocator::free_count() const noexcept {
    size_t count = 0;
    for (bool used : used_map_) {
        if (!used) ++count;
    }
    return count;
}

size_t FixedBlockAllocator::used_count() const noexcept {
    return num_blocks_ - free_count();
}

size_t FixedBlockAllocator::total_blocks() const noexcept {
    return num_blocks_;
}

bool FixedBlockAllocator::owns(void* ptr) const noexcept {
    return ptr >= pool_.get() && ptr < (pool_.get() + block_size_ * num_blocks_);
}

// ============================================
// MemPool
// ============================================
MemPool& MemPool::instance() {
    static MemPool instance;
    return instance;
}

bool MemPool::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;
    
    const std::array<size_t, 5> sizes = {64, 256, 1024, 4096, 16384};
    
    for (size_t i = 0; i < 5; ++i) {
        allocators_[i] = std::make_unique<FixedBlockAllocator>(sizes[i], 16);
    }
    
    initialized_ = true;
#if MW_DEBUG_MEMPOOL
    MW_LOG_MEMPOOL("init ok (5 classes x 16 blocks)");
#endif
    return true;
}

void MemPool::deinit() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& alloc : allocators_) {
        alloc.reset();
    }
    initialized_ = false;
}

void* MemPool::allocate(size_t size) {
    void* ptr = nullptr;
    bool pool_full = false;
    bool system_heap = false;
#if MW_DEBUG_MEMPOOL
    size_t class_size = 0;
    size_t used = 0;
    size_t total = 0;
#endif

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return nullptr;
        }

        const std::array<size_t, 5> sizes = {64, 256, 1024, 4096, 16384};

        for (size_t i = 0; i < 5; ++i) {
            if (size <= sizes[i]) {
#if MW_DEBUG_MEMPOOL
                class_size = sizes[i];
#endif
                ptr = allocators_[i]->allocate();
                if (ptr) {
#if MW_DEBUG_MEMPOOL
                    used = allocators_[i]->used_count();
                    total = allocators_[i]->total_blocks();
#endif
                } else {
                    pool_full = true;
                }
                break;
            }
        }
        if (!ptr && !pool_full) {
            system_heap = true;
        }
    }

    if (system_heap) {
#if MW_DEBUG_MEMPOOL
        MW_LOG_MEMPOOL("alloc %zuB -> system heap", size);
#endif
        return ::operator new(size);
    }

#if MW_DEBUG_MEMPOOL
    if (ptr) {
        MW_LOG_MEMPOOL("alloc %zuB -> class %zuB (used %zu/%zu)",
            size, class_size, used, total);
    } else if (pool_full) {
        MW_LOG_MEMPOOL("alloc %zuB -> class %zuB FAILED (pool full)", size, class_size);
    }
#endif
    return ptr;
}

void* MemPool::allocate(BlockClass cls) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t idx = static_cast<size_t>(cls);
    if (idx >= allocators_.size() || !allocators_[idx]) return nullptr;
    return allocators_[idx]->allocate();
}

void MemPool::deallocate(void* ptr) {
    if (!ptr) {
        return;
    }

    bool system_heap = true;
#if MW_DEBUG_MEMPOOL
    size_t class_size = 0;
    size_t used = 0;
    size_t total = 0;
#endif

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& alloc : allocators_) {
            if (alloc && alloc->owns(ptr)) {
#if MW_DEBUG_MEMPOOL
                class_size = alloc->block_size();
#endif
                alloc->deallocate(ptr);
#if MW_DEBUG_MEMPOOL
                used = alloc->used_count();
                total = alloc->total_blocks();
#endif
                system_heap = false;
                break;
            }
        }
    }

#if MW_DEBUG_MEMPOOL
    if (system_heap) {
        MW_LOG_MEMPOOL("free system heap %p", ptr);
    } else {
        MW_LOG_MEMPOOL("free class %zuB (used %zu/%zu)", class_size, used, total);
    }
#endif

    if (system_heap) {
        ::operator delete(ptr);
    }
}

std::array<MemPool::ClassStats, 5> MemPool::get_stats() const {
    std::array<ClassStats, 5> stats{};
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < allocators_.size(); ++i) {
        if (!allocators_[i]) {
            continue;
        }
        stats[i].block_size = allocators_[i]->block_size();
        stats[i].total_blocks = allocators_[i]->total_blocks();
        stats[i].free_blocks = allocators_[i]->free_count();
        stats[i].used_blocks = allocators_[i]->used_count();
    }
    return stats;
}

size_t MemPool::get_block_size(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& alloc : allocators_) {
        if (alloc && alloc->owns(ptr)) {
            return alloc->block_size();
        }
    }
    return 0;
}

// ============================================
// MemBlock RAII
// ============================================
MemBlock::MemBlock(void* ptr) : ptr_(ptr) {}

MemBlock::~MemBlock() {
    release();
}

MemBlock::MemBlock(MemBlock&& other) noexcept : ptr_(other.ptr_) {
    other.ptr_ = nullptr;
}

MemBlock& MemBlock::operator=(MemBlock&& other) noexcept {
    if (this != &other) {
        release();
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
    }
    return *this;
}

void* MemBlock::get() const noexcept {
    return ptr_;
}

uint8_t* MemBlock::data() const noexcept {
    return static_cast<uint8_t*>(ptr_);
}

bool MemBlock::valid() const noexcept {
    return ptr_ != nullptr;
}

void MemBlock::release() {
    if (ptr_) {
        MemPool::instance().deallocate(ptr_);
        ptr_ = nullptr;
    }
}

} // namespace middleware