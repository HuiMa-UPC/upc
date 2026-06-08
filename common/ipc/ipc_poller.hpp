#ifndef UPC_IPC_POLLER_HPP
#define UPC_IPC_POLLER_HPP

#include <cstdint>
#include <vector>

namespace upc_ipc {

struct PollEvent {
    uint32_t events = 0;
    void*    user_data = nullptr;
};

/** Linux epoll 封装；非 Linux 平台 wait() 恒返回 0 */
class IpcPoller {
public:
    IpcPoller();
    ~IpcPoller();

    IpcPoller(const IpcPoller&) = delete;
    IpcPoller& operator=(const IpcPoller&) = delete;

    [[nodiscard]] bool valid() const noexcept;

    bool add(int fd, uint32_t events, void* user_data);
    bool del(int fd);
    void close();
    /** shutdown 后再次 init 时重建 epoll fd */
    void reopen();

    /** @return 就绪事件数；出错返回 -1 */
    int wait(std::vector<PollEvent>& out, int timeout_ms);

private:
    int epfd_ = -1;
};

} // namespace upc_ipc

#endif
