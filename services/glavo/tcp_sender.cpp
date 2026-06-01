#include "tcp_sender.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace services {

/* ---------- 单例 ---------- */
TcpSender& TcpSender::instance() {
    static TcpSender inst;
    return inst;
}

TcpSender::TcpSender() = default;
TcpSender::~TcpSender() {
    stop();
}

/* ---------- 公共接口 ---------- */

void TcpSender::bind_ring_buffer(RingBuffer* rb) {
    rb_ = rb;
}

void TcpSender::start() {
    if (running_.load()) {
        return;  // 已经在运行
    }
    if (!rb_) {
#if SERVICES_DEBUG
        printf("[TcpSender] start failed: no RingBuffer bound\n");
#endif
        return;
    }

    stop();  // 确保旧线程已结束

    running_.store(true);
    th_ = std::thread(&TcpSender::sender_thread_fn, this);

#if SERVICES_DEBUG
    printf("[TcpSender] started\n");
#endif
}

void TcpSender::stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    if (th_.joinable()) {
        th_.join();
    }

#if SERVICES_DEBUG
    printf("[TcpSender] stopped (sent=%d)\n", sent_count_.load());
#endif
}

bool TcpSender::is_running() const {
    return running_.load();
}

void TcpSender::set_connected(bool connected) {
    connected_.store(connected);
#if SERVICES_DEBUG
    printf("[TcpSender] connection state: %s\n", connected ? "connected" : "disconnected");
#endif
}

bool TcpSender::is_connected() const {
    return connected_.load();
}

int TcpSender::get_sent_count() const {
    return sent_count_.load();
}

int TcpSender::get_sent_bytes() const {
    return sent_bytes_.load();
}

void TcpSender::reset_stats() {
    sent_count_.store(0);
    sent_bytes_.store(0);
}

/* ---------- 发送线程 ---------- */

void TcpSender::sender_thread_fn() {
    while (running_.load(std::memory_order_relaxed)) {
        // 未连接时休眠等待，不空转
        if (!connected_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        rb_packet_t pkt;
        // 阻塞等包，100ms 超时（便于响应 stop）
        if (!rb_->get(pkt, 100)) {
            continue;  // 超时：可能 RingBuffer 空了，继续等
        }

        if (send_packet(pkt)) {
            sent_count_.fetch_add(1);
            sent_bytes_.fetch_add(sizeof(rb_packet_t));
        } else {
            // 发送失败：可以选择重试、丢弃、或停线程
            // 当前策略：打印日志并继续
#if SERVICES_DEBUG
            printf("[TcpSender] send_packet failed, type=%d\n", pkt.type);
#endif
        }
    }
}

/* ---------- 网络辅助 ---------- */

bool TcpSender::set_tcp_nodelay(int sockfd)
{
    int flag = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
#if SERVICES_DEBUG
        printf("[TcpSender] setsockopt TCP_NODELAY failed: %s\n", strerror(errno));
#endif
        return false;
    }
    return true;
}

/* ---------- 实际发送（用户后续接入自己的 TCP 逻辑） ---------- */

bool TcpSender::send_packet(const rb_packet_t& pkt) {
    // TODO: 在此处接入实际的 TCP 发送逻辑
    //
    // 提醒：创建 socket 后务必调用 TcpSender::set_tcp_nodelay(fd)
    // 以禁用 Nagle 算法，确保 G-code 指令实时下发、不被缓存合并。
    //
    // 示例（用户自行替换）：
    // if (pkt.type == RB_PKT_MOTION) {
    //     return tcp_client_send(pkt.data, sizeof(pkt.data)) == sizeof(pkt.data);
    // } else if (pkt.type == RB_PKT_LAYER_MARK) {
    //     return tcp_client_send(&pkt.type, 1) == 1;
    // }
    // return false;

    (void)pkt;  // 抑制未使用警告

#if SERVICES_DEBUG
    // 占位：仅打印，不实际发送
    static int dbg_cnt = 0;
    if (++dbg_cnt <= 5 || dbg_cnt % 100 == 0) {
        printf("[TcpSender] placeholder send: type=%d (dbg #%d)\n",
               pkt.type, dbg_cnt);
    }
#endif
    return true;  // 占位返回 true，避免发送线程因失败而停滞
}

} // namespace services
