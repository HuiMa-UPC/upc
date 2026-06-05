#include "buffer_pool.hpp"
#include "mem_pool.hpp"
#include "rs_log.hpp"

#include <new>

namespace middleware {

namespace {

uint8_t* alloc_buffer_bytes(size_t size) {
    void* ptr = MemPool::instance().allocate(size);
    if (ptr) {
        return static_cast<uint8_t*>(ptr);
    }
    return static_cast<uint8_t*>(::operator new(size));
}

void free_buffer_bytes(uint8_t* ptr) {
    if (ptr) {
        MemPool::instance().deallocate(ptr);
    }
}

} // namespace

BufferPool& BufferPool::instance() {
    static BufferPool instance;
    return instance;
}

bool BufferPool::init(size_t max_buffers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;
    buffers_.resize(max_buffers);
    initialized_ = true;
    UPC_LOG_BUFFER_POOL(Info, "init ok capacity=%zu", max_buffers);
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return INVALID_BUFFER_ID;
        }

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
                break;
            }
        }
    }

    if (id == INVALID_BUFFER_ID) {
        UPC_LOG_BUFFER_POOL(Error, "alloc size=%zu FAILED (pool full)", size);
    }
    return id;
}

void BufferPool::acquire(BufferId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id < buffers_.size() && buffers_[id].used) {
        ++buffers_[id].ref_count;
    }
}

void BufferPool::release(BufferId id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id >= buffers_.size() || !buffers_[id].used) {
            return;
        }
        if (buffers_[id].ref_count == 0) {
            UPC_LOG_BUFFER_POOL(Warn, "release id=%u IGNORE (ref already 0)",
                static_cast<unsigned>(id));
            return;
        }

        if (--buffers_[id].ref_count == 0) {
            free_buffer_bytes(buffers_[id].data);
            buffers_[id].data = nullptr;
            buffers_[id].size = 0;
            buffers_[id].used = false;
        }
    }
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
