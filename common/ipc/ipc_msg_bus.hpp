#ifndef UPC_IPC_MSG_BUS_HPP
#define UPC_IPC_MSG_BUS_HPP

#include "ipc_endpoint.hpp"
#include "ipc_poller.hpp"
#include "msg_runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace upc_ipc {

using IpcHandler = std::function<void(const middleware::Message&)>;

enum class IpcBusMode : uint8_t {
    None   = 0,
    Broker = 1,
    Client = 2,
};

class IpcMsgBus {
public:
    static IpcMsgBus& instance();

    bool init_broker(const char* socket_path);
    bool init_ipc_client(middleware::ModuleId local_id, const char* broker_path);
    void shutdown();

    uint32_t subscribe(middleware::MsgType type,
                       middleware::ModuleId module_id,
                       IpcHandler handler);
    void unsubscribe(uint32_t sub_id);

    bool publish_async(const middleware::Message& msg);
    bool publish_sync(const middleware::Message& msg);
    void process(int io_timeout_ms = 0);

    [[nodiscard]] bool peer_connected() const noexcept;
    [[nodiscard]] bool peer_connected(middleware::ModuleId peer_id) const noexcept;

    bool send_to_peer_with_fds(const middleware::Message& msg,
                               const std::vector<int>& fds);
    bool take_pending_dma_fds(middleware::ModuleId peer_id,
                              std::vector<int>& out_fds);

private:
    enum class PollKind : uint8_t {
        BrokerListen,
        BrokerClient,
        ClientConn,
    };

    struct BrokerClientSlot {
        std::unique_ptr<IpcEndpoint> endpoint;
        middleware::ModuleId module_id{middleware::ModuleId{0}};
        bool registered{false};
    };

    struct PollSlot {
        PollKind kind{};
        BrokerClientSlot* broker_client{nullptr};
        int fd = -1;
    };

    struct LocalSub {
        uint32_t id;
        middleware::MsgType type;
        middleware::ModuleId module_id;
        IpcHandler handler;
    };

    bool deliver_local(const middleware::Message& msg);
    void deliver_client_frames(const std::vector<InboundWireUnit>& units);
    void handle_broker_client_io(BrokerClientSlot& slot);
    void broker_unregister_client(BrokerClientSlot& slot, bool notify_offline);
    void broker_broadcast_reg_notify(uint32_t event, middleware::ModuleId module_id);
    void broker_send_reg_snapshot(BrokerClientSlot& slot);
    bool broker_route_wire(BrokerClientSlot& from, const InboundWireUnit& unit);
    bool client_send_reg();
    void apply_reg_notify(uint32_t event, middleware::ModuleId module_id);
    /**
     * @brief 节流重发 REG，请求 broker 回一次在线快照以对齐 peer 列表
     * @note online_remote_peers_ 是纯事件缓存，丢一条 ONLINE 通知就永久误判离线。
     *       发现"连着 broker 但目标 peer 离线"时调用本函数自愈。
     */
    void client_request_peer_resync();
    void try_reconnect_broker();
    void accept_broker_client();
    bool send_outbound_from_local(const middleware::Message& msg, bool warn_on_fail,
                                  bool allow_buffer = true);
    bool should_buffer_outbound(const middleware::Message& msg) const;
    void enqueue_pending_outbound(const middleware::Message& msg, const char* reason);
    void flush_pending_outbound();
    bool send_wire_frame(const std::vector<uint8_t>& frame);
    bool send_wire_frame_with_fds(const std::vector<uint8_t>& frame,
                                  const middleware::Message& msg,
                                  const std::vector<int>& fds);
    void stash_pending_dma_fds(middleware::ModuleId sender, std::vector<int> fds);
    void poll_io(int timeout_ms);
    void pump_inbound_fallback(int timeout_ms);
    void handle_poll_event(PollSlot* slot, uint32_t events);
    bool poll_register_broker_client(BrokerClientSlot& slot);
    bool poll_register_client_conn();
    bool poll_register_broker_listen();
    /**
     * @brief 判断 PollSlot 指针是否还在 poll_slots_ 里（未被本轮释放）
     * @note epoll 事件里带的是裸 PollSlot*，而处理同一批中前一个事件时可能已经
     *       把这个槽位释放了（重复注册踢旧连接那条路）。解引用前必须先校验。
     */
    bool poll_slot_alive(const PollSlot* slot) const;
    /**
     * @brief 按槽位指针清除该 client 的 PollSlot（不依赖 conn_fd）
     * @note 断连检测通常发生在 recv_pending 内部 close 之后，此时 conn_fd 已是 -1，
     *       按 fd 反注册会漏掉；而 fd 号会被下一条连接复用，漏掉的陈旧槽位会让新
     *       连接的注册被跳过，从此永远收不到它的任何字节。故必须按指针匹配。
     */
    void poll_unregister_broker_client(BrokerClientSlot& slot);
    /** @brief 清除 ClientConn 的 PollSlot（同上，不依赖 conn_fd） */
    void poll_unregister_client_conn();
    /** @brief 强制丢弃 fd 上残留的陈旧槽位，供 poll_register_* 覆盖注册使用 */
    void poll_drop_stale_slot(int fd);
    void poll_clear();

    IpcBusMode mode_{IpcBusMode::None};
    IpcEndpoint server_endpoint_;
    IpcEndpoint client_endpoint_;
    std::string broker_path_;
    std::chrono::steady_clock::time_point client_last_reconnect_{};
    /* 用 atomic 而非 time_point：client_request_peer_resync 由 publish 路径调用，
     * 而 publish 可能来自任意业务线程（Glavo link 线程、WifiManager 回调等）。 */
    std::atomic<int64_t> client_last_peer_resync_ms_{0};
    std::unordered_set<uint32_t> online_remote_peers_;
    mutable std::mutex online_peers_mtx_;
    std::vector<std::unique_ptr<BrokerClientSlot>> broker_clients_;
    std::unordered_map<uint32_t, BrokerClientSlot*> broker_module_index_;
    mutable std::mutex broker_clients_mtx_;
    IpcPoller poller_;
    std::unordered_map<int, std::unique_ptr<PollSlot>> poll_slots_;
    middleware::ModuleId local_id_{middleware::ModuleId::UI};

    std::vector<LocalSub> subscribers_;
    std::mutex subs_mtx_;
    uint32_t next_sub_id_{1};
    bool initialized_{false};

    std::unordered_map<uint32_t, std::vector<int>> pending_dma_fds_;
    mutable std::mutex pending_dma_fds_mtx_;

    struct PendingOutbound {
        middleware::Message msg;
        uint8_t attempts{0};
        std::chrono::steady_clock::time_point first_enqueue{};
    };
    std::deque<PendingOutbound> pending_outbound_;
    mutable std::mutex pending_outbound_mtx_;
};

} // namespace upc_ipc

#endif
