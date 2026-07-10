#ifndef RING_BUFFER_HPP
#define RING_BUFFER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <optional>

namespace middleware {

/**
 * 字节环形缓冲（middleware 版）。
 *
 * 线程模型：仅适用于单生产者单消费者（SPSC），且同一时刻不可并发 write 或并发 read。
 * 多线程混用会导致数据竞争；需要 MPSC/MPMC 时请改用 services/glavo/ring_buffer（mutex）
 * 或在外层加锁。构造时 size 必须 > 0。
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t size);
    ~RingBuffer() = default;
    
    // 禁止拷贝，允许移动
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) noexcept = default;
    RingBuffer& operator=(RingBuffer&&) noexcept = default;
    
    size_t write(const uint8_t* data, size_t len);
    size_t read(uint8_t* data, size_t len);
    size_t peek(uint8_t* data, size_t len) const;
    
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] size_t available() const noexcept;
    [[nodiscard]] size_t free_space() const noexcept;
    
    void flush() noexcept;
    
private:
    std::vector<uint8_t> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::atomic<size_t> count_{0};
    const size_t size_;
};

} // namespace middleware

#endif