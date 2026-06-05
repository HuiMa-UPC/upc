#include "msg_queue.hpp"
#include <algorithm>

namespace middleware {

bool PriorityMsgQueue::push(const Message& msg) {
    if (queue_.size() >= MSG_BUS_QUEUE_DEPTH) {
        return false;
    }
    queue_.push_back(msg);
    std::stable_sort(queue_.begin(), queue_.end(),
        [](const Message& a, const Message& b) {
            return static_cast<uint8_t>(a.priority) > static_cast<uint8_t>(b.priority);
        });
    return true;
}

bool PriorityMsgQueue::pop(Message& msg) {
    if (queue_.empty()) {
        return false;
    }
    msg = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool PriorityMsgQueue::empty() const noexcept {
    return queue_.empty();
}

size_t PriorityMsgQueue::size() const noexcept {
    return queue_.size();
}

void PriorityMsgQueue::swap(PriorityMsgQueue& other) noexcept {
    queue_.swap(other.queue_);
}

} // namespace middleware
