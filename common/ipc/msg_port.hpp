#ifndef UPC_MSG_PORT_HPP
#define UPC_MSG_PORT_HPP

/**
 * @file msg_port.hpp
 * @brief UI / Glavo 统一消息门面（多人协作边界）
 *
 * 发布/订阅 API：subscribe / publish_async / process
 * 消息契约与命名：common/contract/README.md
 *
 * UI 多后端：set_mode_ipc_hub + add_ipc_peer；单播设 target_id，广播用
 * publish_async_broadcast 或 target_id=ModuleId{0}。
 */
#include "msg_bus.hpp"
#include "ipc_config.hpp"

#include <vector>

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
    /** UI 侧多连接 hub（配合 add_ipc_peer） */
    bool set_mode_ipc_hub(middleware::ModuleId local_id = ModuleId::UI);
    bool add_ipc_peer(ModuleId peer_id, const char* socket_path, bool required,
                      bool connect_now = true);
    /** 兼容：等价 set_mode_ipc_hub + add_ipc_peer(APP_GLAVO, …) */
    bool set_mode_ipc_client(middleware::ModuleId local_id,
                             const char* socket_path = UPC_IPC_SOCKET_DEFAULT);

    bool init();
    void shutdown();

    Subscription subscribe(MsgType type, ModuleId module_id, MsgHandler handler);
    bool publish(const Message& msg);
    bool publish_async(const Message& msg);
    /** 广播到所有已连接的 backend peer（内部 target_id=0） */
    bool publish_async_broadcast(const Message& msg);
    void process(int io_timeout_ms = 0);

    [[nodiscard]] MsgPortMode mode() const noexcept { return mode_; }
    /** 所有 required peer 均已连接 */
    [[nodiscard]] bool peer_connected() const noexcept;
    [[nodiscard]] bool peer_connected(ModuleId peer_id) const noexcept;

    /** 经已有 IPC 连接附带 SCM_RIGHTS 发送（Camera Start_Rsp + dma fd） */
    bool publish_to_peer_with_fds(const Message& msg,
                                  const std::vector<int>& fds);
    /** UI Hub：取走 peer 随 wire 帧附带的 dma fd */
    bool take_peer_dma_fds(ModuleId peer_id, std::vector<int>& out_fds);

private:
    MsgPortMode mode_{MsgPortMode::InProcess};
};

} // namespace middleware

#endif
