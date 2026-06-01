#ifndef MSG_BUS_HPP
#define MSG_BUS_HPP

#include "msg_types.hpp"
#include "msg_queue.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>

namespace middleware {

// 消息处理器接口
using MsgHandler = std::function<void(const Message&)>;

// 订阅句柄 (RAII自动取消订阅)
class Subscription {
public:
    Subscription() = default;
    Subscription(uint32_t id, class MsgBus* bus);
    ~Subscription();
    
    // 移动语义
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    
    // 禁止拷贝
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    
    [[nodiscard]] bool valid() const noexcept;
    void unsubscribe();
    
private:
    uint32_t id_ = 0;
    MsgBus* bus_ = nullptr;
};

// 消息总线
class MsgBus {
public:
    MsgBus();
    ~MsgBus();
    
    // 单例访问
    static MsgBus& instance();
    
    // 初始化/销毁
    bool init();
    void deinit();
    
    // 订阅 (返回RAII句柄)
    Subscription subscribe(MsgType type, ModuleId module_id, MsgHandler handler);
    Subscription subscribe_all(ModuleId module_id, MsgHandler handler);
    
    // 取消订阅
    void unsubscribe(uint32_t sub_id);
    
    // 发布消息
    bool publish(const Message& msg);
    bool publish_async(const Message& msg);
    bool publish_urgent(const Message& msg);
    
    // 处理队列 (在LVGL/UI线程调用)
    void process();
    
    // 状态查询
    [[nodiscard]] size_t pending_count() const noexcept;
    [[nodiscard]] bool is_full() const noexcept;
    [[nodiscard]] size_t subscriber_count() const noexcept;
    
private:
    struct Subscriber {
        uint32_t id;
        MsgType type;           // MsgType{0} = 订阅所有
        ModuleId module_id;
        MsgHandler handler;
    };
    
    std::vector<Subscriber> subscribers_;
    mutable std::mutex subscribers_mutex_;
    
    PriorityMsgQueue msg_queue_;
    mutable std::mutex queue_mutex_;
    
    std::atomic<uint32_t> next_sub_id_{1};
    std::atomic<bool> initialized_{false};
    
    bool deliver(const Message& msg);
    static void release_payload_buffer(const MsgPayload& payload);
    void drain_queue_release_buffers(PriorityMsgQueue& queue);
};

} // namespace middleware

#endif