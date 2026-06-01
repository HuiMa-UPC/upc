#ifndef TCP_SENDER_HPP
#define TCP_SENDER_HPP

#include "ring_buffer.hpp"
#include "device_config.hpp"
#include "protocol_mcu.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace services {

enum class TcpJobEvent : uint8_t {
    PrintCompleted = 0,
    TerminateDone,
    PrintError,
};

using TcpJobEventCb = std::function<void(TcpJobEvent)>;
using TcpProgressCb = std::function<void()>;

/**
 * @brief 振镜 TCP/串口链路线程：批量下发、心跳、换层、终止/完成清理
 */
class TcpSender {
public:
    static TcpSender& instance();

    void bind_ring_buffer(RingBuffer* rb);
    void set_job_event_callback(TcpJobEventCb cb);
    /** 每成功下发一批运动包后调用（链路线程） */
    void set_progress_callback(TcpProgressCb cb);

    /** 建立 TCP（单次尝试，失败返回负值） */
    int connect_tcp(const char* ip, int port);

    /** 打开串口（单次尝试） */
    int open_serial(const char* device, int baud);

    /** 启动链路线程（main 在连接成功后调用） */
    void start_link_thread();

    void stop();

    bool is_link_running() const;
    bool is_tcp_connected() const;
    bool is_serial_open() const;
    bool is_sending() const;

    /** 用户确认打印后开始下发（Ring 不足时挂起，满后自动开始） */
    void begin_sending();

    /** 手动终止或 TCP 断线触发的异步清理 */
    void request_terminate();

    static bool set_tcp_nodelay(int sockfd);

    /** 串口帧解析后的事件分发（供链路线程 RX 使用） */
    void notify_serial_event(serial_event_t evt);

private:
    TcpSender();
    ~TcpSender();
    TcpSender(const TcpSender&) = delete;
    TcpSender& operator=(const TcpSender&) = delete;

    void link_thread_fn();

    int tcp_send_raw(const uint8_t* data, size_t len);
    int tcp_recv_nonblock(uint8_t* buf, size_t buf_size);
    void tcp_drain_rx();

    int serial_send_raw(const uint8_t* data, size_t len);
    int serial_recv_nonblock(uint8_t* buf, size_t buf_size);
    void serial_drain_rx();

    void tcp_stage_clear();
    size_t tcp_stage_count();
    bool tcp_stage_peek_type(rb_pkt_type_t* ptype);
    bool tcp_stage_pop_front(rb_packet_t* pkt);
    void refill_tcp_stage_from_ring();

    void reset_motion_state(bool clear_start_pending);
    void begin_sending_internal();
    bool ring_meets_start_threshold() const;
    void try_complete_pending_start();

    void link_drain_motion_queue();
    void link_try_finish_sending();
    bool link_handle_layer_mark();
    void link_try_layer_serial_dispatch();
    int link_send_one_motion_batch();
    void link_process_serial_handshake();

    void send_tcp_reset();
    void send_serial_print_terminate();
    void send_serial_print_finished();
    void run_manual_terminate_cleanup();
    void run_natural_complete_cleanup();
    void emit_event(TcpJobEvent ev);

    bool mcu_rates_drained() const;
    bool motion_stream_active() const;
    int link_thread_sleep_ms() const;

    RingBuffer* rb_ = nullptr;
    TcpJobEventCb event_cb_;
    TcpProgressCb progress_cb_;

    char tcp_ip_[64] = GLAVO_TCP_DEFAULT_IP;
    int  tcp_port_   = GLAVO_TCP_DEFAULT_PORT;

    int tcp_fd_    = -1;
    int serial_fd_ = -1;

    std::thread link_th_;
    std::atomic<bool> link_run_{false};
    std::atomic<bool> link_stop_{false};

    std::atomic<bool> is_sending_{false};
    std::atomic<bool> start_pending_{false};
    std::atomic<bool> terminate_pending_{false};
    std::atomic<bool> natural_complete_pending_{false};

    std::atomic<bool> waiting_layer_{false};
    std::atomic<bool> layer_drain_pending_{false};
    std::atomic<int>  serial_handshake_phase_{0};
    std::atomic<bool> serial_evt_condition_ok_{false};
    std::atomic<bool> serial_evt_print_ready_{false};

    uint16_t active_layer_height_um_ = GCODE_DEFAULT_LAYER_UM;
    uint16_t first_layer_height_um_  = GCODE_DEFAULT_LAYER_UM;

    std::deque<rb_packet_t> tcp_stage_;
    mutable std::mutex tcp_stage_mtx_;
    std::mutex tcp_send_mtx_;

    uint8_t tcp_rx_accum_[1024]{};
    size_t  tcp_rx_accum_len_ = 0;
    uint8_t serial_rx_accum_[256]{};
    size_t  serial_rx_accum_len_ = 0;

    static constexpr int kMaxMotionBatch = TCP_BATCH_SIZE;
};

} // namespace services

#endif
