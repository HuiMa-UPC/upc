#include "ipc_msg_bus.hpp"

#include "ipc_wire.hpp"

#include "buffer_pool.hpp"

#include "mem_pool.hpp"

#include "rs_msg_format.hpp"

#include "rs_log.hpp"



#include <algorithm>



namespace upc_ipc {



IpcMsgBus& IpcMsgBus::instance() {

    static IpcMsgBus inst;

    return inst;

}



bool IpcMsgBus::init_server(middleware::ModuleId local_id,

                            const char* socket_path) {

    if (initialized_) {

        return true;

    }

    local_id_ = local_id;

    peer_id_  = middleware::ModuleId::UI;

    is_server_ = true;



    middleware::MemPool::instance().init();

    middleware::BufferPool::instance().init();



    if (!endpoint_.listen_on(socket_path)) {

        UPC_LOG_IPC(Error, "listen failed path=%s", socket_path ? socket_path : "(null)");

        return false;

    }

    initialized_ = true;

    UPC_LOG_IPC(Info, "server listen path=%s local=%s peer=%s",

        socket_path ? socket_path : "(null)",

        upc_msg_module_name(local_id_),

        upc_msg_module_name(peer_id_));

    return true;

}



bool IpcMsgBus::init_client(middleware::ModuleId local_id,

                            const char* socket_path) {

    if (initialized_) {

        return true;

    }

    local_id_ = local_id;

    peer_id_  = middleware::ModuleId::APP_GLAVO;

    is_server_ = false;



    middleware::MemPool::instance().init();

    middleware::BufferPool::instance().init();



    if (!endpoint_.connect_to(socket_path)) {

        UPC_LOG_IPC(Error, "connect failed path=%s", socket_path ? socket_path : "(null)");

        return false;

    }

    initialized_ = true;

    UPC_LOG_IPC(Info, "client connected path=%s local=%s peer=%s",

        socket_path ? socket_path : "(null)",

        upc_msg_module_name(local_id_),

        upc_msg_module_name(peer_id_));

    return true;

}



void IpcMsgBus::shutdown() {

    {

        std::lock_guard<std::mutex> lock(subs_mtx_);

        subscribers_.clear();

    }

    {

        std::lock_guard<std::mutex> lock(queue_mtx_);

        middleware::Message m;

        while (local_queue_.pop(m)) {

        }

    }

    endpoint_.close();

    initialized_ = false;

    UPC_LOG_IPC(Info, "shutdown");

}



void IpcMsgBus::try_accept_peer() {

    if (is_server_) {

        endpoint_.try_accept();

    }

}



bool IpcMsgBus::peer_connected() const noexcept {

    return endpoint_.connected();

}



bool IpcMsgBus::is_remote(middleware::ModuleId id) const {

    if (id == middleware::ModuleId{0}) {

        return false;

    }

    return id == peer_id_;

}



void IpcMsgBus::unsubscribe(uint32_t sub_id) {

    size_t total = 0;

    bool removed = false;

    {

        std::lock_guard<std::mutex> lock(subs_mtx_);

        const size_t before = subscribers_.size();

        subscribers_.erase(

            std::remove_if(subscribers_.begin(), subscribers_.end(),

                           [sub_id](const LocalSub& s) { return s.id == sub_id; }),

            subscribers_.end());

        removed = subscribers_.size() != before;

        total = subscribers_.size();

    }

    if (removed) {

        UPC_LOG_IPC(Debug, "unsubscribe id=%u total=%zu", sub_id, total);

    }

}



uint32_t IpcMsgBus::subscribe(middleware::MsgType type,

                              middleware::ModuleId module_id,

                              IpcHandler handler) {

    const uint32_t id = next_sub_id_++;

    size_t total = 0;

    {

        std::lock_guard<std::mutex> lock(subs_mtx_);

        subscribers_.push_back({id, type, module_id, std::move(handler)});

        total = subscribers_.size();

    }

    UPC_LOG_IPC(Debug, "subscribe id=%u type=%s module=%s total=%zu",

        id,

        upc_msg_type_name(type),

        upc_msg_module_name(module_id),

        total);

    return id;

}



bool IpcMsgBus::deliver_local(const middleware::Message& msg) {

    std::vector<IpcHandler> handlers;

    handlers.reserve(8);

    {

        std::lock_guard<std::mutex> lock(subs_mtx_);

        for (const auto& sub : subscribers_) {

            const bool type_match =

                (sub.type == middleware::MsgType{0}) || (sub.type == msg.type);

            const bool target_match = (msg.target_id == middleware::ModuleId{0}) ||

                                      (sub.module_id == msg.target_id);

            if (type_match && target_match) {

                handlers.push_back(sub.handler);

            }

        }

    }

    if (handlers.empty()) {

        UPC_LOG_MSG_BUS(Warn, "IPC_DELIVER miss %s %s->%s (no subscriber)",

            upc_msg_type_name(msg.type),

            upc_msg_module_name(msg.sender_id),

            upc_msg_module_name(msg.target_id));

        return false;

    }

    upc_msg_log_message("IPC_DELIVER", msg, handlers.size());

    for (const auto& h : handlers) {

        if (h) {

            h(msg);

        }

    }

    return true;

}



bool IpcMsgBus::send_to_peer(const middleware::Message& msg) {

    if (!endpoint_.connected()) {

        return false;

    }

    std::vector<uint8_t> frame;

    if (!wire_encode(msg, frame)) {

        UPC_LOG_IPC(Error, "wire_encode fail %s %s->%s",

            upc_msg_type_name(msg.type),

            upc_msg_module_name(msg.sender_id),

            upc_msg_module_name(msg.target_id));

        return false;

    }

    if (!endpoint_.send_frame(frame)) {

        return false;

    }

    upc_msg_log_message("IPC_TX", msg);

    return true;

}



bool IpcMsgBus::enqueue_local(const middleware::Message& msg) {

    size_t queue_depth = 0;

    bool pushed = false;

    {

        std::lock_guard<std::mutex> lock(queue_mtx_);

        pushed = local_queue_.push(msg);

        if (pushed) {

            queue_depth = local_queue_.size();

        }

    }

    if (!pushed) {

        UPC_LOG_MSG_BUS(Error, "IPC_ENQUEUE FAIL %s %s->%s (queue full)",

            upc_msg_type_name(msg.type),

            upc_msg_module_name(msg.sender_id),

            upc_msg_module_name(msg.target_id));

        return false;

    }

    upc_msg_log_message("IPC_ENQUEUE", msg, 0, queue_depth);

    return true;

}



bool IpcMsgBus::publish_async(const middleware::Message& msg) {

    const bool broadcast = msg.target_id == middleware::ModuleId{0};

    const bool to_peer =

        (broadcast || is_remote(msg.target_id)) && msg.sender_id == local_id_;



    if (to_peer) {

        if (!send_to_peer(msg)) {

            UPC_LOG_IPC(Warn, "IPC_TX fail %s %s->%s (peer not ready)",

                upc_msg_type_name(msg.type),

                upc_msg_module_name(msg.sender_id),

                upc_msg_module_name(msg.target_id));

            return false;

        }

    }



    if (broadcast || !is_remote(msg.target_id)) {

        return enqueue_local(msg);

    }

    return true;

}



bool IpcMsgBus::publish_sync(const middleware::Message& msg) {

    const bool broadcast = msg.target_id == middleware::ModuleId{0};

    bool ok = true;



    if ((broadcast || is_remote(msg.target_id)) && msg.sender_id == local_id_) {

        if (!send_to_peer(msg)) {

            UPC_LOG_IPC(Warn, "IPC_TX fail %s %s->%s (peer not ready)",

                upc_msg_type_name(msg.type),

                upc_msg_module_name(msg.sender_id),

                upc_msg_module_name(msg.target_id));

            ok = false;

        }

    }



    if (broadcast || !is_remote(msg.target_id)) {

        if (!deliver_local(msg)) {

            ok = false;

        }

    }

    return ok;

}



void IpcMsgBus::pump_inbound() {

    const bool was_connected = endpoint_.connected();

    std::vector<std::vector<uint8_t>> frames;

    endpoint_.recv_pending(frames);

    if (was_connected && !endpoint_.connected()) {

        UPC_LOG_IPC(Warn, "peer disconnected");

    }



    for (const auto& frame : frames) {

        middleware::Message msg;

        if (!wire_decode(frame.data(), frame.size(), msg)) {

            UPC_LOG_IPC(Warn, "wire_decode fail frame_bytes=%zu", frame.size());

            continue;

        }

        upc_msg_log_message("IPC_RX", msg);

        deliver_local(msg);

    }

}



void IpcMsgBus::process() {

    const bool was_connected = endpoint_.connected();

    if (is_server_) {

        try_accept_peer();

    }

    if (!was_connected && endpoint_.connected()) {

        UPC_LOG_IPC(Info, "peer connected");

    }



    pump_inbound();



    middleware::PriorityMsgQueue local;

    {

        std::lock_guard<std::mutex> lock(queue_mtx_);

        local.swap(local_queue_);

    }



    middleware::Message msg;

    size_t delivered = 0;

    while (local.pop(msg)) {

        if (deliver_local(msg)) {

            ++delivered;

        }

    }

    if (delivered > 0) {

        UPC_LOG_IPC(Debug, "process delivered=%zu", delivered);

    }

}



} // namespace upc_ipc

