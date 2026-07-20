#include "msg_port.hpp"
#include "ipc_msg_bus.hpp"

namespace middleware {

MsgPort& MsgPort::instance()
{
    static MsgPort inst;
    return inst;
}

bool MsgPort::set_mode_ipc_client(ModuleId local_id, const char* broker_path)
{
    return upc_ipc::IpcMsgBus::instance().init_ipc_client(local_id, broker_path);
}

bool MsgPort::init()
{
    return true;
}

void MsgPort::shutdown()
{
    upc_ipc::IpcMsgBus::instance().shutdown();
}

bool MsgPort::peer_connected() const noexcept
{
    return upc_ipc::IpcMsgBus::instance().peer_connected();
}

bool MsgPort::peer_connected(ModuleId peer_id) const noexcept
{
    return upc_ipc::IpcMsgBus::instance().peer_connected(peer_id);
}

Subscription MsgPort::subscribe(MsgType type, ModuleId module_id,
                                MsgHandler handler)
{
    auto* ipc = &upc_ipc::IpcMsgBus::instance();
    const uint32_t id = ipc->subscribe(type, module_id, std::move(handler));
    return Subscription::from_cancel([id, ipc]() { ipc->unsubscribe(id); });
}

bool MsgPort::publish(const Message& msg)
{
    return upc_ipc::IpcMsgBus::instance().publish_sync(msg);
}

bool MsgPort::publish_async(const Message& msg)
{
    return upc_ipc::IpcMsgBus::instance().publish_async(msg);
}

void MsgPort::process(int io_timeout_ms)
{
    upc_ipc::IpcMsgBus::instance().process(io_timeout_ms);
}

bool MsgPort::publish_to_peer_with_fds(const Message& msg,
                                       const std::vector<int>& fds)
{
    return upc_ipc::IpcMsgBus::instance().send_to_peer_with_fds(msg, fds);
}

bool MsgPort::take_peer_dma_fds(ModuleId peer_id, std::vector<int>& out_fds)
{
    return upc_ipc::IpcMsgBus::instance().take_pending_dma_fds(peer_id, out_fds);
}

} // namespace middleware
