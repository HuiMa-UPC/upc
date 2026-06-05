#include "ring_buffer.hpp"
#include <algorithm>
#include <cassert>

namespace middleware {

RingBuffer::RingBuffer(size_t size)
    : buffer_(size > 0 ? size : 1)
    , size_(buffer_.size()) {
    assert(size > 0 && "RingBuffer size must be > 0");
}

size_t RingBuffer::write(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;
    
    size_t write_len = std::min(len, free_space());
    size_t tail = tail_.load(std::memory_order_relaxed);
    
    for (size_t i = 0; i < write_len; ++i) {
        buffer_[tail] = data[i];
        tail = (tail + 1) % size_;
    }
    
    tail_.store(tail, std::memory_order_release);
    count_.fetch_add(write_len, std::memory_order_release);
    return write_len;
}

size_t RingBuffer::read(uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;
    
    size_t read_len = std::min(len, available());
    size_t head = head_.load(std::memory_order_relaxed);
    
    for (size_t i = 0; i < read_len; ++i) {
        data[i] = buffer_[head];
        head = (head + 1) % size_;
    }
    
    head_.store(head, std::memory_order_release);
    count_.fetch_sub(read_len, std::memory_order_release);
    return read_len;
}

size_t RingBuffer::peek(uint8_t* data, size_t len) const {
    if (!data || len == 0) return 0;
    
    size_t read_len = std::min(len, available());
    size_t head = head_.load(std::memory_order_acquire);
    
    for (size_t i = 0; i < read_len; ++i) {
        data[i] = buffer_[(head + i) % size_];
    }
    return read_len;
}

bool RingBuffer::empty() const noexcept {
    return count_.load(std::memory_order_acquire) == 0;
}

bool RingBuffer::full() const noexcept {
    return count_.load(std::memory_order_acquire) >= size_;
}

size_t RingBuffer::available() const noexcept {
    return count_.load(std::memory_order_acquire);
}

size_t RingBuffer::free_space() const noexcept {
    return size_ - count_.load(std::memory_order_acquire);
}

void RingBuffer::flush() noexcept {
    head_.store(0, std::memory_order_release);
    tail_.store(0, std::memory_order_release);
    count_.store(0, std::memory_order_release);
}

} // namespace middleware