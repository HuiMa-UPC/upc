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

#include <unistd.h>

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
constexpr size_t kPendingOutboundMax = 256;
constexpr uint8_t kPendingOutboundMaxAttempts = 8;
constexpr auto kPendingOutboundMaxAge = std::chrono::seconds(15);

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
        /* 这些 fd 的所有权在 bus 手里（recvmsg 收到但还没人 take），
         * 直接 clear 就是泄漏。 */
        std::lock_guard<std::mutex> lock(pending_dma_fds_mtx_);
        for (auto& kv : pending_dma_fds_) {
            for (int fd : kv.second) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        }
        pending_dma_fds_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pending_outbound_mtx_);
        pending_outbound_.clear();
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
    /* id 的自增必须在锁内：next_sub_id_ 是普通 uint32_t，两个线程同时订阅会
     * 拿到同一个 id，之后 unsubscribe(id) 会把别人的订阅一起摘掉。 */
    std::lock_guard<std::mutex> lock(subs_mtx_);
    const uint32_t id = next_sub_id_++;
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

bool IpcMsgBus::send_to_peer_with_fds(const middleware::Message& msg,
                                      const std::vector<int>& fds)
{
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
    std::vector<int> discarded;
    {
        std::lock_guard<std::mutex> lock(pending_dma_fds_mtx_);
        auto& slot = pending_dma_fds_[key];
        /* dma 帧缓冲是"只认最新"语义：消费者一旦停止 take_pending_dma_fds（预览
         * 关闭 / 看门狗重启 / UI 不在预览页），旧批次永远不会被取走。必须就地
         * close，否则相机每次开流推 CAMERA_V4L2_BUF_COUNT 个 fd，长跑必然 EMFILE
         * —— 那会让 accept/recvmsg 全失败，整条消息总线瘫痪。 */
        discarded.swap(slot);
        for (int fd : fds) {
            if (fd >= 0) {
                slot.push_back(fd);
            }
        }
    }

    /* close 放在锁外：避免在持锁期间做系统调用。 */
    if (!discarded.empty()) {
        UPC_LOG_IPC(Warn,
                    "pending dma fds dropped peer=%s count=%zu (consumer not draining)",
                    upc_msg_module_name(sender), discarded.size());
        for (int fd : discarded) {
            if (fd >= 0) {
                ::close(fd);
            }
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

bool IpcMsgBus::should_buffer_outbound(const middleware::Message& msg) const
{
    if (msg.priority >= middleware::MsgPriority::High) {
        return true;
    }
    return middleware::is_ipc_reliable_msg_type(msg.type);
}

void IpcMsgBus::enqueue_pending_outbound(const middleware::Message& msg,
                                         const char* reason)
{
    if (!should_buffer_outbound(msg)) {
        /* 不在可靠白名单里的消息发失败就地丢弃。丢弃本身是设计意图（高频状态流
         * 不该缓存），但必须留痕：控制类请求若漏登记白名单，此前是连一条日志都
         * 没有，表现为"UI 点了按钮后端毫无反应"且无从排查。 */
        UPC_LOG_IPC(Warn, "outbound dropped (not reliable) %s %s->%s reason=%s",
                    upc_msg_type_name(msg.type),
                    upc_msg_module_name(msg.sender_id),
                    upc_msg_module_name(msg.target_id),
                    reason ? reason : "unknown");
        return;
    }
    std::lock_guard<std::mutex> lock(pending_outbound_mtx_);
    if (middleware::is_ipc_latest_only_msg_type(msg.type)) {
        for (auto it = pending_outbound_.rbegin(); it != pending_outbound_.rend(); ++it) {
            if (it->msg.type == msg.type &&
                it->msg.sender_id == msg.sender_id &&
                it->msg.target_id == msg.target_id) {
                it->msg = msg;
                it->attempts = 0;
                it->first_enqueue = Clock::now();
                UPC_LOG_IPC(Debug, "pending outbound replaced latest %s %s->%s",
                            upc_msg_type_name(msg.type),
                            upc_msg_module_name(msg.sender_id),
                            upc_msg_module_name(msg.target_id));
                return;
            }
        }
    }
    if (pending_outbound_.size() >= kPendingOutboundMax) {
        pending_outbound_.pop_front();
        UPC_LOG_IPC(Warn, "pending outbound overflow drop oldest size=%zu",
                    pending_outbound_.size());
    }
    PendingOutbound entry{};
    entry.msg = msg;
    entry.first_enqueue = Clock::now();
    pending_outbound_.push_back(std::move(entry));
    UPC_LOG_IPC(Warn, "pending outbound queued %s %s->%s reason=%s depth=%zu",
                upc_msg_type_name(msg.type),
                upc_msg_module_name(msg.sender_id),
                upc_msg_module_name(msg.target_id),
                reason ? reason : "unknown",
                pending_outbound_.size());
}

void IpcMsgBus::flush_pending_outbound()
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return;
    }

    std::deque<PendingOutbound> pending;
    {
        std::lock_guard<std::mutex> lock(pending_outbound_mtx_);
        if (pending_outbound_.empty()) {
            return;
        }
        pending.swap(pending_outbound_);
    }

    std::deque<PendingOutbound> keep;
    const auto now = Clock::now();
    for (auto& entry : pending) {
        if (send_outbound_from_local(entry.msg, false, false)) {
            continue;
        }
        const auto age = now - entry.first_enqueue;
        ++entry.attempts;
        if (entry.attempts >= kPendingOutboundMaxAttempts ||
            age >= kPendingOutboundMaxAge) {
            UPC_LOG_IPC(Warn, "pending outbound drop %s %s->%s attempts=%u age_ms=%lld",
                        upc_msg_type_name(entry.msg.type),
                        upc_msg_module_name(entry.msg.sender_id),
                        upc_msg_module_name(entry.msg.target_id),
                        static_cast<unsigned>(entry.attempts),
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(age).count()));
            continue;
        }
        keep.push_back(std::move(entry));
    }

    if (!keep.empty()) {
        std::lock_guard<std::mutex> lock(pending_outbound_mtx_);
        while (!keep.empty()) {
            pending_outbound_.push_back(std::move(keep.front()));
            keep.pop_front();
            if (pending_outbound_.size() > kPendingOutboundMax) {
                pending_outbound_.pop_front();
            }
        }
    }
}

bool IpcMsgBus::send_outbound_from_local(const middleware::Message& msg,
                                         bool warn_on_fail,
                                         bool allow_buffer)
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

    if (msg.target_id != middleware::ModuleId{0} &&
        !peer_connected(msg.target_id)) {
        /* "离线"可能是假的：online_remote_peers_ 只是 reg notify 维护的缓存，
         * 丢一条 ONLINE 就会永久误判，进而把所有发往该 peer 的消息全部拒掉。
         * 这里顺手踢一次对齐请求，下一轮 flush_pending_outbound 就能补发出去。 */
        client_request_peer_resync();
        if (allow_buffer) {
            enqueue_pending_outbound(msg, "peer_offline");
        }
        return false;
    }

    /* 只发一次，且**不**在这里重连。
     *
     * publish 会被任意业务线程调用（Glavo link 线程、WifiManager 回调、MQTT
     * 线程），而 try_reconnect_broker() 会走到 poll_register_client_conn() 去
     * 无锁改写 poll_slots_，与泵线程的 poll_io()/handle_poll_event() 直接竞争
     * 同一个 unordered_map——那是 UB。重连只允许发生在泵线程的 process() 里。
     *
     * 原先的"3 次即时重试 + 每次 sleep 2ms"也一并去掉：它会把调用方阻塞 4ms
     * 以上，而重试职责本就由 pending_outbound_ 承担（下一次 process() 会 flush）。 */
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
    if (allow_buffer) {
        enqueue_pending_outbound(msg, "tx_fail");
    }
    return false;
}

bool IpcMsgBus::publish_async(const middleware::Message& msg)
{
    return send_outbound_from_local(msg, true);
}

bool IpcMsgBus::publish_sync(const middleware::Message& msg)
{
    // 同步发送保持“即时成功/失败”语义，不进入本地补发队列。
    return send_outbound_from_local(msg, false, false);
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

void IpcMsgBus::client_request_peer_resync()
{
    if (mode_ != IpcBusMode::Client || !client_endpoint_.connected()) {
        return;
    }

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               Clock::now().time_since_epoch()).count();
    int64_t last_ms = client_last_peer_resync_ms_.load(std::memory_order_relaxed);
    if (last_ms != 0 && now_ms - last_ms < UPC_IPC_PEER_RESYNC_INTERVAL_MS) {
        return;
    }
    /* CAS 兜住并发 publish：只让抢到的那个线程真发 REG，其余直接返回。 */
    if (!client_last_peer_resync_ms_.compare_exchange_strong(
            last_ms, now_ms, std::memory_order_relaxed)) {
        return;
    }

    /* broker 在已注册连接上收到 REG 会重发在线快照（见 handle_broker_client_io），
     * 于是丢掉的 ONLINE 通知不再是永久故障。 */
    if (client_send_reg()) {
        UPC_LOG_IPC(Info, "client peer resync: REG re-sent to realign peer list");
    }
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
            UPC_LOG_IPC(Warn, "client wire_decode fail frame_bytes=%zu fds=%zu",
                        unit.frame.size(), unit.fds.size());
            /* 这条路径下 fd 不会进 pending_dma_fds_，没人再负责它们。
             * InboundWireUnit 是裸结构体（所有权按路径区分，见
             * handle_broker_client_io 的 InboundFdGuard 注释），
             * 丢弃报文时必须自己关，否则解码失败一次就漏一组 dma-buf。 */
            for (int fd : unit.fds) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
            continue;
        }
        if (!unit.fds.empty()) {
            stash_pending_dma_fds(msg.sender_id, unit.fds);
            UPC_LOG_IPC(Info, "IPC_RX fds %s from %s count=%zu",
                        upc_msg_type_name(msg.type),
                        upc_msg_module_name(msg.sender_id), unit.fds.size());
        }
        upc_msg_log_message("IPC_RX", msg);
        if (mode_ != IpcBusMode::Broker &&
            (msg.target_id == middleware::ModuleId{0} ||
             msg.target_id == local_id_)) {
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
        if (!slot->endpoint->send_frame(frame)) {
            /* 这一条丢了，对端的 online_remote_peers_ 就与 broker 分叉：丢
             * ONLINE → 它永久认为该 peer 离线并拒发所有消息。send_frame 内部
             * 失败时已关连接，随后 handle_broker_client_io 会摘掉该 slot 并
             * 由它重连对齐；此处不做摘除（正持有 broker_clients_mtx_，
             * broker_unregister_client 会重入同一把锁），只负责留痕。 */
            UPC_LOG_IPC(Warn,
                        "broker reg notify send fail event=%u module=%s to=%s",
                        event, upc_msg_module_name(module_id),
                        upc_msg_module_name(slot->module_id));
        }
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
        if (slot.endpoint && slot.endpoint->connected() &&
            !slot.endpoint->send_frame(frame)) {
            /* 快照本身就是对齐手段，丢了等于对齐失败；client 侧节流重发 REG
             * 会再来一次，这里留痕即可。 */
            UPC_LOG_IPC(Warn, "broker reg snapshot send fail peer=%s to=%s",
                        upc_msg_module_name(peer_id),
                        upc_msg_module_name(slot.module_id));
        }
    }
}

void IpcMsgBus::broker_unregister_client(BrokerClientSlot& slot,
                                         bool notify_offline)
{
    const middleware::ModuleId module_id = slot.module_id;
    const bool was_registered              = slot.registered;

    /* 无条件按槽位反注册：断连基本都是在 recv_pending 内部 close 之后才被发现的
     * （handle_broker_client_io 的 was_connected && !connected() 分支），此时
     * conn_fd 已是 -1，旧的"仅在 connected() 时按 fd 反注册"必然漏掉，留下的
     * 陈旧槽位会让复用同一 fd 号的新连接注册被跳过，从此彻底失联。 */
    poll_unregister_broker_client(slot);

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

    /*
     * broker 只转发不消费：send_frame_with_fds 走 SCM_RIGHTS，内核已把 fd 复制
     * 一份给目标进程，broker 自己 recvmsg 收到的这一份用完必须关掉，否则每转发
     * 一组 dma-buf fd 就漏一组——相机每次开流 / 看门狗重启都会向各 peer 推
     * CAMERA_V4L2_BUF_COUNT 个 fd，长跑后 broker 触发 EMFILE，accept/recvmsg
     * 全失败，整条消息总线瘫痪。
     *
     * 为什么不把 close 放进 InboundWireUnit 的析构：client 路径
     * （deliver_client_frames）靠 stash_pending_dma_fds 把这些整数移交给
     * pending_dma_fds_，由消费者 take_pending_dma_fds 之后负责 close。若在析构
     * 里统一关，client 侧就会对同一个 fd 双重 close。所有权差异只存在于 broker
     * 与 client 两条路径之间，因此只在这里兜。
     *
     * 用 scope guard 而不是循环末尾 close：下面的分支里有多处 early return
     * （注册帧解码失败、被踢下线等），逐个补 close 容易漏。
     */
    struct InboundFdGuard {
        std::vector<InboundWireUnit>& units;
        ~InboundFdGuard()
        {
            for (auto& unit : units) {
                for (int fd : unit.fds) {
                    if (fd >= 0) {
                        ::close(fd);
                    }
                }
                unit.fds.clear();
            }
        }
    } fd_guard{units};

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
            /* 已注册连接上再收到 REG：这是 client 的 peer 列表对齐请求（它认为
             * 某个 peer 离线，但很可能只是丢了一条 ONLINE 通知）。重发一次在线
             * 快照就能自愈，不能当噪音丢掉。REG_NOTIFY 是 broker→client 方向，
             * client 发来的不予处理（ipc_reg_decode 会失败）。 */
            middleware::ModuleId reg_id{0};
            if (ipc_reg_decode(unit.frame.data(), unit.frame.size(), reg_id) &&
                reg_id == slot.module_id) {
                broker_send_reg_snapshot(slot);
                UPC_LOG_IPC(Info, "broker reg snapshot resent %s",
                            upc_msg_module_name(slot.module_id));
            }
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

    /* 连接可能是在 send 失败路径里被 close 的（send_stream_bytes_locked →
     * close_conn_locked），那条路径不经过 poll，槽位会残留。先清掉再重连，
     * 否则新 fd 复用同一号码时注册会被当成陈旧槽位处理（虽已能覆盖，但会
     * 多打一条 Warn，且 fd 号不同时旧条目会一直留在表里）。 */
    poll_unregister_client_conn();

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
    if (fd < 0) {
        return false;
    }
    poll_drop_stale_slot(fd);

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
    if (fd < 0) {
        return false;
    }
    poll_drop_stale_slot(fd);

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
    if (fd < 0) {
        return false;
    }
    poll_drop_stale_slot(fd);

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

bool IpcMsgBus::poll_slot_alive(const PollSlot* slot) const
{
    if (!slot) {
        return false;
    }
    for (const auto& kv : poll_slots_) {
        if (kv.second.get() == slot) {
            return true;
        }
    }
    return false;
}

void IpcMsgBus::poll_unregister_broker_client(BrokerClientSlot& slot)
{
    for (auto it = poll_slots_.begin(); it != poll_slots_.end();) {
        if (it->second && it->second->kind == PollKind::BrokerClient &&
            it->second->broker_client == &slot) {
            poller_.del(it->first);
            it = poll_slots_.erase(it);
        } else {
            ++it;
        }
    }
}

void IpcMsgBus::poll_unregister_client_conn()
{
    for (auto it = poll_slots_.begin(); it != poll_slots_.end();) {
        if (it->second && it->second->kind == PollKind::ClientConn) {
            poller_.del(it->first);
            it = poll_slots_.erase(it);
        } else {
            ++it;
        }
    }
}

void IpcMsgBus::poll_drop_stale_slot(int fd)
{
    const auto it = poll_slots_.find(fd);
    if (it == poll_slots_.end()) {
        return;
    }
    /* 走到这里说明上一条连接的槽位没被清掉（断连路径漏了反注册），而内核已把
     * 这个 fd 号分配给了新连接。必须覆盖，否则新连接进不了 epoll。 */
    UPC_LOG_IPC(Warn, "poll slot stale fd=%d kind=%u replaced", fd,
                static_cast<unsigned>(it->second ? it->second->kind
                                                 : PollKind::ClientConn));
    poller_.del(fd);
    poll_slots_.erase(it);
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
        /* attach 失败时 fd 还没交给 endpoint，析构不会关它，得自己收尾。 */
        ::close(fd);
        UPC_LOG_IPC(Warn, "broker attach client fd=%d failed", fd);
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
    if (!slot || !poll_slot_alive(slot)) {
        /* 同一批 epoll 事件里，处理前一个 fd 时可能已经把这个槽位释放掉了：
         * 典型是 handle_broker_client_io 里"同一 ModuleId 重复注册"会
         * broker_unregister_client(*kicked)，而被踢的那条连接的事件很可能就排在
         * 本批后面。不校验存活就是对已释放内存解引用。 */
        return;
    }

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
            /* 按 kind 而非 conn_fd 反注册：recv_pending 内部已经 close 了连接，
             * 此时 conn_fd() 是 -1。 */
            poll_unregister_client_conn();
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

void IpcMsgBus::process(int io_timeout_ms)
{
    if (mode_ == IpcBusMode::Client) {
        try_reconnect_broker();
        flush_pending_outbound();
    }
    poll_io(io_timeout_ms);
    if (mode_ == IpcBusMode::Client) {
        flush_pending_outbound();
    }
}

} // namespace upc_ipc
