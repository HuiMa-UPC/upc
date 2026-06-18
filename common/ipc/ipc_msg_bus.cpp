#include "ipc_msg_bus.hpp"

#include "ipc_config.hpp"
#include "ipc_wire.hpp"
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
constexpr uint32_t kPollInEvents = EPOLLIN | EPOLLRDHUP;
constexpr uint32_t kPollListenEvents = EPOLLIN;
#endif

void ensure_poller_open(IpcPoller& poller) {
    if (!poller.valid()) {
        poller.reopen();
    }
}

/** Server 仅连 UI Hub；wire 保留逻辑 sender，transport peer 恒为 UI */
bool ipc_sender_mismatch_warn(IpcBusMode mode, middleware::ModuleId expected_sender,
                              middleware::ModuleId wire_sender)
{
    if (expected_sender == middleware::ModuleId{0} ||
        wire_sender == middleware::ModuleId{0}) {
        return false;
    }
    if (wire_sender == expected_sender) {
        return false;
    }
    if (mode == IpcBusMode::Server &&
        expected_sender == middleware::ModuleId::UI) {
        return false;
    }
    return true;
}

} // namespace

IpcMsgBus& IpcMsgBus::instance()
{
    static IpcMsgBus inst;
    return inst;
}

bool IpcMsgBus::init_server(middleware::ModuleId local_id,
                            const char* socket_path)
{
    if (initialized_ && mode_ == IpcBusMode::Server) {
        return true;
    }
    if (initialized_) {
        shutdown();
    }

    local_id_       = local_id;
    server_peer_id_ = middleware::ModuleId::UI;
    mode_           = IpcBusMode::Server;

    middleware::MemPool::instance().init();
    middleware::BufferPool::instance().init();

    if (!server_endpoint_.listen_on(socket_path)) {
        UPC_LOG_IPC(Error, "listen failed path=%s",
                    socket_path ? socket_path : "(null)");
        mode_ = IpcBusMode::None;
        return false;
    }

    poll_clear();
    ensure_poller_open(poller_);
    poll_register_server_listen();

    initialized_ = true;
    UPC_LOG_IPC(Info, "server listen path=%s local=%s peer=%s",
                socket_path ? socket_path : "(null)",
                upc_msg_module_name(local_id_),
                upc_msg_module_name(server_peer_id_));
    return true;
}

bool IpcMsgBus::init_hub(middleware::ModuleId local_id)
{
    if (initialized_ && mode_ == IpcBusMode::Hub) {
        return true;
    }
    if (initialized_) {
        shutdown();
    }

    local_id_ = local_id;
    mode_     = IpcBusMode::Hub;

    middleware::MemPool::instance().init();
    middleware::BufferPool::instance().init();

    poll_clear();
    ensure_poller_open(poller_);

    initialized_ = true;
    UPC_LOG_IPC(Info, "hub init local=%s", upc_msg_module_name(local_id_));
    return true;
}

bool IpcMsgBus::add_peer(middleware::ModuleId peer_id, const char* socket_path,
                         bool required, bool connect_now)
{
    if (mode_ != IpcBusMode::Hub || !initialized_) {
        UPC_LOG_IPC(Error, "add_peer requires hub mode");
        return false;
    }
    if (!socket_path || !socket_path[0]) {
        return false;
    }

    std::lock_guard<std::mutex> lock(peers_mtx_);
    for (const auto& p : peers_) {
        if (p->peer_id == peer_id) {
            UPC_LOG_IPC(Warn, "add_peer duplicate %s",
                        upc_msg_module_name(peer_id));
            return false;
        }
    }

    peers_.push_back(std::make_unique<PeerEntry>());
    PeerEntry& entry = *peers_.back();
    entry.peer_id     = peer_id;
    entry.socket_path = socket_path;
    entry.required    = required;
    entry.last_reconnect_attempt = Clock::time_point{};

    if (connect_now && !entry.endpoint.connect_to(socket_path)) {
        UPC_LOG_IPC(Warn, "add_peer connect failed %s path=%s",
                    upc_msg_module_name(peer_id), socket_path);
        if (required) {
            peers_.pop_back();
            return false;
        }
    } else if (connect_now) {
        poll_register_hub_peer(entry);
    }

    UPC_LOG_IPC(Info, "add_peer %s path=%s required=%d connected=%d",
                upc_msg_module_name(peer_id), socket_path,
                required ? 1 : 0,
                entry.endpoint.connected() ? 1 : 0);
    return true;
}

bool IpcMsgBus::init_client(middleware::ModuleId local_id,
                            const char* socket_path)
{
    if (!init_hub(local_id)) {
        return false;
    }
    return add_peer(middleware::ModuleId::APP_GLAVO, socket_path, true, true);
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
    {
        std::lock_guard<std::mutex> lock(peers_mtx_);
        for (auto& p : peers_) {
            p->endpoint.close();
        }
        peers_.clear();
    }

    initialized_ = false;
    mode_        = IpcBusMode::None;
    UPC_LOG_IPC(Info, "shutdown");
}

void IpcMsgBus::try_accept_peer()
{
    if (mode_ == IpcBusMode::Server) {
        server_endpoint_.try_accept();
    }
}

bool IpcMsgBus::peer_connected() const noexcept
{
    if (mode_ == IpcBusMode::Server) {
        return server_endpoint_.connected();
    }
    if (mode_ != IpcBusMode::Hub) {
        return false;
    }

    std::lock_guard<std::mutex> lock(peers_mtx_);
    for (const auto& p : peers_) {
        if (p->required && !p->endpoint.connected()) {
            return false;
        }
    }
    return !peers_.empty();
}

bool IpcMsgBus::peer_connected(middleware::ModuleId peer_id) const noexcept
{
    if (mode_ == IpcBusMode::Server) {
        if (peer_id == server_peer_id_) {
            return server_endpoint_.connected();
        }
        return false;
    }
    if (mode_ != IpcBusMode::Hub) {
        return false;
    }

    std::lock_guard<std::mutex> lock(peers_mtx_);
    for (const auto& p : peers_) {
        if (p->peer_id == peer_id) {
            return p->endpoint.connected();
        }
    }
    return false;
}

IpcMsgBus::PeerEntry* IpcMsgBus::find_peer(middleware::ModuleId id)
{
    for (auto& p : peers_) {
        if (p->peer_id == id) {
            return p.get();
        }
    }
    return nullptr;
}

const IpcMsgBus::PeerEntry* IpcMsgBus::find_peer(middleware::ModuleId id) const
{
    for (const auto& p : peers_) {
        if (p->peer_id == id) {
            return p.get();
        }
    }
    return nullptr;
}

bool IpcMsgBus::is_registered_peer(middleware::ModuleId id) const
{
    if (mode_ == IpcBusMode::Server) {
        return id == server_peer_id_;
    }
    if (mode_ != IpcBusMode::Hub) {
        return false;
    }
    std::lock_guard<std::mutex> lock(peers_mtx_);
    return find_peer(id) != nullptr;
}

void IpcMsgBus::unsubscribe(uint32_t sub_id)
{
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
        total   = subscribers_.size();
    }
    if (removed) {
        UPC_LOG_IPC(Debug, "unsubscribe id=%u total=%zu", sub_id, total);
    }
}

uint32_t IpcMsgBus::subscribe(middleware::MsgType type,
                              middleware::ModuleId module_id,
                              IpcHandler handler)
{
    const uint32_t id = next_sub_id_++;
    size_t total      = 0;
    {
        std::lock_guard<std::mutex> lock(subs_mtx_);
        subscribers_.push_back({id, type, module_id, std::move(handler)});
        total = subscribers_.size();
    }
    UPC_LOG_IPC(Debug, "subscribe id=%u type=%s module=%s total=%zu",
                id, upc_msg_type_name(type), upc_msg_module_name(module_id),
                total);
    return id;
}

bool IpcMsgBus::deliver_local(const middleware::Message& msg)
{
    std::vector<IpcHandler> handlers;
    handlers.reserve(8);
    {
        std::lock_guard<std::mutex> lock(subs_mtx_);
        for (const auto& sub : subscribers_) {
            const bool type_match =
                (sub.type == middleware::MsgType{0}) || (sub.type == msg.type);
            const bool target_match =
                (msg.target_id == middleware::ModuleId{0}) ||
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

bool IpcMsgBus::send_to_peer(middleware::ModuleId peer_id,
                             const middleware::Message& msg)
{
    std::vector<uint8_t> frame;
    if (!wire_encode(msg, frame)) {
        UPC_LOG_IPC(Error, "wire_encode fail %s %s->%s",
                    upc_msg_type_name(msg.type),
                    upc_msg_module_name(msg.sender_id),
                    upc_msg_module_name(msg.target_id));
        return false;
    }

    if (mode_ == IpcBusMode::Server) {
        if (peer_id != server_peer_id_ || !server_endpoint_.connected()) {
            return false;
        }
        if (!server_endpoint_.send_frame(frame)) {
            return false;
        }
    } else if (mode_ == IpcBusMode::Hub) {
        std::lock_guard<std::mutex> lock(peers_mtx_);
        PeerEntry* peer = find_peer(peer_id);
        if (!peer || !peer->endpoint.connected()) {
            return false;
        }
        if (!peer->endpoint.send_frame(frame)) {
            return false;
        }
    } else {
        return false;
    }

    upc_msg_log_message("IPC_TX", msg);
    return true;
}

bool IpcMsgBus::send_to_all_peers(const middleware::Message& msg)
{
    bool any_sent = false;

    if (mode_ == IpcBusMode::Server) {
        return send_to_peer(server_peer_id_, msg);
    }

    if (mode_ != IpcBusMode::Hub) {
        return false;
    }

    std::vector<uint8_t> frame;
    if (!wire_encode(msg, frame)) {
        UPC_LOG_IPC(Error, "wire_encode fail broadcast %s",
                    upc_msg_type_name(msg.type));
        return false;
    }

    std::lock_guard<std::mutex> lock(peers_mtx_);
    for (auto& peer : peers_) {
        if (!peer->endpoint.connected()) {
            continue;
        }
        if (peer->endpoint.send_frame(frame)) {
            any_sent = true;
        }
    }

    if (any_sent) {
        upc_msg_log_message("IPC_TX", msg);
    }
    return any_sent;
}

bool IpcMsgBus::should_forward_to_peer(const middleware::Message& msg) const
{
    return mode_ == IpcBusMode::Hub &&
           msg.target_id != middleware::ModuleId{0} &&
           msg.target_id != local_id_ &&
           is_registered_peer(msg.target_id);
}

bool IpcMsgBus::needs_local_handling(const middleware::Message& msg) const
{
    if (msg.target_id == middleware::ModuleId{0} ||
        msg.target_id == local_id_) {
        return true;
    }
    return mode_ != IpcBusMode::Server &&
           !is_registered_peer(msg.target_id);
}

bool IpcMsgBus::send_outbound_from_local(const middleware::Message& msg,
                                         bool warn_on_fail)
{
    if (msg.sender_id != local_id_) {
        return true;
    }

    const bool broadcast = msg.target_id == middleware::ModuleId{0};
    if (broadcast) {
        if (send_to_all_peers(msg)) {
            return true;
        }
        if (warn_on_fail) {
            UPC_LOG_IPC(Warn, "IPC_TX broadcast fail %s (no peer ready)",
                        upc_msg_type_name(msg.type));
        }
        return false;
    }

    if (mode_ == IpcBusMode::Server) {
        if (msg.target_id == local_id_) {
            return true;
        }
        if (send_to_peer(server_peer_id_, msg)) {
            return true;
        }
        if (warn_on_fail) {
            UPC_LOG_IPC(Warn, "IPC_TX fail %s %s->%s (hub not ready)",
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id),
                        upc_msg_module_name(msg.target_id));
        }
        return false;
    }

    if (!is_registered_peer(msg.target_id)) {
        return true;
    }
    if (send_to_peer(msg.target_id, msg)) {
        return true;
    }
    if (warn_on_fail) {
        UPC_LOG_IPC(Warn, "IPC_TX fail %s %s->%s (peer not ready)",
                    upc_msg_type_name(msg.type),
                    upc_msg_module_name(msg.sender_id),
                    upc_msg_module_name(msg.target_id));
    }
    return false;
}

bool IpcMsgBus::enqueue_local(const middleware::Message& msg)
{
    size_t queue_depth = 0;
    bool pushed        = false;
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

void IpcMsgBus::deliver_frames(
    const std::vector<std::vector<uint8_t>>& frames,
    middleware::ModuleId expected_sender)
{
    for (const auto& frame : frames) {
        middleware::Message msg;
        if (!wire_decode(frame.data(), frame.size(), msg)) {
            UPC_LOG_IPC(Warn, "wire_decode fail frame_bytes=%zu", frame.size());
            continue;
        }
        if (ipc_sender_mismatch_warn(mode_, expected_sender, msg.sender_id)) {
            UPC_LOG_IPC(Warn, "IPC_RX sender mismatch expect=%s got=%s",
                        upc_msg_module_name(expected_sender),
                        upc_msg_module_name(msg.sender_id));
        }
        upc_msg_log_message("IPC_RX", msg);

        const bool hub_fwd = should_forward_to_peer(msg);
        if (!hub_fwd) {
            deliver_local(msg);
        } else if (!send_to_peer(msg.target_id, msg)) {
            UPC_LOG_IPC(Warn, "IPC_FWD fail %s %s->%s",
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id),
                        upc_msg_module_name(msg.target_id));
        } else {
            upc_msg_log_message("IPC_FWD", msg);
        }
    }
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

bool IpcMsgBus::poll_register_hub_peer(PeerEntry& peer)
{
#if !defined(__linux__)
    (void)peer;
    return false;
#else
    if (!poller_.valid()) {
        return false;
    }
    const int fd = peer.endpoint.conn_fd();
    if (fd < 0) {
        return false;
    }
    if (poll_slots_.count(fd) != 0) {
        return true;
    }

    auto slot  = std::make_unique<PollSlot>();
    slot->kind = PollKind::HubPeer;
    slot->hub_peer_id = peer.peer_id;
    slot->fd = fd;
    PollSlot* raw = slot.get();
    if (!poller_.add(fd, kPollInEvents, raw)) {
        return false;
    }
    poll_slots_.emplace(fd, std::move(slot));
    return true;
#endif
}

bool IpcMsgBus::poll_register_server_listen()
{
#if !defined(__linux__)
    return false;
#else
    if (!poller_.valid()) {
        return false;
    }
    const int fd = server_endpoint_.listen_fd();
    if (fd < 0) {
        return false;
    }
    if (poll_slots_.count(fd) != 0) {
        return true;
    }

    auto slot  = std::make_unique<PollSlot>();
    slot->kind = PollKind::ServerListen;
    slot->fd = fd;
    PollSlot* raw = slot.get();
    if (!poller_.add(fd, kPollListenEvents, raw)) {
        return false;
    }
    poll_slots_.emplace(fd, std::move(slot));
    return true;
#endif
}

bool IpcMsgBus::poll_register_server_conn()
{
#if !defined(__linux__)
    return false;
#else
    if (!poller_.valid()) {
        return false;
    }
    const int fd = server_endpoint_.conn_fd();
    if (fd < 0) {
        return false;
    }
    if (poll_slots_.count(fd) != 0) {
        return true;
    }

    auto slot  = std::make_unique<PollSlot>();
    slot->kind = PollKind::ServerConn;
    slot->fd = fd;
    PollSlot* raw = slot.get();
    if (!poller_.add(fd, kPollInEvents, raw)) {
        return false;
    }
    poll_slots_.emplace(fd, std::move(slot));
    return true;
#endif
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

    if (slot->kind == PollKind::ServerListen) {
        if (events & EPOLLIN) {
            const bool was_connected = server_endpoint_.connected();
            try_accept_peer();
            if (!was_connected && server_endpoint_.connected()) {
                UPC_LOG_IPC(Info, "peer connected");
                poll_register_server_conn();
            }
        }
        return;
    }

    IpcEndpoint* ep = nullptr;
    middleware::ModuleId peer_id{};
    const int reg_fd = slot->fd;

    if (slot->kind == PollKind::HubPeer) {
        peer_id = slot->hub_peer_id;
        PeerEntry* peer = nullptr;
        {
            std::lock_guard<std::mutex> lock(peers_mtx_);
            peer = find_peer(peer_id);
        }
        if (!peer) {
            poll_unregister(reg_fd);
            return;
        }
        ep = &peer->endpoint;
    } else {
        ep      = &server_endpoint_;
        peer_id = server_peer_id_;
    }

    if (!ep || !ep->connected()) {
        poll_unregister(reg_fd);
        return;
    }

    const bool was_connected = ep->connected();
    std::vector<std::vector<uint8_t>> frames;
    ep->recv_pending(frames);

    if (was_connected && !ep->connected()) {
        if (slot->kind == PollKind::HubPeer) {
            UPC_LOG_IPC(Warn, "peer disconnected %s",
                        upc_msg_module_name(slot->hub_peer_id));
        } else {
            UPC_LOG_IPC(Warn, "peer disconnected");
        }
        poll_unregister(reg_fd);
    }

    deliver_frames(frames, peer_id);
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
    const int n = poller_.wait(events, timeout_ms);
    if (n < 0) {
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

    if (mode_ == IpcBusMode::Server) {
        const bool was_connected = server_endpoint_.connected();
        try_accept_peer();
        if (!was_connected && server_endpoint_.connected()) {
            UPC_LOG_IPC(Info, "peer connected");
        }

        std::vector<std::vector<uint8_t>> frames;
        server_endpoint_.recv_pending(frames);
        if (was_connected && !server_endpoint_.connected()) {
            UPC_LOG_IPC(Warn, "peer disconnected");
        }
        deliver_frames(frames, server_peer_id_);
        return;
    }

    if (mode_ != IpcBusMode::Hub) {
        return;
    }

    struct Batch {
        middleware::ModuleId peer_id{};
        std::vector<std::vector<uint8_t>> frames;
    };
    std::vector<Batch> batches;

    {
        std::lock_guard<std::mutex> lock(peers_mtx_);
        batches.reserve(peers_.size());
        for (auto& peer : peers_) {
            const bool was_connected = peer->endpoint.connected();
            Batch batch{};
            batch.peer_id = peer->peer_id;
            peer->endpoint.recv_pending(batch.frames);

            if (was_connected && !peer->endpoint.connected()) {
                UPC_LOG_IPC(Warn, "peer disconnected %s",
                            upc_msg_module_name(peer->peer_id));
            } else if (!was_connected && peer->endpoint.connected()) {
                UPC_LOG_IPC(Info, "peer connected %s",
                            upc_msg_module_name(peer->peer_id));
            }

            if (!batch.frames.empty()) {
                batches.push_back(std::move(batch));
            }
        }
    }

    for (const auto& batch : batches) {
        deliver_frames(batch.frames, batch.peer_id);
    }
}

void IpcMsgBus::try_reconnect_peers()
{
    if (mode_ != IpcBusMode::Hub) {
        return;
    }

    const auto now     = Clock::now();
    const auto interval =
        std::chrono::milliseconds(UPC_IPC_PEER_RECONNECT_INTERVAL_MS);

    std::lock_guard<std::mutex> lock(peers_mtx_);
    for (auto& peer : peers_) {
        if (peer->endpoint.connected()) {
            continue;
        }
        if (peer->last_reconnect_attempt != Clock::time_point{} &&
            now - peer->last_reconnect_attempt < interval) {
            continue;
        }
        peer->last_reconnect_attempt = now;
        if (peer->endpoint.connect_to(peer->socket_path.c_str())) {
            UPC_LOG_IPC(Info, "peer reconnected %s",
                        upc_msg_module_name(peer->peer_id));
            poll_register_hub_peer(*peer);
        }
    }
}

void IpcMsgBus::process(int io_timeout_ms)
{
    if (mode_ == IpcBusMode::Server || mode_ == IpcBusMode::Hub) {
        if (mode_ == IpcBusMode::Hub) {
            try_reconnect_peers();
        }
        poll_io(io_timeout_ms);
    }

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
