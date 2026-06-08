#ifndef UPC_IPC_MSG_BUS_HPP
#define UPC_IPC_MSG_BUS_HPP

#include "ipc_endpoint.hpp"
#include "ipc_poller.hpp"
#include "msg_queue.hpp"
#include "msg_types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace upc_ipc {

using IpcHandler = std::function<void(const middleware::Message&)>;

enum class IpcBusMode : uint8_t {
    None   = 0,
    Server = 1,
    Hub    = 2,
};

class IpcMsgBus {
public:
    static IpcMsgBus& instance();

    bool init_server(middleware::ModuleId local_id, const char* socket_path);
    /** UI 多连接 hub（先 init_hub，再 add_peer） */
    bool init_hub(middleware::ModuleId local_id);
    bool add_peer(middleware::ModuleId peer_id, const char* socket_path,
                  bool required, bool connect_now = true);
    /** 兼容旧 API：等价 init_hub + add_peer(APP_GLAVO, …, required=true) */
    bool init_client(middleware::ModuleId local_id, const char* socket_path);
    void shutdown();

    uint32_t subscribe(middleware::MsgType type,
                       middleware::ModuleId module_id,
                       IpcHandler handler);
    void unsubscribe(uint32_t sub_id);

    bool publish_async(const middleware::Message& msg);
    bool publish_sync(const middleware::Message& msg);
    void process(int io_timeout_ms = 0);

    /** 所有 required peer 均已连接（hub）；server 模式下对端 UI 已连接 */
    [[nodiscard]] bool peer_connected() const noexcept;
    [[nodiscard]] bool peer_connected(middleware::ModuleId peer_id) const noexcept;

private:
    enum class PollKind : uint8_t {
        HubPeer,
        ServerListen,
        ServerConn,
    };

    struct PeerEntry {
        middleware::ModuleId peer_id{};
        std::string socket_path;
        bool required = false;
        IpcEndpoint endpoint;
        std::chrono::steady_clock::time_point last_reconnect_attempt{};
    };

    struct PollSlot {
        PollKind kind{};
        middleware::ModuleId hub_peer_id{};
        int fd = -1;
    };

    bool deliver_local(const middleware::Message& msg);
    bool enqueue_local(const middleware::Message& msg);
    void deliver_frames(const std::vector<std::vector<uint8_t>>& frames,
                        middleware::ModuleId expected_sender);
    void poll_io(int timeout_ms);
    void pump_inbound_fallback(int timeout_ms);
    void handle_poll_event(PollSlot* slot, uint32_t events);
    void try_reconnect_peers();
    void try_accept_peer();
    bool poll_register_hub_peer(PeerEntry& peer);
    bool poll_register_server_listen();
    bool poll_register_server_conn();
    void poll_unregister(int fd);
    void poll_clear();
    bool is_registered_peer(middleware::ModuleId id) const;
    PeerEntry* find_peer(middleware::ModuleId id);
    const PeerEntry* find_peer(middleware::ModuleId id) const;
    bool send_to_peer(middleware::ModuleId peer_id, const middleware::Message& msg);
    bool send_to_all_peers(const middleware::Message& msg);

    IpcBusMode mode_{IpcBusMode::None};
    IpcEndpoint server_endpoint_;
    IpcPoller poller_;
    std::unordered_map<int, std::unique_ptr<PollSlot>> poll_slots_;
    middleware::ModuleId local_id_{middleware::ModuleId::UI};
    middleware::ModuleId server_peer_id_{middleware::ModuleId::UI};

    std::vector<std::unique_ptr<PeerEntry>> peers_;
    mutable std::mutex peers_mtx_;

    struct LocalSub {
        uint32_t id;
        middleware::MsgType type;
        middleware::ModuleId module_id;
        IpcHandler handler;
    };
    std::vector<LocalSub> subscribers_;
    std::mutex subs_mtx_;
    std::atomic<uint32_t> next_sub_id_{1};

    middleware::PriorityMsgQueue local_queue_;
    std::mutex queue_mtx_;
    bool initialized_{false};
};

} // namespace upc_ipc

#endif
