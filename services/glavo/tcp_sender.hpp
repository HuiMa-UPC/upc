#ifndef TCP_SENDER_HPP
#define TCP_SENDER_HPP

#include "ring_buffer.hpp"
#include <atomic>
#include <thread>

namespace services {

/**
 * @brief TCP 指令下发器
 *
 * 职责：从 RingBuffer 中取出解析好的 G-code 指令包，通过 TCP 发送给下位机。
 *
 * 使用方式：
 *   1. TcpSender::instance().bind_ring_buffer(&GcodeWorker::instance().ring_buffer());
 *   2. TcpSender::instance().set_connected(true);   // TCP 连上后
 *   3. TcpSender::instance().start();                // 启动发送线程
 *   4. ... 使用完毕后 ...
 *   5. TcpSender::instance().stop();                 // 停止线程
 */
class TcpSender {
public:
    static TcpSender& instance();

    /** 绑定数据源 RingBuffer（必须在 start 之前调用） */
    void bind_ring_buffer(RingBuffer* rb);

    /** 启动发送线程 */
    void start();

    /** 停止发送线程并等待其结束 */
    void stop();

    /** 是否正在运行 */
    bool is_running() const;

    /** 设置 TCP 连接状态（由外部 TCP 连接模块调用） */
    void set_connected(bool connected);

    /** 获取 TCP 连接状态 */
    bool is_connected() const;

    /** 获取已发送包数 */
    int get_sent_count() const;

    /** 获取已发送字节数 */
    int get_sent_bytes() const;

    /** 复位统计计数器 */
    void reset_stats();

    /** 设置 socket 禁用 Nagle 算法（TCP_NODELAY）
     *  @return true 设置成功，false 失败 */
    static bool set_tcp_nodelay(int sockfd);

private:
    TcpSender();
    ~TcpSender();
    TcpSender(const TcpSender&) = delete;
    TcpSender& operator=(const TcpSender&) = delete;

    void sender_thread_fn();

    /** 实际发送一个包；用户需在此处接入自己的 TCP 发送逻辑
     *  @return true 发送成功，false 发送失败 */
    bool send_packet(const rb_packet_t& pkt);

    RingBuffer* rb_ = nullptr;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> sent_count_{0};
    std::atomic<int> sent_bytes_{0};
};

} // namespace services

#endif // TCP_SENDER_HPP
