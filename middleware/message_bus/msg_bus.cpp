#include "msg_bus.hpp"
#include "rs_msg_format.hpp"
#include "rs_log.hpp"
#include <algorithm>

namespace middleware {

namespace {

void invoke_handlers(const Message& msg, const std::vector<MsgHandler>& handlers) {
    for (const auto& handler : handlers) {
        if (handler) {
            handler(msg);
        }
    }
}

} // namespace

// ============================================
// Subscription RAII
// ============================================
Subscription::Subscription(uint32_t id, MsgBus* bus) : id_(id), bus_(bus) {}

Subscription::~Subscription() {
    unsubscribe();
}

Subscription::Subscription(Subscription&& other) noexcept
    : id_(other.id_), bus_(other.bus_) {
    other.id_ = 0;
    other.bus_ = nullptr;
}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        id_ = other.id_;
        bus_ = other.bus_;
        other.id_ = 0;
        other.bus_ = nullptr;
    }
    return *this;
}

bool Subscription::valid() const noexcept {
    return id_ != 0 && bus_ != nullptr;
}

void Subscription::unsubscribe() {
    if (valid()) {
        bus_->unsubscribe(id_);
        id_ = 0;
        bus_ = nullptr;
    }
}

// ============================================
// MsgBus
// ============================================
MsgBus::MsgBus() = default;

MsgBus::~MsgBus() {
    deinit();
}

MsgBus& MsgBus::instance() {
    static MsgBus instance;
    return instance;
}

bool MsgBus::init() {
    if (initialized_.exchange(true)) {
        return true;
    }
    UPC_LOG_MSG_BUS(Info, "init ok queue_depth=%d", MSG_BUS_QUEUE_DEPTH);
    return true;
}

void MsgBus::release_payload_buffer(const MsgPayload& payload) {
    if (payload.type == MsgPayload::Type::BufferRef) {
        BufferPool::instance().release(payload.data.buffer_ref.buf_id);
    }
}

void MsgBus::drain_queue_release_buffers(PriorityMsgQueue& queue) {
    Message msg;
    while (queue.pop(msg)) {
        // MsgPayload 析构函数会自动释放 BufferRef
    }
}

void MsgBus::deinit() {
    if (!initialized_.exchange(false)) {
        return;
    }

    PriorityMsgQueue pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending.swap(msg_queue_);
    }
    drain_queue_release_buffers(pending);

    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.clear();
}

Subscription MsgBus::subscribe(MsgType type, ModuleId module_id, MsgHandler handler) {
    if (!handler) {
        return Subscription{};
    }

    uint32_t id = next_sub_id_.fetch_add(1);

    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        subscribers_.push_back({id, type, module_id, std::move(handler)});
        total = subscribers_.size();
    }

    UPC_LOG_MSG_BUS(Debug, "subscribe id=%u type=%s module=%s total=%zu",
        id,
        upc_msg_type_name(type),
        upc_msg_module_name(module_id),
        total);

    return Subscription(id, this);
}

Subscription MsgBus::subscribe_all(ModuleId module_id, MsgHandler handler) {
    return subscribe(MsgType{0}, module_id, std::move(handler));
}

void MsgBus::unsubscribe(uint32_t sub_id) {
    size_t total = 0;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        const size_t before = subscribers_.size();
        subscribers_.erase(
            std::remove_if(subscribers_.begin(), subscribers_.end(),
                [sub_id](const auto& s) { return s.id == sub_id; }),
            subscribers_.end());
        removed = subscribers_.size() != before;
        total = subscribers_.size();
    }
    if (removed) {
        UPC_LOG_MSG_BUS(Debug, "unsubscribe id=%u total=%zu", sub_id, total);
    }
}

bool MsgBus::publish(const Message& msg) {
    upc_msg_log_message("PUBLISH", msg);
    return deliver(msg);
}

bool MsgBus::publish_async(const Message& msg) {
    size_t queue_depth = 0;
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pushed = msg_queue_.push(msg);
        if (pushed) {
            queue_depth = msg_queue_.size();
        }
    }
    if (!pushed) {
        UPC_LOG_MSG_BUS(Error, "ENQUEUE FAIL %s %s->%s (queue full)",
            upc_msg_type_name(msg.type),
            upc_msg_module_name(msg.sender_id),
            upc_msg_module_name(msg.target_id));
        return false;
    }
    upc_msg_log_message("ENQUEUE", msg, 0, queue_depth);
    return true;
}

bool MsgBus::publish_urgent(const Message& msg) {
    return deliver(msg);
}

void MsgBus::process() {
    PriorityMsgQueue local_queue;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        local_queue.swap(msg_queue_);
    }

    Message msg;
    size_t delivered = 0;
    while (local_queue.pop(msg)) {
        if (deliver(msg)) {
            ++delivered;
        }
    }
    if (delivered > 0) {
        UPC_LOG_MSG_BUS(Debug, "process delivered=%zu", delivered);
    }
}

size_t MsgBus::pending_count() const noexcept {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return msg_queue_.size();
}

bool MsgBus::is_full() const noexcept {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return msg_queue_.size() >= MSG_BUS_QUEUE_DEPTH;
}

size_t MsgBus::subscriber_count() const noexcept {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    return subscribers_.size();
}

bool MsgBus::deliver(const Message& msg) {
    std::vector<MsgHandler> handlers;
    handlers.reserve(8);

    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        for (const auto& sub : subscribers_) {
            const bool type_match = (sub.type == MsgType{0}) || (sub.type == msg.type);
            const bool target_match = (msg.target_id == ModuleId{0}) ||
                                      (sub.module_id == msg.target_id);
            if (type_match && target_match) {
                handlers.push_back(sub.handler);
            }
        }
    }

    if (handlers.empty()) {
        UPC_LOG_MSG_BUS(Warn, "DELIVER miss %s %s->%s (no subscriber)",
            upc_msg_type_name(msg.type),
            upc_msg_module_name(msg.sender_id),
            upc_msg_module_name(msg.target_id));
        return false;
    }

    upc_msg_log_message("DELIVER", msg, handlers.size());
    invoke_handlers(msg, handlers);
    return true;
}

} // namespace middleware
