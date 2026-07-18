#include "ipc_msg_bus.hpp"

#include "ipc_reg.hpp"
#include "ipc_wire.hpp"
#include "msg_dispatch_utils.hpp"
#include "upc_contract.hpp"
#include "buffer_pool.hpp"
#include "mem_pool.hpp"
#include "rs_msg_format.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <thread>

#if defined(__linux__)
#include <sys/epoll.h>
#endif

namespace upc_ipc {

namespace {

using Clock = std::chrono::steady_clock;

#if defined(__linux__)
constexpr uint32_t kPollInEvents     = EPOLLIN | EPOLLRDHUP;
constexpr uint32_t kPollListenEvents = EPOLLIN;
#endif

void ensure_poller_open(IpcPoller& poller)
{
    if (!poller.valid()) {
        poller.reopen();
    }
}

} // namespace

IpcMsgBus& IpcMsgBus::instance()
{
    static IpcMsgBus inst;
    return inst;
}

bool IpcMsgBus::init_broker(const char* socket_path)
{
    if (initialized_ && mode_ == IpcBusMode::Broker) {
        return true;
    }
    if (initialized_) {
        shutdown();
    }

    mode_ = IpcBusMode::Broker;
    if (!server_endpoint_.listen_on(socket_path)) {
        UPC_LOG_IPC(Error, "broker listen failed path=%s",
                    socket_path ? socket_path : "(null)");
        mode_ = IpcBusMode::None;
        return false;
    }

    poll_clear();
    ensure_poller_open(poller_);
    poll_register_broker_listen();

    initialized_ = true;
    UPC_LOG_IPC(Info, "broker listen path=%s",
                socket_path ? socket_path : "(null)");
    return true;
}

bool IpcMsgBus::init_ipc_client(middleware::ModuleId local_id,
                                const char* broker_path)
{
    if (initialized_ && mode_ == IpcBusMode::Client && local_id_ == local_id &&
        broker_path_ == (broker_path ? broker_path : "")) {
        if (!client_endpoint_.connected()) {
            try_reconnect_broker();
        }
        return client_endpoint_.connected();
    }
    if (initialized_) {
        shutdown();
    }

    local_id_    = local_id;
    broker_path_ = broker_path ? broker_path : "";
    mode_        = IpcBusMode::Client;

    middleware::MemPool::instance().init();
    middleware::BufferPool::instance().init();

    poll_clear();
    ensure_poller_open(poller_);

    if (!broker_path_.empty() && client_endpoint_.connect_to(broker_path_.c_str()) &&
        client_send_reg()) {
        poll_register_client_conn();
    }

    initialized_ = true;
    UPC_LOG_IPC(Info, "client init local=%s broker=%s connected=%d",
                upc_msg_module_name(local_id_), broker_path_.c_str(),
                client_endpoint_.connected() ? 1 : 0);
    return true;
}

void IpcMsgBus::shutdown()
{
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

    poll_clear();
    poller_.close();

    server_endpoint_.close();
    client_endpoint_.close();
    broker_path_.clear();
    {
        std::lock_guard<std::mutex> lock(online_peers_mtx_);
        online_remote_peers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(broker_clients_mtx_);
        broker_clients_.clear();
        broker_module_index_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pending_dma_fds_mtx_);
        pending_dma_fds_.clear();
    }

    initialized_ = false;
    mode_          = IpcBusMode::None;
    UPC_LOG_IPC(Info, "shutdown");
}

bool IpcMsgBus::peer_connected() const noexcept
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(online_peers_mtx_);
    return online_remote_peers_.count(
               static_cast<uint32_t>(middleware::ModuleId::APP_GLAVO)) != 0;
}

bool IpcMsgBus::peer_connected(middleware::ModuleId peer_id) const noexcept
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return false;
    }
    if (peer_id == local_id_) {
        return true;
    }
    std::lock_guard<std::mutex> lock(online_peers_mtx_);
    return online_remote_peers_.count(static_cast<uint32_t>(peer_id)) != 0;
}

void IpcMsgBus::unsubscribe(uint32_t sub_id)
{
    std::lock_guard<std::mutex> lock(subs_mtx_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                       [sub_id](const LocalSub& s) { return s.id == sub_id; }),
        subscribers_.end());
}

uint32_t IpcMsgBus::subscribe(middleware::MsgType type,
                              middleware::ModuleId module_id,
                              IpcHandler handler)
{
    const uint32_t id = next_sub_id_++;
    std::lock_guard<std::mutex> lock(subs_mtx_);
    subscribers_.push_back({id, type, module_id, std::move(handler)});
    return id;
}

bool IpcMsgBus::deliver_local(const middleware::Message& msg)
{
    std::vector<IpcHandler> handlers;
    handlers.reserve(8);
    {
        std::lock_guard<std::mutex> lock(subs_mtx_);
        middleware::detail::collect_matching_handlers(subscribers_, msg,
                                                      handlers);
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
        h(msg);
    }
    return true;
}

bool IpcMsgBus::send_wire_frame(const std::vector<uint8_t>& frame)
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return false;
    }
    return client_endpoint_.send_frame(frame);
}

bool IpcMsgBus::send_wire_frame_with_fds(const std::vector<uint8_t>& frame,
                                         const middleware::Message& msg,
                                         const std::vector<int>& fds)
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return false;
    }
    if (!client_endpoint_.send_frame_with_fds(frame, fds)) {
        UPC_LOG_IPC(Error, "client send_frame_with_fds fail %s fds=%zu",
                    upc_msg_type_name(msg.type), fds.size());
        return false;
    }
    return true;
}

bool IpcMsgBus::send_to_peer_with_fds(middleware::ModuleId peer_id,
                                      const middleware::Message& msg,
                                      const std::vector<int>& fds)
{
    (void)peer_id;
    std::vector<uint8_t> frame;
    if (!wire_encode(msg, frame)) {
        UPC_LOG_IPC(Error, "wire_encode fail (with fds) %s",
                    upc_msg_type_name(msg.type));
        return false;
    }
    if (fds.empty()) {
        if (!send_wire_frame(frame)) {
            return false;
        }
    } else if (!send_wire_frame_with_fds(frame, msg, fds)) {
        return false;
    }
    upc_msg_log_message("IPC_TX", msg);
    if (!fds.empty()) {
        UPC_LOG_IPC(Info, "IPC_TX fds %s count=%zu",
                    upc_msg_type_name(msg.type), fds.size());
    }
    return true;
}

void IpcMsgBus::stash_pending_dma_fds(middleware::ModuleId sender,
                                      std::vector<int> fds)
{
    if (fds.empty()) {
        return;
    }
    const uint32_t key = static_cast<uint32_t>(sender);
    std::lock_guard<std::mutex> lock(pending_dma_fds_mtx_);
    auto& slot = pending_dma_fds_[key];
    for (int fd : fds) {
        if (fd >= 0) {
            slot.push_back(fd);
        }
    }
}

bool IpcMsgBus::take_pending_dma_fds(middleware::ModuleId peer_id,
                                     std::vector<int>& out_fds)
{
    out_fds.clear();
    const uint32_t key = static_cast<uint32_t>(peer_id);
    std::lock_guard<std::mutex> lock(pending_dma_fds_mtx_);
    auto it = pending_dma_fds_.find(key);
    if (it == pending_dma_fds_.end() || it->second.empty()) {
        return false;
    }
    out_fds = std::move(it->second);
    pending_dma_fds_.erase(it);
    return true;
}

bool IpcMsgBus::needs_local_handling(const middleware::Message& msg) const
{
    if (mode_ == IpcBusMode::Broker) {
        return false;
    }
    return msg.target_id == middleware::ModuleId{0} ||
           msg.target_id == local_id_;
}

bool IpcMsgBus::send_outbound_from_local(const middleware::Message& msg,
                                         bool warn_on_fail)
{
    if (msg.sender_id != local_id_ || mode_ != IpcBusMode::Client) {
        return true;
    }
    if (msg.target_id == local_id_) {
        return true;
    }

    std::vector<uint8_t> frame;
    if (!wire_encode(msg, frame)) {
        UPC_LOG_IPC(Error, "wire_encode fail %s %s->%s",
                    upc_msg_type_name(msg.type),
                    upc_msg_module_name(msg.sender_id),
                    upc_msg_module_name(msg.target_id));
        return false;
    }
    if (send_wire_frame(frame)) {
        upc_msg_log_message("IPC_TX", msg);
        return true;
    }
    if (warn_on_fail) {
        UPC_LOG_IPC(Warn, "IPC_TX fail %s %s->%s (broker not ready)",
                    upc_msg_type_name(msg.type),
                    upc_msg_module_name(msg.sender_id),
                    upc_msg_module_name(msg.target_id));
    }
    return false;
}

bool IpcMsgBus::enqueue_local(const middleware::Message& msg)
{
    std::lock_guard<std::mutex> lock(queue_mtx_);
    if (!local_queue_.push(msg)) {
        UPC_LOG_MSG_BUS(Error, "IPC_ENQUEUE FAIL %s %s->%s (queue full)",
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id),
                        upc_msg_module_name(msg.target_id));
        return false;
    }
    return true;
}

bool IpcMsgBus::publish_async(const middleware::Message& msg)
{
    bool ok = send_outbound_from_local(msg, true);
    if (needs_local_handling(msg) && !enqueue_local(msg)) {
        ok = false;
    }
    return ok;
}

bool IpcMsgBus::publish_sync(const middleware::Message& msg)
{
    bool ok = send_outbound_from_local(msg, false);
    if (needs_local_handling(msg) && !deliver_local(msg)) {
        ok = false;
    }
    return ok;
}

bool IpcMsgBus::client_send_reg()
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return false;
    }
    std::vector<uint8_t> frame;
    if (!ipc_reg_encode(local_id_, frame) || !client_endpoint_.send_frame(frame)) {
        UPC_LOG_IPC(Error, "client reg send failed local=%s",
                    upc_msg_module_name(local_id_));
        return false;
    }
    UPC_LOG_IPC(Info, "client reg sent local=%s", upc_msg_module_name(local_id_));
    return true;
}

void IpcMsgBus::apply_reg_notify(uint32_t event, middleware::ModuleId module_id)
{
    const uint32_t key = static_cast<uint32_t>(module_id);
    std::lock_guard<std::mutex> lock(online_peers_mtx_);
    if (event == IPC_REG_EVENT_ONLINE) {
        online_remote_peers_.insert(key);
    } else if (event == IPC_REG_EVENT_OFFLINE) {
        online_remote_peers_.erase(key);
    }
    UPC_LOG_IPC(Info, "client reg notify event=%u module=%s",
                event, upc_msg_module_name(module_id));
}

void IpcMsgBus::deliver_client_frames(const std::vector<InboundWireUnit>& units)
{
    for (const auto& unit : units) {
        if (ipc_reg_is_control_frame(unit.frame.data(), unit.frame.size())) {
            uint32_t event = 0;
            middleware::ModuleId module_id{0};
            if (ipc_reg_notify_decode(unit.frame.data(), unit.frame.size(), event,
                                      module_id)) {
                apply_reg_notify(event, module_id);
            }
            continue;
        }

        middleware::Message msg;
        if (!wire_decode(unit.frame.data(), unit.frame.size(), msg)) {
            UPC_LOG_IPC(Warn, "client wire_decode fail frame_bytes=%zu",
                        unit.frame.size());
            continue;
        }
        if (!unit.fds.empty()) {
            stash_pending_dma_fds(msg.sender_id, unit.fds);
            UPC_LOG_IPC(Info, "IPC_RX fds %s from %s count=%zu",
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id), unit.fds.size());
        }
        upc_msg_log_message("IPC_RX", msg);
        if (needs_local_handling(msg)) {
            deliver_local(msg);
        }
    }
}

void IpcMsgBus::broker_broadcast_reg_notify(uint32_t event,
                                            middleware::ModuleId module_id)
{
    std::vector<uint8_t> frame;
    if (!ipc_reg_notify_encode(event, module_id, frame)) {
        return;
    }

    std::lock_guard<std::mutex> lock(broker_clients_mtx_);
    for (auto& slot : broker_clients_) {
        if (!slot || !slot->registered || !slot->endpoint ||
            !slot->endpoint->connected()) {
            continue;
        }
        (void)slot->endpoint->send_frame(frame);
    }
}

void IpcMsgBus::broker_send_reg_snapshot(BrokerClientSlot& slot)
{
    std::vector<middleware::ModuleId> peers;
    {
        std::lock_guard<std::mutex> lock(broker_clients_mtx_);
        peers.reserve(broker_module_index_.size());
        for (const auto& kv : broker_module_index_) {
            if (kv.second == &slot || !kv.second || !kv.second->registered) {
                continue;
            }
            peers.push_back(static_cast<middleware::ModuleId>(kv.first));
        }
    }

    for (const auto peer_id : peers) {
        std::vector<uint8_t> frame;
        if (!ipc_reg_notify_encode(IPC_REG_EVENT_ONLINE, peer_id, frame)) {
            continue;
        }
        if (slot.endpoint && slot.endpoint->connected()) {
            (void)slot.endpoint->send_frame(frame);
        }
    }
}

void IpcMsgBus::broker_unregister_client(BrokerClientSlot& slot,
                                         bool notify_offline)
{
    const middleware::ModuleId module_id = slot.module_id;
    const bool was_registered              = slot.registered;

    if (slot.endpoint && slot.endpoint->connected()) {
        poll_unregister(slot.endpoint->conn_fd());
    }

    if (was_registered) {
        std::lock_guard<std::mutex> lock(broker_clients_mtx_);
        const uint32_t key = static_cast<uint32_t>(module_id);
        auto it            = broker_module_index_.find(key);
        if (it != broker_module_index_.end() && it->second == &slot) {
            broker_module_index_.erase(it);
        }
    }

    slot.registered = false;
    slot.module_id  = middleware::ModuleId{0};
    if (slot.endpoint) {
        slot.endpoint->close();
    }

    {
        std::lock_guard<std::mutex> lock(broker_clients_mtx_);
        broker_clients_.erase(
            std::remove_if(broker_clients_.begin(), broker_clients_.end(),
                           [&slot](const std::unique_ptr<BrokerClientSlot>& p) {
                               return p.get() == &slot;
                           }),
            broker_clients_.end());
    }

    if (was_registered && notify_offline &&
        module_id != middleware::ModuleId{0}) {
        broker_broadcast_reg_notify(IPC_REG_EVENT_OFFLINE, module_id);
        UPC_LOG_IPC(Info, "broker client offline %s",
                    upc_msg_module_name(module_id));
    }
}

bool IpcMsgBus::broker_route_wire(BrokerClientSlot& from,
                                  const InboundWireUnit& unit)
{
    middleware::ModuleId sender_id{0};
    middleware::ModuleId target_id{0};
    if (!wire_peek_route(unit.frame.data(), unit.frame.size(), sender_id,
                         target_id)) {
        UPC_LOG_IPC(Warn, "broker wire_peek_route fail bytes=%zu",
                    unit.frame.size());
        return false;
    }
    if (from.registered && sender_id != from.module_id) {
        UPC_LOG_IPC(Error, "broker sender mismatch conn=%s frame=%s",
                    upc_msg_module_name(from.module_id),
                    upc_msg_module_name(sender_id));
        return false;
    }

    const auto forward_to = [&](BrokerClientSlot* dst) -> bool {
        if (!dst || !dst->endpoint || !dst->endpoint->connected()) {
            return false;
        }
        if (!unit.fds.empty()) {
            return dst->endpoint->send_frame_with_fds(unit.frame, unit.fds);
        }
        return dst->endpoint->send_frame(unit.frame);
    };

    if (target_id == middleware::ModuleId{0}) {
        std::vector<BrokerClientSlot*> targets;
        {
            std::lock_guard<std::mutex> lock(broker_clients_mtx_);
            targets.reserve(broker_clients_.size());
            for (auto& client : broker_clients_) {
                if (!client || !client->registered || client.get() == &from) {
                    continue;
                }
                targets.push_back(client.get());
            }
        }

        bool any = false;
        for (BrokerClientSlot* dst : targets) {
            if (forward_to(dst)) {
                any = true;
            }
        }
        if (!any) {
            UPC_LOG_IPC(Warn, "broker broadcast miss sender=%s",
                        upc_msg_module_name(sender_id));
        }
        return any;
    }

    BrokerClientSlot* dst = nullptr;
    {
        std::lock_guard<std::mutex> lock(broker_clients_mtx_);
        const auto it =
            broker_module_index_.find(static_cast<uint32_t>(target_id));
        if (it != broker_module_index_.end()) {
            dst = it->second;
        }
    }
    if (!dst || dst == &from) {
        UPC_LOG_IPC(Warn, "broker route miss %s->%s",
                    upc_msg_module_name(sender_id),
                    upc_msg_module_name(target_id));
        return false;
    }
    if (!forward_to(dst)) {
        UPC_LOG_IPC(Warn, "broker route miss %s->%s",
                    upc_msg_module_name(sender_id),
                    upc_msg_module_name(target_id));
        return false;
    }
    return true;
}

void IpcMsgBus::handle_broker_client_io(BrokerClientSlot& slot)
{
    if (!slot.endpoint) {
        return;
    }

    const bool was_connected = slot.endpoint->connected();
    std::vector<InboundWireUnit> units;
    slot.endpoint->recv_pending(units);

    if (was_connected && !slot.endpoint->connected()) {
        broker_unregister_client(slot, true);
        return;
    }

    for (const auto& unit : units) {
        if (!slot.registered) {
            middleware::ModuleId reg_id{0};
            if (!ipc_reg_decode(unit.frame.data(), unit.frame.size(), reg_id)) {
                UPC_LOG_IPC(Warn, "broker reg decode fail — closing client");
                broker_unregister_client(slot, false);
                return;
            }

            BrokerClientSlot* kicked = nullptr;
            {
                std::lock_guard<std::mutex> lock(broker_clients_mtx_);
                const uint32_t key = static_cast<uint32_t>(reg_id);
                auto it            = broker_module_index_.find(key);
                if (it != broker_module_index_.end() && it->second != &slot) {
                    kicked = it->second;
                }
                slot.module_id  = reg_id;
                slot.registered = true;
                broker_module_index_[key] = &slot;
            }

            if (kicked) {
                UPC_LOG_IPC(Warn, "broker kick old %s",
                            upc_msg_module_name(reg_id));
                broker_unregister_client(*kicked, true);
            }

            broker_send_reg_snapshot(slot);
            broker_broadcast_reg_notify(IPC_REG_EVENT_ONLINE, reg_id);
            UPC_LOG_IPC(Info, "broker client registered %s",
                        upc_msg_module_name(reg_id));
            continue;
        }

        if (ipc_reg_is_control_frame(unit.frame.data(), unit.frame.size())) {
            continue;
        }
        (void)broker_route_wire(slot, unit);
    }
}

void IpcMsgBus::try_reconnect_broker()
{
    if (mode_ != IpcBusMode::Client || broker_path_.empty() ||
        client_endpoint_.connected()) {
        return;
    }

    const auto now      = Clock::now();
    const auto interval = std::chrono::milliseconds(
        UPC_IPC_PEER_RECONNECT_INTERVAL_MS);
    if (client_last_reconnect_ != Clock::time_point{} &&
        now - client_last_reconnect_ < interval) {
        return;
    }
    client_last_reconnect_ = now;

    {
        std::lock_guard<std::mutex> lock(online_peers_mtx_);
        online_remote_peers_.clear();
    }

    if (!client_endpoint_.connect_to(broker_path_.c_str()) || !client_send_reg()) {
        client_endpoint_.close();
        return;
    }
    poll_register_client_conn();
    UPC_LOG_IPC(Info, "client reconnected broker=%s", broker_path_.c_str());
}

bool IpcMsgBus::poll_register_broker_listen()
{
#if !defined(__linux__)
    return false;
#else
    if (!poller_.valid()) {
        return false;
    }
    const int fd = server_endpoint_.listen_fd();
    if (fd < 0 || poll_slots_.count(fd) != 0) {
        return fd >= 0;
    }

    auto slot       = std::make_unique<PollSlot>();
    slot->kind      = PollKind::BrokerListen;
    slot->fd        = fd;
    PollSlot* raw   = slot.get();
    if (!poller_.add(fd, kPollListenEvents, raw)) {
        return false;
    }
    poll_slots_.emplace(fd, std::move(slot));
    return true;
#endif
}

bool IpcMsgBus::poll_register_broker_client(BrokerClientSlot& client_slot)
{
#if !defined(__linux__)
    (void)client_slot;
    return false;
#else
    if (!poller_.valid() || !client_slot.endpoint) {
        return false;
    }
    const int fd = client_slot.endpoint->conn_fd();
    if (fd < 0 || poll_slots_.count(fd) != 0) {
        return fd >= 0;
    }

    auto slot             = std::make_unique<PollSlot>();
    slot->kind            = PollKind::BrokerClient;
    slot->fd              = fd;
    slot->broker_client   = &client_slot;
    PollSlot* raw         = slot.get();
    if (!poller_.add(fd, kPollInEvents, raw)) {
        return false;
    }
    poll_slots_.emplace(fd, std::move(slot));
    return true;
#endif
}

bool IpcMsgBus::poll_register_client_conn()
{
#if !defined(__linux__)
    return false;
#else
    if (!poller_.valid()) {
        return false;
    }
    const int fd = client_endpoint_.conn_fd();
    if (fd < 0 || poll_slots_.count(fd) != 0) {
        return fd >= 0;
    }

    auto slot     = std::make_unique<PollSlot>();
    slot->kind    = PollKind::ClientConn;
    slot->fd      = fd;
    PollSlot* raw = slot.get();
    if (!poller_.add(fd, kPollInEvents, raw)) {
        return false;
    }
    poll_slots_.emplace(fd, std::move(slot));
    return true;
#endif
}

void IpcMsgBus::poll_unregister(int fd)
{
    if (fd < 0) {
        return;
    }
    poller_.del(fd);
    poll_slots_.erase(fd);
}

void IpcMsgBus::poll_clear()
{
    for (const auto& kv : poll_slots_) {
        poller_.del(kv.first);
    }
    poll_slots_.clear();
}

void IpcMsgBus::accept_broker_client()
{
    const int fd = server_endpoint_.accept_connection();
    if (fd < 0) {
        return;
    }

    auto client_slot  = std::make_unique<BrokerClientSlot>();
    client_slot->endpoint = std::make_unique<IpcEndpoint>();
    if (!client_slot->endpoint->attach_connected_fd(fd)) {
        return;
    }

    BrokerClientSlot* raw = client_slot.get();
    {
        std::lock_guard<std::mutex> lock(broker_clients_mtx_);
        broker_clients_.push_back(std::move(client_slot));
    }
    poll_register_broker_client(*raw);
    UPC_LOG_IPC(Info, "broker accepted client fd=%d", fd);
}

void IpcMsgBus::handle_poll_event(PollSlot* slot, uint32_t events)
{
#if !defined(__linux__)
    (void)slot;
    (void)events;
    return;
#else
    if (!slot) {
        return;
    }

    const int reg_fd = slot->fd;

    if (slot->kind == PollKind::BrokerListen) {
        if (events & EPOLLIN) {
            accept_broker_client();
        }
        return;
    }

    if (slot->kind == PollKind::BrokerClient) {
        if (slot->broker_client) {
            handle_broker_client_io(*slot->broker_client);
        }
        return;
    }

    if (slot->kind == PollKind::ClientConn) {
        const bool was_connected = client_endpoint_.connected();
        std::vector<InboundWireUnit> units;
        client_endpoint_.recv_pending(units);
        if (was_connected && !client_endpoint_.connected()) {
            poll_unregister(reg_fd);
            std::lock_guard<std::mutex> lock(online_peers_mtx_);
            online_remote_peers_.clear();
            UPC_LOG_IPC(Warn, "client broker disconnected");
        } else {
            deliver_client_frames(units);
        }
    }
#endif
}

void IpcMsgBus::poll_io(int timeout_ms)
{
#if !defined(__linux__)
    pump_inbound_fallback(timeout_ms);
    return;
#else
    if (!poller_.valid()) {
        pump_inbound_fallback(timeout_ms);
        return;
    }

    std::vector<PollEvent> events;
    if (poller_.wait(events, timeout_ms) < 0) {
        return;
    }
    for (const auto& ev : events) {
        handle_poll_event(static_cast<PollSlot*>(ev.user_data), ev.events);
    }
#endif
}

void IpcMsgBus::pump_inbound_fallback(int timeout_ms)
{
    if (timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    }

    if (mode_ == IpcBusMode::Broker) {
        accept_broker_client();
        std::vector<BrokerClientSlot*> snapshot;
        {
            std::lock_guard<std::mutex> lock(broker_clients_mtx_);
            snapshot.reserve(broker_clients_.size());
            for (auto& client : broker_clients_) {
                snapshot.push_back(client.get());
            }
        }
        for (BrokerClientSlot* client : snapshot) {
            if (client) {
                handle_broker_client_io(*client);
            }
        }
        return;
    }

    if (mode_ == IpcBusMode::Client) {
        try_reconnect_broker();
        if (client_endpoint_.connected()) {
            std::vector<InboundWireUnit> units;
            client_endpoint_.recv_pending(units);
            deliver_client_frames(units);
        }
    }
}

void IpcMsgBus::process_local_queue()
{
    middleware::PriorityMsgQueue local;
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        local.swap(local_queue_);
    }

    middleware::Message msg;
    while (local.pop(msg)) {
        deliver_local(msg);
    }
}

void IpcMsgBus::process(int io_timeout_ms)
{
    if (mode_ == IpcBusMode::Client) {
        try_reconnect_broker();
    }
    poll_io(io_timeout_ms);
    if (mode_ != IpcBusMode::Broker) {
        process_local_queue();
    }
}

} // namespace upc_ipc
