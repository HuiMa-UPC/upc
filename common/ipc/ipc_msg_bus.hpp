#ifndef UPC_IPC_MSG_BUS_HPP
#define UPC_IPC_MSG_BUS_HPP

#include "ipc_endpoint.hpp"
#include "ipc_poller.hpp"
#include "msg_runtime.hpp"

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
        int poll_fd{-1}; /**< 注册进 poller 的 fd；endpoint 断开后 conn_fd 已失效，不能用于反注册 */
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
    void poll_unregister(int fd);
    void poll_clear();

    IpcBusMode mode_{IpcBusMode::None};
    IpcEndpoint server_endpoint_;
    IpcEndpoint client_endpoint_;
    std::string broker_path_;
    std::chrono::steady_clock::time_point client_last_reconnect_{};
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
