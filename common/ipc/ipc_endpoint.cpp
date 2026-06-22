#include "ipc_endpoint.hpp"

#include "rs_log.hpp"

#include <cerrno>
#include <cstring>

#if defined(__linux__) || defined(__unix__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace upc_ipc {

IpcEndpoint::~IpcEndpoint() {
    close();
}

int IpcEndpoint::conn_fd() const {
    std::lock_guard<std::mutex> lock(io_mtx_);
    return conn_fd_;
}

int IpcEndpoint::listen_fd() const {
    std::lock_guard<std::mutex> lock(io_mtx_);
    return listen_fd_;
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

bool IpcEndpoint::send_frame_with_fds(const std::vector<uint8_t>& frame,
                                      const std::vector<int>& fds)
{
#if !defined(__linux__) && !defined(__unix__)
    (void)frame;
    (void)fds;
    return false;
#else
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (!connected() || frame.empty() || fds.empty() ||
        fds.size() > IPC_FD_FRAME_MAX_FDS ||
        frame.size() > WIRE_MAX_PAYLOAD + 256) {
        return false;
    }

    const uint32_t magic = IPC_FD_FRAME_MAGIC;
    const uint32_t len   = static_cast<uint32_t>(frame.size());

    struct iovec iov[3] {};
    iov[0].iov_base = const_cast<uint32_t*>(&magic);
    iov[0].iov_len  = sizeof(magic);
    iov[1].iov_base = const_cast<uint32_t*>(&len);
    iov[1].iov_len  = sizeof(len);
    iov[2].iov_base = const_cast<uint8_t*>(frame.data());
    iov[2].iov_len  = frame.size();

    const size_t cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::vector<uint8_t> cmsg_buf(CMSG_SPACE(sizeof(int) * fds.size()));

    struct msghdr msg {};
    msg.msg_iov        = iov;
    msg.msg_iovlen     = 3;
    msg.msg_control    = cmsg_buf.data();
    msg.msg_controllen = cmsg_buf.size();

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level     = SOL_SOCKET;
    cmsg->cmsg_type      = SCM_RIGHTS;
    cmsg->cmsg_len       = cmsg_len;
    int* fd_ptr          = reinterpret_cast<int*>(CMSG_DATA(cmsg));
    for (size_t i = 0; i < fds.size(); ++i) {
        fd_ptr[i] = fds[i];
    }

    const ssize_t w = ::sendmsg(conn_fd_, &msg, MSG_NOSIGNAL);
    return w >= 0 &&
           static_cast<size_t>(w) == sizeof(magic) + sizeof(len) + frame.size();
#endif
}

bool IpcEndpoint::peek_fd_frame_magic() const
{
#if !defined(__linux__) && !defined(__unix__)
    return false;
#else
    if (conn_fd_ < 0) {
        return false;
    }
    if (!rx_accum_.empty()) {
        if (rx_accum_.size() >= sizeof(uint32_t)) {
            uint32_t tag = 0;
            std::memcpy(&tag, rx_accum_.data(), sizeof(tag));
            if (tag == IPC_FD_FRAME_MAGIC) {
                UPC_LOG_IPC(Error,
                            "UPDF magic in rx_accum (mixed read/sendmsg)");
                return false;
            }
        }
        return false;
    }

    uint32_t magic = 0;
    const ssize_t n =
        ::recv(conn_fd_, &magic, sizeof(magic), MSG_PEEK | MSG_DONTWAIT);
    return n == static_cast<ssize_t>(sizeof(magic)) &&
           magic == IPC_FD_FRAME_MAGIC;
#endif
}

bool IpcEndpoint::recv_fd_frame_unit(InboundWireUnit& out)
{
#if !defined(__linux__) && !defined(__unix__)
    (void)out;
    return false;
#else
    if (conn_fd_ < 0) {
        return false;
    }

    uint32_t magic     = 0;
    uint32_t wire_len  = 0;
    std::vector<uint8_t> body(WIRE_MAX_PAYLOAD + 256);

    struct iovec iov[3] {};
    iov[0].iov_base = &magic;
    iov[0].iov_len  = sizeof(magic);
    iov[1].iov_base = &wire_len;
    iov[1].iov_len  = sizeof(wire_len);
    iov[2].iov_base = body.data();
    iov[2].iov_len  = body.size();

    std::vector<uint8_t> cmsg_buf(CMSG_SPACE(sizeof(int) * IPC_FD_FRAME_MAX_FDS));

    struct msghdr msg {};
    msg.msg_iov        = iov;
    msg.msg_iovlen     = 3;
    msg.msg_control    = cmsg_buf.data();
    msg.msg_controllen = cmsg_buf.size();

    const auto recv_once = [&]() -> ssize_t {
        return ::recvmsg(conn_fd_, &msg, MSG_DONTWAIT);
    };

    ssize_t n = recv_once();
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd {};
        pfd.fd     = conn_fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 100);
        if (pr > 0) {
            n = recv_once();
        }
    }

    if (n <= 0) {
        if (n == 0) {
            ::close(conn_fd_);
            conn_fd_ = -1;
            rx_accum_.clear();
        }
        return false;
    }

    if (magic != IPC_FD_FRAME_MAGIC || wire_len == 0 ||
        wire_len > WIRE_MAX_PAYLOAD + 256 ||
        static_cast<size_t>(n) < sizeof(magic) + sizeof(wire_len) + wire_len) {
        UPC_LOG_IPC(Error, "UPDF recv invalid magic=0x%x len=%u bytes=%zd",
                    magic, wire_len, n);
        rx_accum_.clear();
        return false;
    }

    out.frame.assign(body.data(), body.data() + wire_len);
    out.fds.clear();

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        UPC_LOG_IPC(Error, "UPDF recv missing SCM_RIGHTS");
        return false;
    }

    const size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
    const size_t fd_count = fd_bytes / sizeof(int);
    if (fd_count == 0 || fd_count > IPC_FD_FRAME_MAX_FDS) {
        UPC_LOG_IPC(Error, "UPDF recv bad fd count=%zu", fd_count);
        return false;
    }

    const int* fd_ptr = reinterpret_cast<int*>(CMSG_DATA(cmsg));
    out.fds.reserve(fd_count);
    for (size_t i = 0; i < fd_count; ++i) {
        out.fds.push_back(fd_ptr[i]);
    }
    return true;
#endif
}

void IpcEndpoint::parse_stream_frames(std::vector<InboundWireUnit>& out_units)
{
#if !defined(__linux__) && !defined(__unix__)
    (void)out_units;
    return;
#else
    for (;;) {
        if (rx_accum_.size() < sizeof(uint32_t)) {
            break;
        }
        uint32_t frame_len = 0;
        std::memcpy(&frame_len, rx_accum_.data(), sizeof(frame_len));
        if (frame_len == IPC_FD_FRAME_MAGIC) {
            UPC_LOG_IPC(Error, "UPDF magic in byte stream; clearing rx_accum");
            rx_accum_.clear();
            break;
        }
        if (frame_len == 0 || frame_len > WIRE_MAX_PAYLOAD + 256) {
            rx_accum_.clear();
            break;
        }
        const size_t total = sizeof(uint32_t) + frame_len;
        if (rx_accum_.size() < total) {
            break;
        }
        InboundWireUnit unit;
        unit.frame.resize(frame_len);
        std::memcpy(unit.frame.data(), rx_accum_.data() + sizeof(uint32_t),
                    frame_len);
        out_units.push_back(std::move(unit));
        rx_accum_.erase(rx_accum_.begin(),
                        rx_accum_.begin() +
                            static_cast<std::ptrdiff_t>(total));
    }
#endif
}

void IpcEndpoint::recv_pending(std::vector<InboundWireUnit>& out_units)
{
#if !defined(__linux__) && !defined(__unix__)
    (void)out_units;
    return;
#else
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (!connected()) {
        return;
    }

    for (;;) {
        if (!peek_fd_frame_magic()) {
            break;
        }
        InboundWireUnit unit;
        if (!recv_fd_frame_unit(unit)) {
            break;
        }
        out_units.push_back(std::move(unit));
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

    parse_stream_frames(out_units);
#endif
}

} // namespace upc_ipc
