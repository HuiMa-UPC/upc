#include "ring_buffer.hpp"

namespace services {

RingBuffer::RingBuffer()
{
}

bool RingBuffer::put_wait(const rb_packet_t& pkt, const std::atomic<bool>& stop_flag)
{
    std::unique_lock<std::mutex> lock(mtx);
    while(cnt >= RB_MAX_PACKETS) {
        if(stop_flag.load(std::memory_order_relaxed)) {
            return false;
        }
        cv_not_full.wait(lock, [this, &stop_flag] {
            return stop_flag.load(std::memory_order_relaxed) || cnt < RB_MAX_PACKETS;
        });
        if(stop_flag.load(std::memory_order_relaxed)) {
            return false;
        }
    }

    buf[tail] = pkt;
    tail = (tail + 1) % RB_MAX_PACKETS;
    ++cnt;
    cv_not_empty.notify_one();
    return true;
}

bool RingBuffer::get(rb_packet_t& pkt, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mtx);
    if (!cv_not_empty.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this] { return cnt > 0; })) {
        return false;
    }

    pkt = buf[head];
    head = (head + 1) % RB_MAX_PACKETS;
    --cnt;

    cv_not_full.notify_one();
    return true;
}

bool RingBuffer::peek_type(rb_pkt_type_t& type) const
{
    std::lock_guard<std::mutex> lock(mtx);
    if (cnt == 0) return false;
    type = static_cast<rb_pkt_type_t>(buf[head].type);
    return true;
}

bool RingBuffer::empty() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return cnt == 0;
}

size_t RingBuffer::count() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return cnt;
}

void RingBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mtx);
    head = tail = cnt = 0;
    cv_not_full.notify_all();
}


} // namespace services
