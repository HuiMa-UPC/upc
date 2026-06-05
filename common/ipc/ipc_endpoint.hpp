#ifndef UPC_IPC_ENDPOINT_HPP
#define UPC_IPC_ENDPOINT_HPP

#include "ipc_wire.hpp"
#include <cstdint>
#include <mutex>
#include <vector>

namespace upc_ipc {

enum class IpcRole {
    Server,
    Client,
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
    /** 服务端：非阻塞 accept，成功则 connected() 为 true */
    bool try_accept();
    /** 客户端：连接已 listen 的服务端 */
    bool connect_to(const char* socket_path);

    void close();
    [[nodiscard]] bool connected() const noexcept { return conn_fd_ >= 0; }
    [[nodiscard]] bool listening() const noexcept { return listen_fd_ >= 0; }

    bool send_frame(const std::vector<uint8_t>& frame);
    void recv_pending(std::vector<std::vector<uint8_t>>& out_frames);

private:
    bool write_exact(const uint8_t* buf, size_t n);

    void set_conn_nonblocking();

    int listen_fd_ = -1;
    int conn_fd_   = -1;
    std::vector<uint8_t> rx_accum_;
    mutable std::mutex io_mtx_;
};

} // namespace upc_ipc

#endif
