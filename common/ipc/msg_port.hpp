#ifndef UPC_MSG_PORT_HPP
#define UPC_MSG_PORT_HPP

/**
 * @file msg_port.hpp
 * @brief UI / Glavo 统一消息门面（多人协作边界）
 *
 * 发布/订阅 API：subscribe / publish_async / process
 * 消息契约与命名：common/contract/README.md
 *
 * 业务代码（ui_msg_proxy、service_base、tcp_sender）只依赖本头文件，
 * 不直接区分进程内 MsgBus 或跨进程 IPC。启动时由 main 选择模式。
 * 当前 IPC 为单 peer（UI ↔ Glavo）；多后端见 contract §5。
 */
#include "msg_bus.hpp"
#include "ipc_config.hpp"

namespace middleware {

enum class MsgPortMode : uint8_t {
    InProcess,
    Ipc,
};

class MsgPort {
public:
    static MsgPort& instance();

    void set_mode_in_process();
    bool set_mode_ipc_server(middleware::ModuleId local_id,
                             const char* socket_path = UPC_IPC_SOCKET_DEFAULT);
    bool set_mode_ipc_client(middleware::ModuleId local_id,
                             const char* socket_path = UPC_IPC_SOCKET_DEFAULT);

    bool init();
    void shutdown();

    Subscription subscribe(MsgType type, ModuleId module_id, MsgHandler handler);
    bool publish(const Message& msg);
    bool publish_async(const Message& msg);
    void process();

    [[nodiscard]] MsgPortMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool peer_connected() const noexcept;

private:
    MsgPortMode mode_{MsgPortMode::InProcess};
};

} // namespace middleware

#endif
