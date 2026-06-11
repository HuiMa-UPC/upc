#ifndef TCP_SENDER_HPP
#define TCP_SENDER_HPP

#include "slice_ring_buffer.hpp"
#include "parser_config.hpp"
#include "device_config.hpp"
#include "protocol_mcu.hpp"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace upc::glavo {

/**
 * @brief TCP 发送器链路事件枚举
 *
 * 用于向上层（如 UI 或打印控制逻辑）报告打印任务的最终状态变化。
 */
enum class TcpJobEvent : uint8_t {
    PrintCompleted = 0, /**< 打印自然完成（RingBuffer 已空、解析器已结束且 MCU 缓存已排空） */
    TerminateDone,      /**< 用户手动终止或异常中断后的清理已完成 */
    PrintError,         /**< 打印过程中发生错误（如 TCP 断线）导致的终止 */
};

/** @brief 链路事件回调类型：接收到 TcpJobEvent 时通知上层 */
using TcpJobEventCb = std::function<void(TcpJobEvent)>;
/** @brief 进度回调类型：每成功下发一批运动包后触发，用于更新进度条等 UI */
using TcpProgressCb = std::function<void()>;

/** @brief 环境传感器缓存（串口 0x9B 上报解析结果，供 UI/打印进度读取） */
struct EnvSnapshotCache {
    float pm25_indoor     = 0.f;
    float pm25_cabin      = 0.f;
    float temperature_c   = 0.f;
    float oxygen_percent  = 0.f;
    float humidity_percent = 0.f;
    bool  valid           = false;
};

/**
 * @brief 振镜 TCP/串口链路线程管理类（单例）
 *
 * 职责：
 * - 维护与振镜控制板的 TCP 长连接（非阻塞）及心跳；
 * - 维护与 MCU 的串口通信（换层同步、打印状态握手）；
 * - 从 RingBuffer 批量取运动包，按协议组帧后下发；
 * - 处理换层标记（Layer Mark），通过串口与 MCU 进行层间握手；
 * - 支持用户主动终止（terminate）与自然完成两种结束路径。
 *
 * 线程模型：所有网络/串口 I/O 与协议处理集中在独立的 link_thread_fn 线程中，
 * 对外接口仅做状态设置与原子标志操作，避免跨线程直接操作 fd。
 */
class TcpSender {
public:
    /** @brief 获取 TcpSender 单例实例 */
    static TcpSender& instance();

    /** @brief 绑定 RingBuffer，作为运动包的数据来源 */
    void bind_ring_buffer(upc::parser::RingBuffer* rb);

    /** @brief 设置链路事件回调（如打印完成、终止完成） */
    void set_job_event_callback(TcpJobEventCb cb);

    /**
     * @brief 设置进度回调
     * @note 每成功下发一批运动包后，在链路线程中调用。
     */
    void set_progress_callback(TcpProgressCb cb);

    /**
     * @brief 建立 TCP 连接（阻塞式单次尝试）
     * @param ip   目标 IP 地址字符串，如 "192.168.1.100"
     * @param port 目标端口
     * @return 0 表示成功，负值表示失败（-1 参数错误/-2 IP 非法/-3 connect 失败）
     */
    int connect_tcp(const char* ip, int port);

    /**
     * @brief 打开串口设备（单次尝试）
     * @param device 串口设备路径，如 "/dev/ttyUSB0"
     * @param baud   波特率，当前支持 9600 / 115200（默认 115200）
     * @return 0 表示成功，负值表示失败
     */
    int open_serial(const char* device, int baud);

    /** @brief 启动链路线程（建议在 connect_tcp / open_serial 成功后调用） */
    void start_link_thread();

    /** @brief 停止链路线程，关闭 TCP 与串口 fd，重置所有状态 */
    void stop();

    /** @brief TCP 是否处于已连接状态（fd >= 0） */
    bool is_tcp_connected() const;
    /** @brief 当前是否处于“正在下发运动包”阶段 */
    bool is_sending() const;

    /**
     * @brief 用户确认开始打印后调用
     *
     * 若 RingBuffer 中可用包数不足，则设置 start_pending_ 标志，
     * 待 RingBuffer 累积到阈值后由链路线程自动启动。
     */
    void begin_sending();

    /** @brief 请求异步终止打印（用户手动停止或 TCP 断线触发） */
    void request_terminate();

    /**
     * @brief 将串口发送帧入队，由 link_thread 统一 write（线程安全）
     * @note 队列满时丢弃最旧一条
     */
    void enqueue_serial_tx(const uint8_t* data, size_t len);

    /** @brief 读取最新环境传感器快照（线程安全） */
    [[nodiscard]] EnvSnapshotCache env_snapshot() const;

    /**
     * @brief 设置 TCP_NODELAY，禁用 Nagle 算法以降低发包延迟
     * @param sockfd 已连接的 socket 文件描述符
     */
    static bool set_tcp_nodelay(int sockfd);

    /**
     * @brief 串口帧解析后的事件分发入口（供链路线程 RX 使用）
     * @param evt 由 serial_parse_frame 解析出的串口事件
     */
    void notify_serial_event(serial_event_t evt);

    void on_sensor_report(const serial_sensor_report_t& report);
    void publish_env_update_ntf();
    void drain_serial_tx_queue();

private:
    TcpSender();
    ~TcpSender();
    TcpSender(const TcpSender&) = delete;
    TcpSender& operator=(const TcpSender&) = delete;

    /** @brief 链路线程主函数：负责心跳、RX 排空、运动包下发、状态机推进 */
    void link_thread_fn();

    /** @brief TCP 原始发送（带互斥锁，遇 EPIPE/ECONNRESET 自动关闭 fd） */
    int tcp_send_raw(const uint8_t* data, size_t len);
    /** @brief TCP 非阻塞接收，返回读取到的字节数（0 表示暂无数据，-1 表示出错或断开） */
    int tcp_recv_nonblock(uint8_t* buf, size_t buf_size);
    /** @brief 排空 TCP 接收缓冲区，并做帧解析与处理 */
    void tcp_drain_rx();

    /** @brief 串口原始发送 */
    int serial_send_raw(const uint8_t* data, size_t len);
    /** @brief 串口非阻塞接收 */
    int serial_recv_nonblock(uint8_t* buf, size_t buf_size);
    /** @brief 排空串口接收缓冲区，并做帧解析与事件分发 */
    void serial_drain_rx();

    /** @brief 清空 tcp_stage_ 暂存队列 */
    void tcp_stage_clear();
    /** @brief 获取 tcp_stage_ 当前包数（线程安全） */
    size_t tcp_stage_count();
    /** @brief 查看队列头部包的类型，不移除（线程安全） */
    bool tcp_stage_peek_type(upc::parser::rb_pkt_type_t* ptype);
    /** @brief 从队列头部取出一个包（线程安全） */
    bool tcp_stage_pop_front(upc::parser::rb_packet_t* pkt);
    /** @brief 从 RingBuffer 批量预取包到 tcp_stage_，减少链路线程加锁粒度 */
    void refill_tcp_stage_from_ring();

    /**
     * @brief 重置运动/发送相关状态
     * @param clear_start_pending 是否同时清除 start_pending_ 标志
     */
    void reset_motion_state(bool clear_start_pending);
    /** @brief 内部真正开始发送：初始化层高、状态标志，并可选发起串口打印请求握手 */
    void begin_sending_internal();
    /** @brief 判断 RingBuffer 是否满足开始发送的阈值条件 */
    bool ring_meets_start_threshold() const;
    /** @brief 若存在挂起的 begin_sending 且阈值已满足，则正式启动 */
    void try_complete_pending_start();

    /** @brief 主链路逻辑：尝试从 tcp_stage_ 批量下发运动包 */
    void link_drain_motion_queue();
    /** @brief 尝试结束打印：检查 RingBuffer、解析器、MCU 缓存是否全部排空 */
    void link_try_finish_sending();
    /**
     * @brief 处理队列头部的换层标记（Layer Mark）
     * @return true 表示成功识别并进入换层等待状态
     */
    bool link_handle_layer_mark();
    /** @brief 若 MCU 缓存已排空，向串口发送“本层已完成”通知，启动下一层握手 */
    void link_try_layer_serial_dispatch();
    /** @brief 从 tcp_stage_ 取一批运动包，组帧后通过 TCP 下发 */
    int link_send_one_motion_batch();
    /** @brief 处理串口打印前的握手状态机（CONDITION_OK -> PRINT_READY） */
    void link_process_serial_handshake();

    /** @brief 向振镜发送 Reset 帧 */
    void send_tcp_reset();
    /** @brief 向 MCU 串口发送打印终止帧 */
    void send_serial_print_terminate();
    /** @brief 向 MCU 串口发送打印完成帧 */
    void send_serial_print_finished();
    /** @brief 执行手动终止后的清理：发 reset、清队列、回调 TerminateDone/PrintError */
    void run_manual_terminate_cleanup();
    /** @brief 执行自然完成后的清理：发 finished、回调 PrintCompleted */
    void run_natural_complete_cleanup();
    void emit_event(TcpJobEvent ev);
    void poll_mcu_drain_heartbeat();

    /** @brief 判断 MCU 的指令缓存与数据缓存是否均已排空 */
    bool mcu_rates_drained() const;
    /** @brief 当前是否处于“应持续发运动包”的活跃流状态 */
    bool motion_stream_active() const;
    /** @brief 根据当前状态决定链路线程每次循环的休眠间隔（ms） */
    int link_thread_sleep_ms() const;

    /* ---------- 核心依赖 ---------- */
    upc::parser::RingBuffer* rb_ = nullptr;          /**< 绑定的 RingBuffer，运动包来源 */
    TcpJobEventCb event_cb_;            /**< 链路事件回调 */
    TcpProgressCb progress_cb_;         /**< 进度更新回调 */

    /* ---------- 网络/串口配置 ---------- */
    char tcp_ip_[64] = GLAVO_TCP_DEFAULT_IP; /**< 当前连接或重连使用的 IP */
    int  tcp_port_   = GLAVO_TCP_DEFAULT_PORT; /**< 当前连接或重连使用的端口 */

    /* ---------- 文件描述符 ---------- */
    int tcp_fd_    = -1; /**< TCP socket 文件描述符，-1 表示未连接 */
    int serial_fd_ = -1; /**< 串口文件描述符，-1 表示未打开 */

    /* ---------- 链路线程控制 ---------- */
    std::thread link_th_;             /**< 链路线程句柄 */
    std::atomic<bool> link_run_{false};  /**< 线程是否应继续主循环 */
    std::atomic<bool> link_stop_{false}; /**< 外部请求停止标志 */

    /* ---------- 发送状态机 ---------- */
    std::atomic<bool> is_sending_{false};              /**< 是否正在下发运动包 */
    std::atomic<bool> start_pending_{false};           /**< begin_sending 已调用但 RingBuffer 未达阈值，等待中 */
    std::atomic<bool> terminate_pending_{false};       /**< 收到终止请求，待链路线程执行清理 */
    std::atomic<bool> natural_complete_pending_{false};/**< 运动包已发完，等待 MCU 缓存排空以触发自然完成 */
    std::chrono::steady_clock::time_point natural_complete_since_{};

    /* ---------- 换层/串口握手状态 ---------- */
    std::atomic<bool> waiting_layer_{false};           /**< 当前是否处于换层等待状态 */
    std::atomic<bool> layer_drain_pending_{false};     /**< 已遇到 LayerMark，等待 MCU 缓存排空后发送 layer_done */
    std::atomic<int>  serial_handshake_phase_{0};      /**< 串口握手阶段（0=无 1=等CONDITION_OK 2=等PRINT_READY） */
    std::atomic<bool> serial_evt_condition_ok_{false}; /**< 串口收到打印条件就绪事件 */
    std::atomic<bool> serial_evt_print_ready_{false};  /**< 串口收到打印准备就绪事件 */

    /* ---------- 层高信息 ---------- */
    uint16_t active_layer_height_um_ = GCODE_DEFAULT_LAYER_UM; /**< 当前层高度（微米） */
    uint16_t first_layer_height_um_  = GCODE_DEFAULT_LAYER_UM; /**< 首层高度（微米），用于串口首次握手 */
    /** 收到 PRINT_READY 时的 sent_packets，用于判定本层是否已下发扫描指令 */
    int sent_at_layer_ready_ = 0;

    /* ---------- 预取队列 ---------- */
    std::deque<upc::parser::rb_packet_t> tcp_stage_;      /**< 从 RingBuffer 预取出来的包暂存队列 */
    mutable std::mutex tcp_stage_mtx_;       /**< 保护 tcp_stage_ 的互斥锁 */
    std::mutex tcp_send_mtx_;                /**< 保护 TCP send 的互斥锁（避免多线程并发写 socket） */

    /* ---------- 接收累积缓冲区 ---------- */
    uint8_t tcp_rx_accum_[1024]{};   /**< TCP 接收分帧累积缓冲区 */
    size_t  tcp_rx_accum_len_ = 0;   /**< TCP 累积缓冲区当前有效长度 */
    uint8_t serial_rx_accum_[256]{}; /**< 串口接收分帧累积缓冲区 */
    size_t  serial_rx_accum_len_ = 0;/**< 串口累积缓冲区当前有效长度 */

    struct SerialTxItem {
        uint8_t data[64]{};
        size_t  len = 0;
    };
    std::deque<SerialTxItem> serial_tx_queue_;
    mutable std::mutex serial_tx_mtx_;

    EnvSnapshotCache env_cache_{};
    mutable std::mutex env_mtx_;

    /* ---------- 常量 ---------- */
    static constexpr size_t kSerialTxQueueMax = 32;
    static constexpr int kMaxMotionBatch = TCP_BATCH_SIZE; /**< 单次下发的最大运动包数量 */
};

} // namespace upc::glavo

#endif
