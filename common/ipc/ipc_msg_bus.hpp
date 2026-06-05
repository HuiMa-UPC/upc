#ifndef UPC_IPC_MSG_BUS_HPP
#define UPC_IPC_MSG_BUS_HPP

#include "ipc_endpoint.hpp"
#include "msg_queue.hpp"
#include "msg_types.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace upc_ipc {

using IpcHandler = std::function<void(const middleware::Message&)>;

class IpcMsgBus {
public:
    static IpcMsgBus& instance();

    bool init_server(middleware::ModuleId local_id, const char* socket_path);
    bool init_client(middleware::ModuleId local_id, const char* socket_path);
    void shutdown();

    uint32_t subscribe(middleware::MsgType type,
                       middleware::ModuleId module_id,
                       IpcHandler handler);
    void unsubscribe(uint32_t sub_id);

    bool publish_async(const middleware::Message& msg);
    bool publish_sync(const middleware::Message& msg);
    void process();
    void try_accept_peer();

    [[nodiscard]] bool peer_connected() const noexcept;

private:
    bool deliver_local(const middleware::Message& msg);
    bool enqueue_local(const middleware::Message& msg);
    void pump_inbound();
    bool is_remote(middleware::ModuleId id) const;
    bool send_to_peer(const middleware::Message& msg);

    IpcEndpoint endpoint_;
    middleware::ModuleId local_id_{middleware::ModuleId::UI};
    middleware::ModuleId peer_id_{middleware::ModuleId::APP_GLAVO};

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
    bool is_server_{false};
};

} // namespace upc_ipc

#endif
