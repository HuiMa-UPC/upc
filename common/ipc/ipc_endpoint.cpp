#include "ipc_endpoint.hpp"

#include "rs_log.hpp"
#include "upc_contract.hpp"

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

namespace {

void close_fd_list(std::vector<int>& fds)
{
#if defined(__linux__) || defined(__unix__)
    for(int fd : fds) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
#endif
    fds.clear();
}

} // namespace

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
    clear_fd_rx_state_locked();
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
    if (::listen(fd, UPC_IPC_LISTEN_BACKLOG) < 0) {
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
    const int fd = accept_connection();
    if (fd < 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (conn_fd_ >= 0) {
        ::close(fd);
        return true;
    }
    conn_fd_ = fd;
    set_conn_nonblocking();
    return true;
#endif
}

int IpcEndpoint::accept_connection() {
#if !defined(__linux__) && !defined(__unix__)
    return -1;
#else
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (listen_fd_ < 0) {
        return -1;
    }

    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
#endif
}

bool IpcEndpoint::attach_connected_fd(int fd) {
#if !defined(__linux__) && !defined(__unix__)
    (void)fd;
    return false;
#else
    if (fd < 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (conn_fd_ >= 0) {
        ::close(conn_fd_);
    }
    conn_fd_ = fd;
    rx_accum_.clear();
    clear_fd_rx_state_locked();
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

bool IpcEndpoint::wait_writable_locked(int timeout_ms) const
{
#if !defined(__linux__) && !defined(__unix__)
    (void)timeout_ms;
    return false;
#else
    if(conn_fd_ < 0) {
        return false;
    }
    pollfd pfd{};
    pfd.fd = conn_fd_;
    pfd.events = POLLOUT;
    for(;;) {
        const int pr = ::poll(&pfd, 1, timeout_ms);
        if(pr > 0) {
            if((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
            return (pfd.revents & POLLOUT) != 0;
        }
        if(pr == 0) {
            return false;
        }
        if(errno == EINTR) {
            continue;
        }
        return false;
    }
#endif
}

void IpcEndpoint::close_conn_locked()
{
#if defined(__linux__) || defined(__unix__)
    if(conn_fd_ >= 0) {
        ::close(conn_fd_);
        conn_fd_ = -1;
    }
#endif
    rx_accum_.clear();
    clear_fd_rx_state_locked();
}

void IpcEndpoint::clear_fd_rx_state_locked()
{
    fd_rx_accum_.clear();
    close_fd_list(fd_rx_fds_);
}

bool IpcEndpoint::send_stream_bytes_locked(const uint8_t* buf, size_t n)
{
#if !defined(__linux__) && !defined(__unix__)
    (void)buf;
    (void)n;
    return false;
#else
    if(conn_fd_ < 0 || !buf || n == 0) {
        return false;
    }
    size_t off = 0;
    while(off < n) {
        const ssize_t w = ::send(conn_fd_, buf + off, n - off, MSG_NOSIGNAL);
        if(w > 0) {
            off += static_cast<size_t>(w);
            continue;
        }
        if(w == 0) {
            close_conn_locked();
            return false;
        }
        if(errno == EINTR) {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            if(wait_writable_locked(100) && conn_fd_ >= 0) {
                continue;
            }
            close_conn_locked();
            return false;
        }
        close_conn_locked();
        return false;
    }
    return true;
#endif
}

bool IpcEndpoint::send_frame(const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(io_mtx_);
    if (!connected() || frame.empty()) {
        return false;
    }
    const uint32_t len = static_cast<uint32_t>(frame.size());
    if (!send_stream_bytes_locked(reinterpret_cast<const uint8_t*>(&len),
                                  sizeof(len))) {
        return false;
    }
    return send_stream_bytes_locked(frame.data(), frame.size());
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

    for(;;) {
        const ssize_t w = ::sendmsg(conn_fd_, &msg, MSG_NOSIGNAL);
        if(w > 0) {
            const size_t total =
                sizeof(magic) + sizeof(len) + frame.size();
            if(static_cast<size_t>(w) != total) {
                UPC_LOG_IPC(Error,
                            "UPDF partial send bytes=%zd total=%zu, close conn",
                            w, total);
                close_conn_locked();
                return false;
            }
            return true;
        }
        if(w == 0) {
            close_conn_locked();
            return false;
        }
        if(errno == EINTR) {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            if(wait_writable_locked(100) && conn_fd_ >= 0) {
                continue;
            }
            close_conn_locked();
            return false;
        }
        close_conn_locked();
        return false;
    }
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

bool IpcEndpoint::recv_stream_chunk_locked(uint8_t* buf, size_t cap, ssize_t& out_n)
{
    out_n = -1;
#if !defined(__linux__) && !defined(__unix__)
    (void)buf;
    (void)cap;
    return false;
#else
    if(conn_fd_ < 0 || buf == nullptr || cap == 0) {
        return false;
    }

    alignas(struct cmsghdr) uint8_t cmsg_buf[
        CMSG_SPACE(sizeof(int) * IPC_FD_FRAME_MAX_FDS)];

    for(;;) {
        struct iovec iov {};
        iov.iov_base = buf;
        iov.iov_len  = cap;

        struct msghdr msg {};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        /* MSG_CMSG_CLOEXEC：收到的 dma fd 不该被后续 fork/exec 的子进程继承。 */
        const ssize_t n =
            ::recvmsg(conn_fd_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            close_conn_locked();
            return false;
        }
        if(n == 0) {
            out_n = 0;
            close_conn_locked();
            return false;
        }
        out_n = n;

        for(struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg != nullptr;
            cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if(cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
                continue;
            }
            if(cmsg->cmsg_len < CMSG_LEN(0)) {
                continue;
            }
            const size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
            const size_t fd_count = fd_bytes / sizeof(int);
            const int* fd_ptr     = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            if(fd_count == 0) {
                continue;
            }
            if(fd_rx_fds_.size() + fd_count > IPC_FD_FRAME_MAX_FDS) {
                /* 收不下的必须就地 close：句柄已经过户到本进程，丢掉引用就是
                 * 泄漏，攒够就 EMFILE。 */
                UPC_LOG_IPC(Error, "UPDF fd overflow current=%zu add=%zu",
                            fd_rx_fds_.size(), fd_count);
                for(size_t i = 0; i < fd_count; ++i) {
                    if(fd_ptr[i] >= 0) {
                        ::close(fd_ptr[i]);
                    }
                }
                continue;
            }
            for(size_t i = 0; i < fd_count; ++i) {
                fd_rx_fds_.push_back(fd_ptr[i]);
            }
        }
        return true;
    }
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
    const size_t max_wire_len = WIRE_MAX_PAYLOAD + 256u;
    uint8_t chunk[4096];

    for(;;) {
        if(fd_rx_accum_.size() >= sizeof(uint32_t)) {
            uint32_t magic = 0;
            std::memcpy(&magic, fd_rx_accum_.data(), sizeof(magic));
            if(magic != IPC_FD_FRAME_MAGIC) {
                if(!fd_rx_fds_.empty()) {
                    UPC_LOG_IPC(Error, "UPDF non-magic with pending fds");
                    clear_fd_rx_state_locked();
                    return false;
                }
                rx_accum_.insert(rx_accum_.end(),
                                 fd_rx_accum_.begin(),
                                 fd_rx_accum_.end());
                fd_rx_accum_.clear();
                return false;
            }
        }

        if(fd_rx_accum_.size() >= sizeof(uint32_t) * 2u) {
            uint32_t magic = 0;
            uint32_t wire_len = 0;
            std::memcpy(&magic, fd_rx_accum_.data(), sizeof(magic));
            std::memcpy(&wire_len, fd_rx_accum_.data() + sizeof(magic),
                        sizeof(wire_len));

            if(magic != IPC_FD_FRAME_MAGIC || wire_len == 0 ||
               wire_len > max_wire_len) {
                UPC_LOG_IPC(Error, "UPDF invalid header magic=0x%x len=%u",
                            magic, wire_len);
                clear_fd_rx_state_locked();
                return false;
            }

            const size_t total_need = sizeof(uint32_t) * 2u + wire_len;
            if(fd_rx_accum_.size() >= total_need) {
                if(fd_rx_fds_.empty()) {
                    UPC_LOG_IPC(Error, "UPDF missing SCM_RIGHTS");
                    clear_fd_rx_state_locked();
                    return false;
                }
                out.frame.assign(fd_rx_accum_.data() + sizeof(uint32_t) * 2u,
                                 fd_rx_accum_.data() + total_need);
                out.fds = std::move(fd_rx_fds_);
                fd_rx_fds_.clear();
                fd_rx_accum_.erase(fd_rx_accum_.begin(),
                                   fd_rx_accum_.begin() +
                                       static_cast<std::ptrdiff_t>(total_need));
                return true;
            }
        }

        /* 收字节 + 收 SCM_RIGHTS 由 recv_stream_chunk_locked 统一负责，
         * fd 直接进 fd_rx_fds_。 */
        ssize_t n = 0;
        if(!recv_stream_chunk_locked(chunk, sizeof(chunk), n)) {
            return false;
        }

        fd_rx_accum_.insert(fd_rx_accum_.end(), chunk, chunk + n);
        if(fd_rx_accum_.size() > max_wire_len + sizeof(uint32_t) * 2u) {
            UPC_LOG_IPC(Error, "UPDF oversized stream bytes=%zu",
                        fd_rx_accum_.size());
            clear_fd_rx_state_locked();
            return false;
        }
    }
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
            UPC_LOG_IPC(Warn, "UPDF magic in byte stream; handoff to fd parser");
            fd_rx_accum_.insert(fd_rx_accum_.end(), rx_accum_.begin(), rx_accum_.end());
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
        const bool expect_fd = !fd_rx_accum_.empty() ||
                               !fd_rx_fds_.empty() ||
                               peek_fd_frame_magic();
        if (!expect_fd) {
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
        if(peek_fd_frame_magic()) {
            break;
        }
        /* 这里必须走 recvmsg（见 recv_stream_chunk_locked 注释）：rx_accum_ 里
         * 残留半帧时 peek_fd_frame_magic 一律返回 false，紧跟其后的 UPDF 帧就会
         * 从这条路进来；用 read 会把它的 fd 丢在内核里，只剩字节，后面必然报
         * "UPDF missing SCM_RIGHTS" 把整帧扔掉。 */
        ssize_t n = 0;
        if(!recv_stream_chunk_locked(chunk, sizeof(chunk), n)) {
            if(!connected()) {
                return; // 对端关闭或出错，close_conn_locked 已清干净
            }
            break;
        }
        rx_accum_.insert(rx_accum_.end(), chunk, chunk + n);
    }

    parse_stream_frames(out_units);

    // read() 期间若头部切到 UPDF，parse_stream_frames 会把字节移交给 fd_rx_accum_；
    // 本轮再尝试一次 fd 解析，避免延后到下一次 process()。
    for (;;) {
        const bool expect_fd = !fd_rx_accum_.empty() ||
                               !fd_rx_fds_.empty() ||
                               peek_fd_frame_magic();
        if (!expect_fd) {
            break;
        }
        InboundWireUnit unit;
        if (!recv_fd_frame_unit(unit)) {
            break;
        }
        out_units.push_back(std::move(unit));
    }
#endif
}

} // namespace upc_ipc
