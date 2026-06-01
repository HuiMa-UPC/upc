#include "buffer_pool.hpp"
#include "mem_pool.hpp"
#include "mw_debug.hpp"

#include <new>

namespace middleware {

namespace {

uint8_t* alloc_buffer_bytes(size_t size) {
    void* ptr = MemPool::instance().allocate(size);
    if (ptr) {
        return static_cast<uint8_t*>(ptr);
    }
    /* MemPool 档块耗尽或 >16K 时 MemPool 已走 system heap；档块满则在此兜底 */
    return static_cast<uint8_t*>(::operator new(size));
}

void free_buffer_bytes(uint8_t* ptr) {
    if (ptr) {
        MemPool::instance().deallocate(ptr);
    }
}

} // namespace

// ============================================
// BufferPool
// ============================================
BufferPool& BufferPool::instance() {
    static BufferPool instance;
    return instance;
}

bool BufferPool::init(size_t max_buffers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;
    buffers_.resize(max_buffers);
    initialized_ = true;
#if MIDDLEWARE_DEBUG
    MW_LOG_BUFFERPOOL("init ok capacity=%zu", max_buffers);
#endif
    return true;
}

void BufferPool::deinit() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : buffers_) {
        if (entry.used && entry.data) {
            free_buffer_bytes(entry.data);
            entry.data = nullptr;
            entry.used = false;
            entry.ref_count = 0;
            entry.size = 0;
        }
    }
    buffers_.clear();
    initialized_ = false;
}

BufferId BufferPool::allocate(size_t size) {
    BufferId id = INVALID_BUFFER_ID;
#if MW_DEBUG_BUFFERPOOL
    size_t used_count = 0;
    size_t capacity = 0;
#endif

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return INVALID_BUFFER_ID;
        }

#if MW_DEBUG_BUFFERPOOL
        capacity = buffers_.size();
#endif
        for (size_t i = 0; i < buffers_.size(); ++i) {
            if (!buffers_[i].used) {
                uint8_t* ptr = alloc_buffer_bytes(size);
                if (!ptr) {
                    break;
                }
                buffers_[i].data = ptr;
                buffers_[i].size = size;
                buffers_[i].ref_count = 1;
                buffers_[i].used = true;
                id = static_cast<BufferId>(i);
#if MW_DEBUG_BUFFERPOOL
                used_count = get_stats_locked().used_count;
#endif
                break;
            }
        }
    }

#if MW_DEBUG_BUFFERPOOL
    if (id != INVALID_BUFFER_ID) {
        MW_LOG_BUFFERPOOL("alloc id=%u size=%zu (used %zu/%zu)",
            static_cast<unsigned>(id), size, used_count, capacity);
    } else {
        MW_LOG_BUFFERPOOL("alloc size=%zu FAILED (pool full)", size);
    }
#endif
    return id;
}

void BufferPool::acquire(BufferId id) {
#if MW_DEBUG_BUFFERPOOL
    uint32_t ref = 0;
    bool ok = false;
#endif
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id < buffers_.size() && buffers_[id].used) {
#if MW_DEBUG_BUFFERPOOL
            ref = ++buffers_[id].ref_count;
            ok = true;
#else
            ++buffers_[id].ref_count;
#endif
        }
    }
#if MW_DEBUG_BUFFERPOOL
    if (ok) {
        MW_LOG_BUFFERPOOL("acquire id=%u ref=%u", static_cast<unsigned>(id), ref);
    }
#endif
}

void BufferPool::release(BufferId id) {
#if MW_DEBUG_BUFFERPOOL
    uint32_t ref = 0;
    bool freed = false;
#endif
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id >= buffers_.size() || !buffers_[id].used) {
            return;
        }
        if (buffers_[id].ref_count == 0) {
#if MW_DEBUG_BUFFERPOOL
            MW_LOG_BUFFERPOOL("release id=%u IGNORE (ref already 0)", static_cast<unsigned>(id));
#endif
            return;
        }

#if MW_DEBUG_BUFFERPOOL
        ref = --buffers_[id].ref_count;
        if (ref == 0) {
#else
        if (--buffers_[id].ref_count == 0) {
#endif
            free_buffer_bytes(buffers_[id].data);
            buffers_[id].data = nullptr;
            buffers_[id].size = 0;
            buffers_[id].used = false;
#if MW_DEBUG_BUFFERPOOL
            freed = true;
#endif
        }
    }
#if MW_DEBUG_BUFFERPOOL
    if (freed) {
        MW_LOG_BUFFERPOOL("release id=%u (freed)", static_cast<unsigned>(id));
    } else {
        MW_LOG_BUFFERPOOL("release id=%u ref=%u", static_cast<unsigned>(id), ref);
    }
#endif
}

uint8_t* BufferPool::get_ptr(BufferId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= buffers_.size() || !buffers_[id].used) return nullptr;
    return buffers_[id].data;
}

size_t BufferPool::get_size(BufferId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= buffers_.size() || !buffers_[id].used) return 0;
    return buffers_[id].size;
}

uint32_t BufferPool::get_ref_count(BufferId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= buffers_.size() || !buffers_[id].used) return 0;
    return buffers_[id].ref_count;
}

bool BufferPool::is_valid(BufferId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id < buffers_.size() && buffers_[id].used;
}

BufferPool::Stats BufferPool::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_stats_locked();
}

BufferPool::Stats BufferPool::get_stats_locked() const {
    Stats s;
    s.capacity = buffers_.size();
    for (const auto& entry : buffers_) {
        if (!entry.used) {
            continue;
        }
        ++s.used_count;
        s.bytes_used += entry.size;
    }
    return s;
}

// ============================================
// BufferHandle RAII
// ============================================
BufferHandle::BufferHandle(BufferId id) : id_(id) {}

BufferHandle::~BufferHandle() {
    reset();
}

BufferHandle::BufferHandle(BufferHandle&& other) noexcept : id_(other.id_) {
    other.id_ = INVALID_BUFFER_ID;
}

BufferHandle& BufferHandle::operator=(BufferHandle&& other) noexcept {
    if (this != &other) {
        reset();
        id_ = other.id_;
        other.id_ = INVALID_BUFFER_ID;
    }
    return *this;
}

BufferId BufferHandle::id() const noexcept {
    return id_;
}

uint8_t* BufferHandle::data() const {
    return BufferPool::instance().get_ptr(id_);
}

size_t BufferHandle::size() const {
    return BufferPool::instance().get_size(id_);
}

bool BufferHandle::valid() const noexcept {
    return id_ != INVALID_BUFFER_ID && BufferPool::instance().is_valid(id_);
}

void BufferHandle::reset(BufferId id) {
    if (id_ != INVALID_BUFFER_ID) {
        BufferPool::instance().release(id_);
    }
    id_ = id;
}

BufferId BufferHandle::detach() noexcept {
    BufferId tmp = id_;
    id_ = INVALID_BUFFER_ID;
    return tmp;
}

} // namespace middleware