#include "msg_port.hpp"
#include "ipc_msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"

namespace middleware {

MsgPort& MsgPort::instance() {
    static MsgPort inst;
    return inst;
}

void MsgPort::set_mode_in_process() {
    mode_ = MsgPortMode::InProcess;
}

bool MsgPort::set_mode_ipc_server(ModuleId local_id, const char* socket_path) {
    mode_ = MsgPortMode::Ipc;
    return upc_ipc::IpcMsgBus::instance().init_server(local_id, socket_path);
}

bool MsgPort::set_mode_ipc_client(ModuleId local_id, const char* socket_path) {
    mode_ = MsgPortMode::Ipc;
    return upc_ipc::IpcMsgBus::instance().init_client(local_id, socket_path);
}

bool MsgPort::init() {
    if (mode_ == MsgPortMode::InProcess) {
        MemPool::instance().init();
        BufferPool::instance().init();
        return MsgBus::instance().init();
    }
    return true;
}

void MsgPort::shutdown() {
    if (mode_ == MsgPortMode::Ipc) {
        upc_ipc::IpcMsgBus::instance().shutdown();
    } else {
        MsgBus::instance().deinit();
    }
}

bool MsgPort::peer_connected() const noexcept {
    if (mode_ == MsgPortMode::Ipc) {
        return upc_ipc::IpcMsgBus::instance().peer_connected();
    }
    return true;
}

Subscription MsgPort::subscribe(MsgType type, ModuleId module_id,
                                MsgHandler handler) {
    if (mode_ == MsgPortMode::InProcess) {
        return MsgBus::instance().subscribe(type, module_id, std::move(handler));
    }

    auto* ipc = &upc_ipc::IpcMsgBus::instance();
    const uint32_t id = ipc->subscribe(type, module_id, std::move(handler));
    return Subscription::from_cancel([id, ipc]() { ipc->unsubscribe(id); });
}

bool MsgPort::publish(const Message& msg) {
    if (mode_ == MsgPortMode::InProcess) {
        return MsgBus::instance().publish(msg);
    }
    return upc_ipc::IpcMsgBus::instance().publish_sync(msg);
}

bool MsgPort::publish_async(const Message& msg) {
    if (mode_ == MsgPortMode::InProcess) {
        return MsgBus::instance().publish_async(msg);
    }
    return upc_ipc::IpcMsgBus::instance().publish_async(msg);
}

void MsgPort::process() {
    if (mode_ == MsgPortMode::InProcess) {
        MsgBus::instance().process();
    } else {
        upc_ipc::IpcMsgBus::instance().process();
    }
}

} // namespace middleware
