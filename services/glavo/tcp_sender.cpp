#include "tcp_sender.hpp"
#include "protocol_mcu.hpp"
#include "print_job_state.hpp"
#include "gcode_worker.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <thread>

namespace services {

namespace {

#if TCP_DEBUG
/**
 * @brief 将字节数组格式化为十六进制字符串，用于调试打印
 * @param data  数据指针
 * @param len   数据长度
 * @param out   输出缓冲区
 * @param out_size 输出缓冲区大小
 */
void hex_dump(const uint8_t* data, size_t len, char* out, size_t out_size)
{
    size_t pos = 0;
    for(size_t i = 0; i < len && pos + 3 < out_size; i++) {
        pos += std::snprintf(out + pos, out_size - pos, "%02X ", data[i]);
    }
    if(pos > 0 && pos < out_size) {
        out[pos - 1] = '\0'; // 去掉末尾空格
    } else if(out_size > 0) {
        out[0] = '\0';
    }
}
#endif

/**
 * @brief 串口打印前握手阶段枚举
 *
 * 仅在 GLAVO_ENABLE_LAYER_CHANGE 开启时有效：
 * - NONE：无需握手或已完成
 * - WAIT_CONDITION_OK：已发打印请求，等待 MCU 返回打印条件就绪
 * - WAIT_PRINT_READY：已发首层/换层参数，等待 MCU 返回准备就绪
 */
enum SerialHandshakePhase : int {
    SERIAL_HS_NONE = 0,
    SERIAL_HS_WAIT_CONDITION_OK,
    SERIAL_HS_WAIT_PRINT_READY,
};

/** @brief 帧长度猜测函数类型：根据缓冲区内容猜测完整帧长度 */
using frame_guess_fn  = size_t (*)(const uint8_t*, size_t);
/** @brief 帧处理函数类型：对解析出的完整帧进行业务处理 */
using frame_handle_fn = void (*)(const uint8_t*, size_t);

/**
 * @brief TCP 帧长度猜测函数
 * @param buf   接收缓冲区指针
 * @param avail 缓冲区中可用字节数
 * @return 完整帧长度；0 表示数据不足，1 表示当前字节非法应跳过
 *
 * 协议格式：
 * - 心跳/状态帧：0xBE 0xEF 0x10 0x01 ...（固定 19 字节）
 * - 运动响应帧：0xBE 0xEF pkg_num ...（pkg_num ∈ [1,63]，总长度 3 + pkg_num*11）
 */
size_t tcp_guess_frame_len(const uint8_t* buf, size_t avail)
{
    if(avail < 4) {
        return 0; // 头信息不足，等待更多数据
    }
    // 检查同步字
    if(buf[0] != 0xBE || buf[1] != 0xEF) {
        return 1; // 非法字节，跳过
    }
    // 心跳/状态响应帧（固定 19 字节）
    if(buf[2] == 0x10 && buf[3] == 0x01 && avail >= 19) {
        return 19;
    }
    // 运动响应帧：pkg_num 表示包含多少个包的响应
    const uint8_t pkg_num = buf[2];
    if(pkg_num > 0 && pkg_num < 64) {
        const size_t need = 3u + (size_t)pkg_num * 11u;
        if(avail >= need) {
            return need;
        }
        return 0; // 数据不足，等待
    }
    return 1; // 非法 pkg_num，跳过同步字后重试
}

/**
 * @brief TCP 帧处理函数
 * @param frame 完整帧数据指针
 * @param len   帧长度
 *
 * 当前仅处理心跳/状态响应帧，解析出 MCU 的 cmd_rate 与 data_rate，
 * 并更新到 PrintJobState 中，供背压控制和完成检测使用。
 */
void tcp_handle_frame(const uint8_t* frame, size_t len)
{
    if(len >= 19 && frame[0] == 0xBE && frame[1] == 0xEF &&
       frame[2] == 0x10 && frame[3] == 0x01) {
        tcp_response_t resp{};
        if(parse_tcp_response(frame, len, &resp) == 0) {
            PrintJobState::instance().set_mcu_rates(resp.cmd_rate, resp.data_rate);
        }
    }
}

/**
 * @brief 串口帧长度猜测函数
 * @param buf   接收缓冲区指针
 * @param avail 可用字节数
 * @return 完整帧长度；0 表示不足，1 表示非法字节应跳过
 *
 * 串口协议：0x5A 0xA5 0xBB [3..5 保留] [6 payload_len] [payload] [checksum]
 * 总长度 = 8 + payload_len
 */
size_t serial_guess_frame_len(const uint8_t* buf, size_t avail)
{
    if(avail < 7) {
        return 0; // 头部长度不足
    }
    // 同步字检查
    if(buf[0] != 0x5A || buf[1] != 0xA5) {
        return 1; // 非法，跳过一字节
    }
    // 固定类型字节检查
    if(buf[2] != 0xBB) {
        return 1;
    }
    const uint8_t payload_len = buf[6];
    const size_t frame_len = 8u + (size_t)payload_len;
    return avail < frame_len ? 0 : frame_len;
}

/**
 * @brief 串口帧处理函数
 * @param frame 完整帧数据指针
 * @param len   帧长度
 *
 * 解析帧为串口事件，并通过 TcpSender 单例分发到 notify_serial_event，
 * 驱动串口握手状态机前进。
 */
void serial_handle_frame(const uint8_t* frame, size_t len)
{
    const serial_event_t evt = serial_parse_frame(frame, len);
#if TCP_DEBUG
    if(evt != SERIAL_EVT_NONE) {
        char hex[128];
        hex_dump(frame, len, hex, sizeof(hex));
        const char* evt_name = "UNKNOWN";
        switch(evt) {
            case SERIAL_EVT_PRINT_READY:        evt_name = "PRINT_READY";        break;
            case SERIAL_EVT_PRINT_CONDITION_OK: evt_name = "PRINT_CONDITION_OK"; break;
            default: break;
        }
        printf("[TcpSender][RX] serial event: %s, frame[%zu] = %s\n", evt_name, len, hex);
    }
#endif
    if(evt != SERIAL_EVT_NONE) {
        TcpSender::instance().notify_serial_event(evt);
    }
}

/**
 * @brief 通用分帧接收处理函数
 * @param recv_fn    非阻塞接收函数，返回读取到的字节数（<=0 表示无数据或断开）
 * @param chunk      单次 recv 使用的临时缓冲区
 * @param chunk_sz   chunk 大小
 * @param accum      累积缓冲区（用于处理粘包/拆包）
 * @param accum_cap  累积缓冲区总容量
 * @param accum_len  累积缓冲区当前有效长度（in/out）
 * @param guess      帧长度猜测函数
 * @param handle     帧处理函数
 *
 * 逻辑：
 * 1. 循环调用 recv_fn 读取数据到 chunk；
 * 2. 将 chunk 追加到 accum 尾部；
 * 3. 对 accum 做分帧解析：调用 guess 得到帧长度，调用 handle 处理完整帧；
 * 4. 若 accum 即将溢出，丢弃前一半旧数据以腾出空间。
 */
void drain_framed_rx(int (*recv_fn)(uint8_t*, size_t),
                     uint8_t* chunk, size_t chunk_sz,
                     uint8_t* accum, size_t accum_cap, size_t* accum_len,
                     frame_guess_fn guess,
                     frame_handle_fn handle)
{
    for(;;) {
        const int n = recv_fn(chunk, chunk_sz);
        if(n <= 0) {
            break; // 无数据或出错，结束本次排空
        }
        // 防溢出：若累积缓冲区快满了，丢弃前半部分最旧数据
        if(*accum_len + (size_t)n > accum_cap) {
            const size_t keep = accum_cap / 2;
            if(*accum_len > keep) {
                std::memmove(accum, accum + *accum_len - keep, keep);
                *accum_len = keep;
            }
        }
        // 追加新数据
        std::memcpy(accum + *accum_len, chunk, (size_t)n);
        *accum_len += (size_t)n;

        // 分帧解析
        size_t pos = 0;
        while(pos < *accum_len) {
            const size_t avail = *accum_len - pos;
            const size_t flen = guess(accum + pos, avail);
            if(flen == 0) {
                break; // 数据不足以构成完整帧，保留剩余数据等待下次接收
            }
            if(flen == 1) {
                pos++; // 非法字节，跳过
                continue;
            }
            handle(accum + pos, flen);
            pos += flen;
        }
        // 移除已处理的数据，保留未解析的部分
        if(pos > 0) {
            std::memmove(accum, accum + pos, *accum_len - pos);
            *accum_len -= pos;
        }
    }
}

} // namespace

/* ---------- 单例与生命周期 ---------- */

TcpSender& TcpSender::instance()
{
    static TcpSender inst;
    return inst;
}

TcpSender::TcpSender() = default;

TcpSender::~TcpSender()
{
    stop(); // 确保析构时线程已结束、fd 已关闭
}

/* ---------- 依赖注入与回调设置 ---------- */

void TcpSender::bind_ring_buffer(RingBuffer* rb)
{
    rb_ = rb;
}

void TcpSender::set_job_event_callback(TcpJobEventCb cb)
{
    event_cb_ = std::move(cb);
}

void TcpSender::set_progress_callback(TcpProgressCb cb)
{
    progress_cb_ = std::move(cb);
}

/* ---------- 静态工具 ---------- */

bool TcpSender::set_tcp_nodelay(int sockfd)
{
    int flag = 1;
    return setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

/* ---------- TCP 连接 ---------- */

int TcpSender::connect_tcp(const char* ip, int port)
{
    if(!ip || port <= 0) {
        return -1; // 参数非法
    }
    // 若已有连接，先关闭旧 fd
    if(tcp_fd_ >= 0) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        return -1; // 创建 socket 失败
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if(inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        return -2; // IP 地址格式错误
    }

    if(connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -3; // 连接失败
    }

    // 禁用 Nagle 算法，确保小帧及时发出
    if(!set_tcp_nodelay(fd)) {
#if SERVICES_DEBUG
        printf("[TcpSender] TCP_NODELAY set failed\n");
#endif
    }

    // 设为非阻塞模式，方便链路线程做轮询式收发
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // 保存地址信息，供断线后自动重连使用
    std::snprintf(tcp_ip_, sizeof(tcp_ip_), "%s", ip);
    tcp_port_ = port;

    tcp_fd_ = fd;
    PrintJobState::instance().set_tcp_connected(true);
    tcp_rx_accum_len_ = 0; // 清空接收累积区

#if SERVICES_DEBUG
    printf("[TcpSender] TCP connected %s:%d (NODELAY=1)\n", ip, port);
#endif
    return 0;
}

/* ---------- 串口打开与配置 ---------- */

int TcpSender::open_serial(const char* device, int baud)
{
    if(!device) {
        return -1;
    }
    // 关闭旧 fd
    if(serial_fd_ >= 0) {
        close(serial_fd_);
        serial_fd_ = -1;
    }

    const int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) {
        return -1; // 打开设备失败
    }

    struct termios tty{};
    if(tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1; // 获取串口属性失败
    }

    // 设置波特率
    speed_t spd = B115200;
    switch(baud) {
        case 9600:   spd = B9600; break;
        case 115200: spd = B115200; break;
        default:     spd = B115200; break;
    }
    cfsetospeed(&tty, spd);
    cfsetispeed(&tty, spd);

    // 8N1 无硬件流控，原始模式
    tty.c_cflag |= (CLOCAL | CREAD);           // 忽略调制解调器状态，启用接收
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE); // 无校验、1 位停止位
    tty.c_cflag |= CS8;                        // 8 位数据位
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // 原始输入（非规范模式）
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);    // 关闭软件流控
    tty.c_oflag &= ~OPOST;                     // 原始输出
    tty.c_cc[VMIN]  = 0;                       // 非阻塞读的最小字符数
    tty.c_cc[VTIME] = 0;                       // 非阻塞读的超时时间（deciseconds）

    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH); // 清空收发缓冲区

    serial_fd_ = fd;
    serial_rx_accum_len_ = 0;
    PrintJobState::instance().set_serial_open(true);

#if SERVICES_DEBUG
    printf("[TcpSender] serial open %s @ %d\n", device, baud);
#endif
    return 0;
}

/* ---------- 链路线程控制 ---------- */

void TcpSender::start_link_thread()
{
    if(link_run_.load()) {
        return; // 已在运行
    }
    link_stop_.store(false);
    link_run_.store(true);
    if(link_th_.joinable()) {
        link_th_.join(); // 等待旧线程结束（理论上不应发生）
    }
    link_th_ = std::thread(&TcpSender::link_thread_fn, this);
}

void TcpSender::stop()
{
    // 设置停止标志
    link_stop_.store(true);
    link_run_.store(false);
    if(link_th_.joinable()) {
        link_th_.join(); // 等待线程结束
    }
    // 关闭文件描述符
    if(tcp_fd_ >= 0) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }
    if(serial_fd_ >= 0) {
        close(serial_fd_);
        serial_fd_ = -1;
    }
    PrintJobState::instance().set_tcp_connected(false);
    PrintJobState::instance().set_serial_open(false);
}

/* ---------- 状态查询 ---------- */

bool TcpSender::is_link_running() const { return link_run_.load(); }
bool TcpSender::is_tcp_connected() const { return tcp_fd_ >= 0; }
bool TcpSender::is_serial_open() const { return serial_fd_ >= 0; }
bool TcpSender::is_sending() const { return is_sending_.load(); }

/* ---------- 事件与回调 ---------- */

void TcpSender::emit_event(TcpJobEvent ev)
{
    if(event_cb_) {
        event_cb_(ev);
    }
}

void TcpSender::notify_serial_event(serial_event_t evt)
{
#if TCP_DEBUG
    const char* evt_name = "UNKNOWN";
    switch(evt) {
        case SERIAL_EVT_PRINT_CONDITION_OK: evt_name = "PRINT_CONDITION_OK"; break;
        case SERIAL_EVT_PRINT_READY:        evt_name = "PRINT_READY";        break;
        default:                            evt_name = "NONE";               break;
    }
    printf("[TcpSender] notify_serial_event: %s\n", evt_name);
#endif
    if(evt == SERIAL_EVT_PRINT_CONDITION_OK) {
        serial_evt_condition_ok_.store(true);
    } else if(evt == SERIAL_EVT_PRINT_READY) {
        serial_evt_print_ready_.store(true);
    }
}

/* ---------- TCP 原始收发 ---------- */

int TcpSender::tcp_send_raw(const uint8_t* data, size_t len)
{
    if(tcp_fd_ < 0 || !data || len == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(tcp_send_mtx_);
    const ssize_t n = send(tcp_fd_, data, len, 0);
    if(n != (ssize_t)len) {
        // 对端断开或连接重置，关闭 fd 并更新状态
        if(n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            close(tcp_fd_);
            tcp_fd_ = -1;
            PrintJobState::instance().set_tcp_connected(false);
        }
        return -1;
    }
    return 0;
}

int TcpSender::tcp_recv_nonblock(uint8_t* buf, size_t buf_size)
{
    if(tcp_fd_ < 0 || !buf) {
        return -1;
    }
    const ssize_t n = recv(tcp_fd_, buf, buf_size, MSG_DONTWAIT);
    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // 暂无数据，属于正常情况
        }
        return -1; // 其他错误
    }
    if(n == 0) {
        // 对端优雅关闭
        close(tcp_fd_);
        tcp_fd_ = -1;
        PrintJobState::instance().set_tcp_connected(false);
        return -1;
    }
    return (int)n;
}

void TcpSender::tcp_drain_rx()
{
    uint8_t chunk[256];
    auto recv_fn = +[](uint8_t* b, size_t s) -> int {
        return TcpSender::instance().tcp_recv_nonblock(b, s);
    };
    drain_framed_rx(recv_fn, chunk, sizeof(chunk),
                    tcp_rx_accum_, sizeof(tcp_rx_accum_), &tcp_rx_accum_len_,
                    tcp_guess_frame_len,
                    +[](const uint8_t* f, size_t l) { tcp_handle_frame(f, l); });
}

/* ---------- 串口原始收发 ---------- */

int TcpSender::serial_send_raw(const uint8_t* data, size_t len)
{
    if(serial_fd_ < 0 || !data || len == 0) {
        return -1;
    }
#if TCP_DEBUG
    {
        char hex[256];
        hex_dump(data, len, hex, sizeof(hex));
        printf("[TcpSender][TX] serial raw[%zu] = %s\n", len, hex);
    }
#endif
    const ssize_t n = write(serial_fd_, data, len);
    return n == (ssize_t)len ? 0 : -1;
}

int TcpSender::serial_recv_nonblock(uint8_t* buf, size_t buf_size)
{
    if(serial_fd_ < 0 || !buf) {
        return -1;
    }
    const ssize_t n = read(serial_fd_, buf, buf_size);
    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // 暂无数据
        }
        return -1;
    }
    return (int)n;
}

void TcpSender::serial_drain_rx()
{
    uint8_t chunk[64];
    auto recv_fn = +[](uint8_t* b, size_t s) -> int {
        return TcpSender::instance().serial_recv_nonblock(b, s);
    };
    drain_framed_rx(recv_fn, chunk, sizeof(chunk),
                    serial_rx_accum_, sizeof(serial_rx_accum_), &serial_rx_accum_len_,
                    serial_guess_frame_len,
                    +[](const uint8_t* f, size_t l) { serial_handle_frame(f, l); });
}

/* ---------- tcp_stage_ 队列操作 ---------- */

void TcpSender::tcp_stage_clear()
{
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    tcp_stage_.clear();
}

size_t TcpSender::tcp_stage_count()
{
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    return tcp_stage_.size();
}

bool TcpSender::tcp_stage_peek_type(rb_pkt_type_t* ptype)
{
    if(!ptype) {
        return false;
    }
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    if(tcp_stage_.empty()) {
        return false;
    }
    *ptype = static_cast<rb_pkt_type_t>(tcp_stage_.front().type);
    return true;
}

bool TcpSender::tcp_stage_pop_front(rb_packet_t* pkt)
{
    if(!pkt) {
        return false;
    }
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    if(tcp_stage_.empty()) {
        return false;
    }
    *pkt = tcp_stage_.front();
    tcp_stage_.pop_front();
    return true;
}

/**
 * @brief 从 RingBuffer 预取包到 tcp_stage_
 *
 * 预取策略：
 * - 每次最多取 REFILL_STAGE_MAX_PACKETS 个包；
 * - 若 tcp_stage_ 已有 TCP_STAGE_MAX_PACKETS 个包则停止，避免内存无限增长；
 * - 对 RingBuffer 的 peek/get 操作限时 5ms，防止阻塞链路线程。
 */
void TcpSender::refill_tcp_stage_from_ring()
{
    if(!rb_) {
        return;
    }

    size_t moved = 0;
    while(moved < REFILL_STAGE_MAX_PACKETS) {
        {
            std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
            if(tcp_stage_.size() >= TCP_STAGE_MAX_PACKETS) {
                break; // 暂存队列已满
            }
        }

        rb_pkt_type_t ptype;
        if(!rb_->peek_type(ptype)) {
            break; // RingBuffer 已空
        }

        rb_packet_t pkt;
        if(!rb_->get(pkt, 5)) {
            break; // 获取失败（超时或无数据）
        }

        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        tcp_stage_.push_back(pkt);
        moved++;
    }
}

/* ---------- 状态辅助 ---------- */

bool TcpSender::mcu_rates_drained() const
{
    const auto snap = PrintJobState::instance().snapshot();
    return snap.cmd_cache_rate == 0 && snap.data_cache_rate == 0;
}

bool TcpSender::motion_stream_active() const
{
    return is_sending_.load() && !waiting_layer_.load() && !terminate_pending_.load();
}

/* ---------- 开始阈值判断 ---------- */

/**
 * @brief 判断 RingBuffer 是否满足开始发送的阈值条件
 * @return true 表示可以立即启动发送
 *
 * 判定逻辑：
 * 1. 若 RingBuffer 中已缓存 >= TCP_BATCH_SIZE 个包，直接满足；
 * 2. 若 GcodeWorker 仍在解析中：
 *    - 若总运动包数 >= batch 且当前有包，则满足；
 *    - 若处于 start_pending 挂起状态，且文件已读完、总包数 < batch 但 > 0，
 *      说明文件很小，也满足；
 * 3. 若 GcodeWorker 已结束解析但文件已打开：只要总包数 > 0 且有包，即满足。
 */
bool TcpSender::ring_meets_start_threshold() const
{
    if(!rb_) {
        return false;
    }
    const size_t count = rb_->count();
    const int batch = TCP_BATCH_SIZE;
    int total = GcodeWorker::instance().get_total_motion_packets();
    if(total <= 0) {
        total = PrintJobState::instance().snapshot().total_packets;
    }

    if(count >= static_cast<size_t>(batch)) {
        return true;
    }
    if(GcodeWorker::instance().is_parsing()) {
        if(total >= batch && count > 0) {
            return true;
        }
        if(start_pending_.load() && GcodeWorker::instance().is_file_read_exhausted() &&
           total > 0 && total < batch && count > 0) {
            return true; // 小文件特例
        }
        return false;
    }
    if(GcodeWorker::instance().is_file_open()) {
        return total > 0 && count > 0;
    }
    return false;
}

/* ---------- 运动状态管理 ---------- */

void TcpSender::reset_motion_state(bool clear_start_pending)
{
    if(clear_start_pending) {
        start_pending_.store(false);
    }
    is_sending_.store(false);
    waiting_layer_.store(false);
    layer_drain_pending_.store(false);
    natural_complete_pending_.store(false);
    serial_handshake_phase_.store(SERIAL_HS_NONE);
    serial_evt_condition_ok_.store(false);
    serial_evt_print_ready_.store(false);
    sent_at_layer_ready_ = 0;
    tcp_stage_clear();
    PrintJobState::instance().set_sending(false);
}

void TcpSender::begin_sending_internal()
{
    is_sending_.store(true);
    waiting_layer_.store(false);
    layer_drain_pending_.store(false);
    natural_complete_pending_.store(false);
    tcp_stage_clear();

    // 记录首层高度，用于串口首次握手
    first_layer_height_um_  = GcodeWorker::instance().get_layer_height_um();
    active_layer_height_um_ = first_layer_height_um_;

    // 重置 PrintJobState 中的发送相关计数与标志
    PrintJobState::instance().set_sending(true);
    PrintJobState::instance().set_job_completed(false);
    PrintJobState::instance().set_job_error(false);
    PrintJobState::instance().set_sent_packets(0);

#if GLAVO_ENABLE_LAYER_CHANGE
    // 若串口已打开，发送打印请求帧并进入握手等待
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_request(sbuf, sizeof(sbuf));
        if(slen > 0) {
#if TCP_DEBUG
            printf("[TcpSender] serial TX cmd: PRINT_REQUEST\n");
#endif
            serial_send_raw(sbuf, (size_t)slen);
        }
        waiting_layer_.store(true);
        serial_handshake_phase_.store(SERIAL_HS_WAIT_CONDITION_OK);
        serial_evt_condition_ok_.store(false);
        serial_evt_print_ready_.store(false);
    }
#endif

#if SERVICES_DEBUG
    printf("[TcpSender] begin_sending\n");
#endif
}

void TcpSender::begin_sending()
{
    if(tcp_fd_ < 0) {
        return; // TCP 未连接，无法开始
    }
    if(is_sending_.load()) {
        return; // 已在发送中
    }
    if(!ring_meets_start_threshold()) {
        // 缓存不足，设置挂起标志，由链路线程在条件满足时自动启动
        start_pending_.store(true);
        return;
    }
    start_pending_.store(false);
    begin_sending_internal();
}

void TcpSender::try_complete_pending_start()
{
    if(!start_pending_.load() || is_sending_.load() || tcp_fd_ < 0) {
        return;
    }
    if(!ring_meets_start_threshold()) {
        return; // 仍不满足阈值，继续等待
    }
    start_pending_.store(false);
    begin_sending_internal();
}

/* ---------- 终止请求 ---------- */

void TcpSender::request_terminate()
{
    terminate_pending_.store(true);
    is_sending_.store(false);
    start_pending_.store(false);
    PrintJobState::instance().set_sending(false);
}

/* ---------- 清理辅助：协议帧发送 ---------- */

void TcpSender::send_tcp_reset()
{
    uint8_t frame[64];
    const int flen = build_reset_frame(frame, sizeof(frame));
    if(flen > 0) {
        tcp_send_raw(frame, (size_t)flen);
    }
}

void TcpSender::send_serial_print_terminate()
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_terminate(sbuf, sizeof(sbuf));
        if(slen > 0) {
#if TCP_DEBUG
            printf("[TcpSender] serial TX cmd: PRINT_TERMINATE\n");
#endif
            serial_send_raw(sbuf, (size_t)slen);
        }
    }
#endif
}

void TcpSender::send_serial_print_finished()
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_finished(sbuf, sizeof(sbuf));
        if(slen > 0) {
#if TCP_DEBUG
            printf("[TcpSender] serial TX cmd: PRINT_FINISHED\n");
#endif
            serial_send_raw(sbuf, (size_t)slen);
        }
    }
#endif
}

/* ---------- 终止/完成清理流程 ---------- */

void TcpSender::run_manual_terminate_cleanup()
{
    // 1. 向振镜发 Reset，向 MCU 发终止通知
    send_tcp_reset();
    send_serial_print_terminate();

    // 2. 停止 Gcode 解析、清空 RingBuffer 与 stage 队列
    GcodeWorker::instance().stop_parse();
    if(rb_) {
        rb_->clear();
    }
    tcp_stage_clear();

    // 3. 判断是否是错误导致的终止，选择对应事件上报
    const bool was_error = PrintJobState::instance().snapshot().job_error;
    reset_motion_state(true);
    PrintJobState::instance().reset_job_counters();

    emit_event(was_error ? TcpJobEvent::PrintError : TcpJobEvent::TerminateDone);
}

void TcpSender::run_natural_complete_cleanup()
{
    // 1. 通知 MCU 打印完成，并向振镜发 Reset
    send_serial_print_finished();
    send_tcp_reset();

    // 2. 重置状态并上报完成事件
    reset_motion_state(true);
    PrintJobState::instance().set_job_completed(true);
    PrintJobState::instance().set_sending(false);

    emit_event(TcpJobEvent::PrintCompleted);
}

/* ---------- 串口握手与换层处理 ---------- */

void TcpSender::link_process_serial_handshake()
{
#if !GLAVO_ENABLE_LAYER_CHANGE
    return; // 换层功能未启用，无需串口握手
#endif
    if(!waiting_layer_.load() || serial_fd_ < 0) {
        return; // 不处于等待状态或串口未打开
    }

    const int phase = serial_handshake_phase_.load();

    // 阶段一：收到 PRINT_CONDITION_OK 后，发送首层/换层高度并进入阶段二
    if(phase == SERIAL_HS_WAIT_CONDITION_OK &&
       serial_evt_condition_ok_.exchange(false)) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_start(first_layer_height_um_, sbuf, sizeof(sbuf));
        if(slen > 0) {
#if TCP_DEBUG
            printf("[TcpSender] serial TX cmd: PRINT_START (layer=%u um)\n",
                   static_cast<unsigned>(first_layer_height_um_));
#endif
            serial_send_raw(sbuf, (size_t)slen);
        }
        serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
        serial_evt_print_ready_.store(false);
        return;
    }

    // 阶段二：收到 PRINT_READY 后，解除等待，开始下发本层运动包
    if(phase == SERIAL_HS_WAIT_PRINT_READY &&
       serial_evt_print_ready_.exchange(false)) {
        sent_at_layer_ready_ = PrintJobState::instance().snapshot().sent_packets;
        waiting_layer_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
    }
}

bool TcpSender::link_handle_layer_mark()
{
    rb_pkt_type_t ptype;
    if(!tcp_stage_peek_type(&ptype) || ptype != RB_PKT_LAYER_MARK) {
        return false; // 队列头部不是换层标记
    }

    rb_packet_t mark;
    if(!tcp_stage_pop_front(&mark)) {
        return false;
    }

#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ < 0) {
        return false; // 无串口时不进行换层握手
    }
    // 读取换层标记中的下层成高（协议 0x05 载荷）
    const uint16_t mark_um = rb_layer_mark_get_height_um(mark);
    if(mark_um > 0) {
        active_layer_height_um_ = mark_um;
    }
    // 本层尚未下发任何扫描指令时跳过层标记（如文件开头多余的 M3000）
    if(PrintJobState::instance().snapshot().sent_packets <= sent_at_layer_ready_) {
        return false;
    }
    // 进入换层等待状态，等待当前层运动包全部下发并 MCU 缓存排空
    waiting_layer_.store(true);
    layer_drain_pending_.store(true);
    serial_evt_condition_ok_.store(false);
    serial_evt_print_ready_.store(false);
    serial_handshake_phase_.store(SERIAL_HS_NONE);
    return true;
#else
    (void)mark;
    return false;
#endif
}

void TcpSender::link_try_layer_serial_dispatch()
{
#if !GLAVO_ENABLE_LAYER_CHANGE
    return;
#endif
    if(!waiting_layer_.load() || !layer_drain_pending_.load()) {
        return; // 不处于换层排空等待状态
    }
    if(serial_fd_ < 0) {
        // 无串口时直接跳过换层等待
        waiting_layer_.store(false);
        layer_drain_pending_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
        return;
    }
    // 必须等 MCU 缓存完全排空，确保当前层最后一包已被执行
    if(!mcu_rates_drained()) {
        return;
    }
    // 本层须已下发过扫描指令，才允许发单层扫描完成（协议：扫描 → 0x05）
    if(PrintJobState::instance().snapshot().sent_packets <= sent_at_layer_ready_) {
        return;
    }

    // 发送 layer_done 帧，通知 MCU 当前层已结束
    uint8_t sbuf[32];
    const int slen = serial_build_layer_done(active_layer_height_um_, sbuf, sizeof(sbuf));
    if(slen > 0) {
#if TCP_DEBUG
        printf("[TcpSender] serial TX cmd: LAYER_DONE (next_layer=%u um)\n",
               static_cast<unsigned>(active_layer_height_um_));
#endif
        serial_send_raw(sbuf, (size_t)slen);
    }
    layer_drain_pending_.store(false);
    // 转入等待 PRINT_READY 状态
    serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
    serial_evt_print_ready_.store(false);
}

/* ---------- 运动包批量下发 ---------- */

int TcpSender::link_send_one_motion_batch()
{
    if(tcp_stage_count() == 0) {
        return 0; // 暂存队列为空
    }

    rb_packet_t staged[kMaxMotionBatch];
    int n = 0;

    // 从 tcp_stage_ 头部连续取出运动包，遇到 LayerMark 或非运动包则停止
    for(int i = 0; i < kMaxMotionBatch; i++) {
        rb_pkt_type_t ptype;
        if(!tcp_stage_peek_type(&ptype)) {
            break; // 队列为空
        }
        if(ptype == RB_PKT_LAYER_MARK) {
            break; // 遇到换层标记，留待外层处理
        }
        if(ptype != RB_PKT_MOTION) {
            // 遇到未知类型包，丢弃并终止本轮批量
            rb_packet_t discard;
            tcp_stage_pop_front(&discard);
            break;
        }
        rb_packet_t rp;
        if(!tcp_stage_pop_front(&rp)) {
            break;
        }
        staged[n++] = rp;
    }

    if(n <= 0) {
        return 0;
    }

    // 将 n 个运动包的数据拼接到连续 payload 中（每包 11 字节）
    uint8_t payload[kMaxMotionBatch * 11];
    for(int i = 0; i < n; i++) {
        std::memcpy(&payload[i * 11], staged[i].data, 11);
    }

    // 构建完整的 TCP 运动帧并发送
    uint8_t frame[3 + kMaxMotionBatch * 11];
    const int flen = build_motion_frame(payload, n, frame, sizeof(frame));
    if(flen <= 0 || tcp_send_raw(frame, (size_t)flen) != 0) {
        // 发送失败：将取出的包回退到 tcp_stage_ 头部，等待下次重试
        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        for(int i = n - 1; i >= 0; i--) {
            tcp_stage_.push_front(staged[i]);
        }
        return 0;
    }

    // 更新已发送包计数
    const int sent = PrintJobState::instance().snapshot().sent_packets + n;
    PrintJobState::instance().set_sent_packets(sent);

#if TCP_DEBUG
    printf("[TcpSender] motion batch sent: %d packets (total sent=%d)\n", n, sent);
#endif

    // 触发进度回调
    if(progress_cb_) {
        progress_cb_();
    }
    return n;
}

/* ---------- 链路主逻辑：运动队列排空与完成检测 ---------- */

void TcpSender::link_drain_motion_queue()
{
    if(!is_sending_.load()) {
        return; // 未开始发送
    }

    // 1. 从 RingBuffer 预取包到 tcp_stage_
    refill_tcp_stage_from_ring();
    // 2. 本层指令已发完且 MCU 缓存排空时，尝试下发单层扫描完成（0x05）
    link_try_layer_serial_dispatch();

    // 3. 换层等待 / 首层握手等待：仅暂停 TCP 运动下发，不阻塞 0x05 排空检测
    if(waiting_layer_.load()) {
        return;
    }

    // 4. 背压检查：若 MCU 指令缓存占用率超过阈值，暂停下发
    const auto snap = PrintJobState::instance().snapshot();
    const uint32_t threshold =
        (65535u * (uint32_t)MCU_CMD_BACKPRESSURE_PCT) / 100u;
    if(snap.cmd_cache_rate >= threshold) {
        return;
    }

    // 5. 处理换层标记：若头部是 LayerMark，进入换层流程
    if(link_handle_layer_mark()) {
        link_try_layer_serial_dispatch();
        return;
    }

    // 6. 若暂存队列为空，本次无可下发内容
    if(tcp_stage_count() == 0) {
        return;
    }

    // 7. 批量发送运动包
    link_send_one_motion_batch();
}

void TcpSender::link_try_finish_sending()
{
    if(!is_sending_.load() || waiting_layer_.load()) {
        return; // 未发送中，或正在换层等待
    }
    if(tcp_stage_count() > 0) {
        return; // 暂存队列仍有数据待下发
    }
    if(rb_ && !rb_->empty()) {
        return; // RingBuffer 仍有数据
    }
    if(GcodeWorker::instance().is_parsing()) {
        return; // GcodeWorker 仍在解析，可能还有新数据产生
    }
    if(!GcodeWorker::instance().is_file_read_exhausted()) {
        return; // 文件尚未读完
    }
    // 所有运动包已下发，但需等 MCU 缓存排空才算真正完成
    if(!mcu_rates_drained()) {
        natural_complete_pending_.store(true);
        return;
    }

    // 真正自然完成：执行清理并上报事件
    natural_complete_pending_.store(false);
    is_sending_.store(false);
    run_natural_complete_cleanup();
}

/* ---------- 链路线程休眠策略 ---------- */

int TcpSender::link_thread_sleep_ms() const
{
    if(terminate_pending_.load()) {
        return 5; // 终止请求需快速响应
    }
    if(is_sending_.load() && !waiting_layer_.load()) {
        return TCP_SEND_INTERVAL_MS; // 活跃下发期间按固定间隔轮询
    }
    if(waiting_layer_.load() || natural_complete_pending_.load()) {
        return 10; // 换层等待或完成等待，稍快轮询
    }
    if(tcp_fd_ >= 0) {
        return TCP_HEARTBEAT_INTERVAL_MS; // 空闲但有连接，按心跳间隔休眠
    }
    return 50; // 无连接时的默认轮询间隔
}

/* ---------- 链路线程主循环 ---------- */

void TcpSender::link_thread_fn()
{
    using clock = std::chrono::steady_clock;
    auto next_hb = clock::now(); // 下次心跳发送时间点

    while(link_run_.load() && !link_stop_.load()) {
        const auto now = clock::now();

        // ---------- TCP RX 排空与断线检测 ----------
        if(tcp_fd_ >= 0) {
            tcp_drain_rx();
        } else if(is_sending_.load()) {
            // TCP 在发送过程中断开：标记错误并触发终止流程
            PrintJobState::instance().set_job_error(true);
            is_sending_.store(false);
            terminate_pending_.store(true);
        } else if(!terminate_pending_.load()) {
            // 空闲断线：尝试自动重连
            connect_tcp(tcp_ip_, tcp_port_);
            if(tcp_fd_ < 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
                continue; // 重连失败，跳过本轮其余逻辑
            }
        }

        // ---------- 终止处理 ----------
        if(terminate_pending_.load()) {
            run_manual_terminate_cleanup();
            terminate_pending_.store(false);
        } else {
            // ---------- 心跳 ----------
            const bool stream = motion_stream_active();
            if(!stream && tcp_fd_ >= 0 && now >= next_hb) {
                uint8_t hb[64];
                const int hblen = build_heartbeat_frame(hb, sizeof(hb));
                if(hblen > 0) {
                    tcp_send_raw(hb, (size_t)hblen);
                }
                next_hb = now + std::chrono::milliseconds(TCP_HEARTBEAT_INTERVAL_MS);
                tcp_drain_rx(); // 心跳发送后再排空一次 RX
            }

            // ---------- 串口 RX 与握手 ----------
            serial_drain_rx();
            link_process_serial_handshake();

            // ---------- 开始发送（处理挂起的 begin_sending） ----------
            try_complete_pending_start();

            // ---------- 运动包下发 ----------
            link_drain_motion_queue();

            // ---------- 完成检测 ----------
            link_try_finish_sending();
        }

        // ---------- 休眠等待下一轮 ----------
        std::this_thread::sleep_for(
            std::chrono::milliseconds(link_thread_sleep_ms()));
    }
}

} // namespace services
