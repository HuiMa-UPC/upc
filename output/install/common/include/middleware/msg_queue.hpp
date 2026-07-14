#ifndef MSG_QUEUE_HPP
#define MSG_QUEUE_HPP

#include "msg_runtime.hpp"
#include <cstddef>
#include <deque>

namespace middleware {

// 按 MsgPriority 降序排列的异步消息队列（高优先级先出队）
class PriorityMsgQueue {
public:
    bool push(const Message& msg);
    bool pop(Message& msg);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] size_t size() const noexcept;

    void swap(PriorityMsgQueue& other) noexcept;

private:
    std::deque<Message> queue_;
};

} // namespace middleware

#endif
