#include "ipc_poller.hpp"

#if defined(__linux__)
#include <sys/epoll.h>
#include <cerrno>
#include <unistd.h>
#endif

namespace upc_ipc {

IpcPoller::IpcPoller() {
#if defined(__linux__)
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
}

IpcPoller::~IpcPoller() {
    close();
}

bool IpcPoller::valid() const noexcept {
#if defined(__linux__)
    return epfd_ >= 0;
#else
    return false;
#endif
}

void IpcPoller::close() {
#if defined(__linux__)
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
#endif
}

void IpcPoller::reopen() {
    close();
#if defined(__linux__)
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
}

bool IpcPoller::add(int fd, uint32_t events, void* user_data) {
#if defined(__linux__)
    if (epfd_ < 0 || fd < 0) {
        return false;
    }
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = user_data;
    return ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
#else
    (void)fd;
    (void)events;
    (void)user_data;
    return false;
#endif
}

bool IpcPoller::del(int fd) {
#if defined(__linux__)
    if (epfd_ < 0 || fd < 0) {
        return false;
    }
    return ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#else
    (void)fd;
    return false;
#endif
}

int IpcPoller::wait(std::vector<PollEvent>& out, int timeout_ms) {
    out.clear();
#if defined(__linux__)
    if (epfd_ < 0) {
        return -1;
    }
    epoll_event evs[16];
    for (;;) {
        const int n = ::epoll_wait(epfd_, evs, 16, timeout_ms);
        if (n >= 0) {
            out.reserve(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                PollEvent pe{};
                pe.events    = evs[i].events;
                pe.user_data = evs[i].data.ptr;
                out.push_back(pe);
            }
            return n;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
#else
    (void)timeout_ms;
    return 0;
#endif
}

} // namespace upc_ipc
