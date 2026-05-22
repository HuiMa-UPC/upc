#include "tcpclient_backend.hpp"
#include "lvgl_wrap.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>

static int s_fd = -1;
static lv_timer_t *s_timer = nullptr;
static tcp_state_cb_t s_state_cb = nullptr;
static tcp_data_cb_t  s_data_cb  = nullptr;
static void *s_state_user = nullptr;
static void *s_data_user  = nullptr;

/* ---------- 内部定时器回调：轮询接收 ---------- */
static void read_timer_cb(lv_timer_t *)
{
    if(s_fd < 0) return;

    char buf[256];
    ssize_t n = recv(s_fd, buf, sizeof(buf) - 1, 0);
    if(n > 0) {
        buf[n] = '\0';
        if(s_data_cb) {
            s_data_cb(buf, (size_t)n, s_data_user);
        }
    }
    else if(n == 0) {
        close(s_fd);
        s_fd = -1;
        lv_timer_pause(s_timer);
        if(s_state_cb) {
            s_state_cb(TCP_STATE_REMOTE_CLOSED, "Remote closed", s_state_user);
        }
    }
    /* n < 0 时：非阻塞 socket 通常是 EAGAIN，原代码也是忽略，保持行为一致 */
}

/* ---------- 公共接口 ---------- */

void tcp_backend_init(void)
{
    if(!s_timer) {
        s_timer = lv_timer_create(read_timer_cb, 50, nullptr);
        lv_timer_pause(s_timer);
    }
}

void tcp_backend_deinit(void)
{
    if(s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    if(s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
    s_state_cb = nullptr;
    s_data_cb  = nullptr;
}

void tcp_backend_set_state_cb(tcp_state_cb_t cb, void *user_data)
{
    s_state_cb = cb;
    s_state_user = user_data;
}

void tcp_backend_set_data_cb(tcp_data_cb_t cb, void *user_data)
{
    s_data_cb = cb;
    s_data_user = user_data;
}

int tcp_backend_connect(const char *ip, int port)
{
    if(!ip || !ip[0] || port <= 0) {
        if(s_state_cb) s_state_cb(TCP_STATE_ERROR, "Empty IP/Port", s_state_user);
        return -4;
    }

    if(s_fd >= 0) {
        tcp_backend_disconnect();
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        if(s_state_cb) s_state_cb(TCP_STATE_ERROR, "Socket fail", s_state_user);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        if(s_state_cb) s_state_cb(TCP_STATE_ERROR, "Invalid IP", s_state_user);
        return -2;
    }

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        if(s_state_cb) s_state_cb(TCP_STATE_ERROR, "Connect failed", s_state_user);
        return -3;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    s_fd = fd;
    lv_timer_resume(s_timer);
    if(s_state_cb) s_state_cb(TCP_STATE_CONNECTED, "Connected", s_state_user);
    return 0;
}

void tcp_backend_disconnect(void)
{
    if(s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
        lv_timer_pause(s_timer);
        if(s_state_cb) s_state_cb(TCP_STATE_DISCONNECTED, "Disconnected", s_state_user);
    }
}

int tcp_backend_send(const char *data, size_t len)
{
    if(s_fd < 0 || !data || len == 0) return -1;
    ssize_t n = send(s_fd, data, len, 0);
    return (n == (ssize_t)len) ? 0 : -1;
}

int tcp_backend_is_connected(void)
{
    return s_fd >= 0;
}
