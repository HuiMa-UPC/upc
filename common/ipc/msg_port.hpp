#ifndef UPC_MSG_PORT_HPP
#define UPC_MSG_PORT_HPP

/**
 * @file msg_port.hpp
 * @brief UI / Glavo / Camera 统一消息门面（多人协作边界）
 *
 * 发布/订阅 API：subscribe / publish_async / process
 * 消息契约：common/upc_contract.hpp
 *
 * Broker 拓扑：业务进程 set_mode_ipc_client；Broker 进程直接使用 IpcMsgBus::init_broker。
 */
#include "msg_bus.hpp"
#include "upc_contract.hpp"

#include <vector>

namespace middleware {

class MsgPort {
public:
    static MsgPort& instance();

    bool set_mode_ipc_client(middleware::ModuleId local_id,
                             const char* broker_path = UPC_BROKER_SOCKET_DEFAULT);

    bool init();
    void shutdown();

    Subscription subscribe(MsgType type, ModuleId module_id, MsgHandler handler);
    bool publish(const Message& msg);
    bool publish_async(const Message& msg);
    void process(int io_timeout_ms = 0);

    [[nodiscard]] bool peer_connected() const noexcept;
    [[nodiscard]] bool peer_connected(ModuleId peer_id) const noexcept;

    bool publish_to_peer_with_fds(const Message& msg,
                                  const std::vector<int>& fds);
    bool take_peer_dma_fds(ModuleId peer_id, std::vector<int>& out_fds);

};

} // namespace middleware

#endif
