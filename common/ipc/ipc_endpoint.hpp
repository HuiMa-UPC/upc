#ifndef UPC_IPC_ENDPOINT_HPP
#define UPC_IPC_ENDPOINT_HPP

#include "ipc_wire.hpp"
#include <cstdint>
#include <mutex>
#include <vector>

namespace upc_ipc {

/** 单次 inbound：普通 wire 帧，或 wire+SCM_RIGHTS（fds 非空） */
struct InboundWireUnit {
    std::vector<uint8_t> frame;
    std::vector<int>     fds;
};

/** 单连接 Unix 域套接字；服务端 listen 与 accept 分离，便于 Glavo 先起设备链路 */
class IpcEndpoint {
public:
    IpcEndpoint() = default;
    ~IpcEndpoint();

    IpcEndpoint(const IpcEndpoint&) = delete;
    IpcEndpoint& operator=(const IpcEndpoint&) = delete;

    /** 服务端：bind + listen，不阻塞等待 UI */
    bool listen_on(const char* socket_path);
    /** 服务端：非阻塞 accept，单连接模式（conn_fd_ 存于本 endpoint） */
    bool try_accept();
    /**
     * 服务端：非阻塞 accept，返回新 client fd（listen 端点不占用 conn_fd_）。
     * 调用方负责 close；Broker 等多连接场景使用。
     */
    int accept_connection();
    /** 已连接 fd 绑定到本端点（Broker accept 等多连接场景） */
    bool attach_connected_fd(int fd);
    /** 客户端：连接已 listen 的服务端 */
    bool connect_to(const char* socket_path);

    void close();
    [[nodiscard]] bool connected() const noexcept { return conn_fd_ >= 0; }
    [[nodiscard]] bool listening() const noexcept { return listen_fd_ >= 0; }
    [[nodiscard]] int conn_fd() const;
    [[nodiscard]] int listen_fd() const;

    bool send_frame(const std::vector<uint8_t>& frame);
    /** wire 帧与 dma fd 同一 sendmsg（UPDF 魔数），仅走 SCM_RIGHTS */
    bool send_frame_with_fds(const std::vector<uint8_t>& frame,
                             const std::vector<int>& fds);
    void recv_pending(std::vector<InboundWireUnit>& out_units);

private:
    bool wait_writable_locked(int timeout_ms) const;
    void close_conn_locked();
    void clear_fd_rx_state_locked();
    bool send_stream_bytes_locked(const uint8_t* buf, size_t n);
    bool peek_fd_frame_magic() const;
    bool recv_fd_frame_unit(InboundWireUnit& out);
    void parse_stream_frames(std::vector<InboundWireUnit>& out_units);

    void set_conn_nonblocking();

    int listen_fd_ = -1;
    int conn_fd_   = -1;
    std::vector<uint8_t> rx_accum_;
    std::vector<uint8_t> fd_rx_accum_;
    std::vector<int> fd_rx_fds_;
    mutable std::mutex io_mtx_;
};

} // namespace upc_ipc

#endif
