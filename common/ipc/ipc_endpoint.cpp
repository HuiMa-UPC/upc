#include "ipc_endpoint.hpp"

#include <cerrno>
#include <cstring>

#if defined(__linux__) || defined(__unix__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace upc_ipc {

IpcEndpoint::~IpcEndpoint() {
    close();
}

void IpcEndpoint::set_conn_nonblocking() {
#if defined(__linux__) || defined(__unix__)
    if (conn_fd_ < 0) {
        return;
    }
    const int flags = ::fcntl(conn_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(conn_fd_, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

void IpcEndpoint::close() {
    std::lock_guard<std::mutex> lock(io_mtx_);
#if defined(__linux__) || defined(__unix__)
    if (conn_fd_ >= 0) {
        ::close(conn_fd_);
        conn_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
#endif
    rx_accum_.clear();
}

bool IpcEndpoint::listen_on(const char* socket_path) {
#if !defined(__linux__) && !defined(__unix__)
    (void)socket_path;
    return false;
#else
    close();
    if (!socket_path || socket_path[0] == '\0') {
        return false;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    ::unlink(socket_path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }
    if (::listen(fd, 1) < 0) {
        ::close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(io_mtx_);
        listen_fd_ = fd;
    }
    return true;
#endif
}

bool IpcEndpoint::try_accept() {
#if !defined(__linux__) && !defined(__unix__)
    return false;
#else
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (conn_fd_ >= 0 || listen_fd_ < 0) {
        return conn_fd_ >= 0;
    }

    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return false;
    }
    conn_fd_ = fd;
    set_conn_nonblocking();
    return true;
#endif
}

bool IpcEndpoint::connect_to(const char* socket_path) {
#if !defined(__linux__) && !defined(__unix__)
    (void)socket_path;
    return false;
#else
    close();
    if (!socket_path || socket_path[0] == '\0') {
        return false;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(io_mtx_);
        conn_fd_ = fd;
        set_conn_nonblocking();
    }
    return true;
#endif
}

bool IpcEndpoint::write_exact(const uint8_t* buf, size_t n) {
#if defined(__linux__) || defined(__unix__)
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(conn_fd_, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (w == 0) {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
#else
    (void)buf;
    (void)n;
    return false;
#endif
}

bool IpcEndpoint::send_frame(const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (!connected() || frame.empty()) {
        return false;
    }
    const uint32_t len = static_cast<uint32_t>(frame.size());
    if (!write_exact(reinterpret_cast<const uint8_t*>(&len), sizeof(len))) {
        return false;
    }
    return write_exact(frame.data(), frame.size());
}

void IpcEndpoint::recv_pending(std::vector<std::vector<uint8_t>>& out_frames) {
#if !defined(__linux__) && !defined(__unix__)
    (void)out_frames;
    return;
#else
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (!connected()) {
        return;
    }

    uint8_t chunk[4096];
    for (;;) {
        const ssize_t n = ::read(conn_fd_, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        if (n == 0) {
            ::close(conn_fd_);
            conn_fd_ = -1;
            rx_accum_.clear();
            return;
        }
        rx_accum_.insert(rx_accum_.end(), chunk, chunk + n);
    }

    for (;;) {
        if (rx_accum_.size() < sizeof(uint32_t)) {
            break;
        }
        uint32_t frame_len = 0;
        std::memcpy(&frame_len, rx_accum_.data(), sizeof(frame_len));
        if (frame_len == 0 || frame_len > WIRE_MAX_PAYLOAD + 256) {
            rx_accum_.clear();
            break;
        }
        const size_t total = sizeof(uint32_t) + frame_len;
        if (rx_accum_.size() < total) {
            break;
        }
        std::vector<uint8_t> frame(frame_len);
        std::memcpy(frame.data(), rx_accum_.data() + sizeof(uint32_t), frame_len);
        out_frames.push_back(std::move(frame));
        rx_accum_.erase(rx_accum_.begin(),
                        rx_accum_.begin() + static_cast<std::ptrdiff_t>(total));
    }
#endif
}

} // namespace upc_ipc
