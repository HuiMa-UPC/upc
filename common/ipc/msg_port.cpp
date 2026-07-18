#include "msg_port.hpp"
#include "ipc_msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"

namespace middleware {

MsgPort& MsgPort::instance()
{
    static MsgPort inst;
    return inst;
}

void MsgPort::set_mode_in_process()
{
    mode_ = MsgPortMode::InProcess;
}

bool MsgPort::set_mode_ipc_client(ModuleId local_id, const char* broker_path)
{
    mode_ = MsgPortMode::Ipc;
    return upc_ipc::IpcMsgBus::instance().init_ipc_client(local_id, broker_path);
}

bool MsgPort::init()
{
    if (mode_ == MsgPortMode::InProcess) {
        MemPool::instance().init();
        BufferPool::instance().init();
        return MsgBus::instance().init();
    }
    return true;
}

void MsgPort::shutdown()
{
    if (mode_ == MsgPortMode::Ipc) {
        upc_ipc::IpcMsgBus::instance().shutdown();
    } else {
        MsgBus::instance().deinit();
    }
}

bool MsgPort::peer_connected() const noexcept
{
    if (mode_ == MsgPortMode::Ipc) {
        return upc_ipc::IpcMsgBus::instance().peer_connected();
    }
    return true;
}

bool MsgPort::peer_connected(ModuleId peer_id) const noexcept
{
    if (mode_ == MsgPortMode::Ipc) {
        return upc_ipc::IpcMsgBus::instance().peer_connected(peer_id);
    }
    return true;
}

Subscription MsgPort::subscribe(MsgType type, ModuleId module_id,
                                MsgHandler handler)
{
    if (mode_ == MsgPortMode::InProcess) {
        return MsgBus::instance().subscribe(type, module_id, std::move(handler));
    }

    auto* ipc = &upc_ipc::IpcMsgBus::instance();
    const uint32_t id = ipc->subscribe(type, module_id, std::move(handler));
    return Subscription::from_cancel([id, ipc]() { ipc->unsubscribe(id); });
}

bool MsgPort::publish(const Message& msg)
{
    if (mode_ == MsgPortMode::InProcess) {
        return MsgBus::instance().publish(msg);
    }
    return upc_ipc::IpcMsgBus::instance().publish_sync(msg);
}

bool MsgPort::publish_async(const Message& msg)
{
    if (mode_ == MsgPortMode::InProcess) {
        return MsgBus::instance().publish_async(msg);
    }
    return upc_ipc::IpcMsgBus::instance().publish_async(msg);
}

bool MsgPort::publish_async_broadcast(const Message& msg)
{
    Message copy = msg;
    copy.target_id = ModuleId{0};
    return publish_async(copy);
}

void MsgPort::process(int io_timeout_ms)
{
    if (mode_ == MsgPortMode::InProcess) {
        MsgBus::instance().process();
        (void)io_timeout_ms;
    } else {
        upc_ipc::IpcMsgBus::instance().process(io_timeout_ms);
    }
}

bool MsgPort::publish_to_peer_with_fds(const Message& msg,
                                       const std::vector<int>& fds)
{
    if (mode_ == MsgPortMode::InProcess) {
        (void)fds;
        return MsgBus::instance().publish_async(msg);
    }
    return upc_ipc::IpcMsgBus::instance().send_to_peer_with_fds(msg.target_id,
                                                                msg, fds);
}

bool MsgPort::take_peer_dma_fds(ModuleId peer_id, std::vector<int>& out_fds)
{
    if (mode_ == MsgPortMode::InProcess) {
        out_fds.clear();
        return false;
    }
    return upc_ipc::IpcMsgBus::instance().take_pending_dma_fds(peer_id, out_fds);
}

} // namespace middleware
