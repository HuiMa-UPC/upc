#include "msg_queue.hpp"
namespace middleware {

bool PriorityMsgQueue::push(const Message& msg) {
    if (queue_.size() >= MSG_BUS_QUEUE_DEPTH) {
        return false;
    }
    const auto prio = static_cast<uint8_t>(msg.priority);
    auto it = queue_.begin();
    for (; it != queue_.end(); ++it) {
        if (static_cast<uint8_t>(it->priority) < prio) {
            break;
        }
    }
    queue_.insert(it, msg);
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
