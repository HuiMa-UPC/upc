#include "mcu_link.hpp"
#include "protocol.hpp"
#include "print_job_state.hpp"
#include "print_video_notify.hpp"
#include "parser_manager.hpp"
#include "parser_worker.hpp"
#include "msg_bus.hpp"
#include "msg_types.hpp"
#include "rs_log.hpp"
#include "msg_port.hpp"

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

namespace upc::glavo {

namespace parser = upc::parser;

namespace {

parser::IParserWorker* active_parser()
{
    return parser::ParserManager::instance().active();
}

#if UPC_DEBUG && UPC_MOD_MCU_LINK

/** 串口帧十六进制转储（5A A5 AA/BB …）；单条最多展示 64 字节 */
void serial_log_frame_hex(const char* dir, const uint8_t* data, size_t len)
{
    if(!dir || !data || len == 0) {
        return;
    }
    constexpr size_t kMaxShow = 64;
    const size_t show = len > kMaxShow ? kMaxShow : len;
    char hex[320];
    size_t pos = 0;
    for(size_t i = 0; i < show && pos + 4 < sizeof(hex); i++) {
        pos += static_cast<size_t>(std::snprintf(
            hex + pos, sizeof(hex) - pos, "%02X%s",
            data[i], (i + 1 < show) ? " " : ""));
    }
    if(len > show && pos + 16 < sizeof(hex)) {
        std::snprintf(hex + pos, sizeof(hex) - pos, " ... +%zuB", len - show);
    }
    UPC_LOG_MCU_LINK(Debug, "SERIAL_%s frame[%zu] %s", dir, len, hex);
}

void serial_log_rx_frame(const uint8_t* frame, size_t len)
{
    serial_log_frame_hex("RX", frame, len);

    if(serial_is_mcu_ack(frame, len)) {
        UPC_LOG_MCU_LINK(Debug, "RX MCU_ACK port=%02X cmd=%02X type=%02X",
            len >= 4 ? frame[3] : 0,
            len >= 5 ? frame[4] : 0,
            len >= 8 ? frame[7] : 0);
        return;
    }

    serial_sensor_report_t sensor{};
    serial_sensor_err_t serr = SERIAL_SENSOR_ERR_NONE;
    if(serial_parse_sensor_report_detail(frame, len, &sensor, &serr) == 0) {
        UPC_LOG_MCU_LINK(Debug,
            "RX SENSOR OK O2=%.1f%% press=%d mbar temp=%.1fC hum=%.1f%% "
            "PM2.5=%.0f powder=%u seq=%u",
            sensor.oxygen_percent,
            static_cast<int>(sensor.pressure_mbar),
            sensor.temperature_c,
            sensor.humidity_percent,
            sensor.pm25_ug_m3,
            static_cast<unsigned>(sensor.powder_present),
            len >= 6 ? static_cast<unsigned>(frame[5]) : 0u);
        return;
    }

    /* 运动轴位置上报（0xA5 0x9B + TLV 0x05）：与传感器帧同 Port/Cmd，需在
     * serial_frame_is_sensor_report 之前判定，否则会被误记为 RX SENSOR FAIL。 */
    serial_motion_pos_t mpos{};
    if(serial_parse_motion_pos_report(frame, len, &mpos) == 0) {
        UPC_LOG_MCU_LINK(Debug,
            "RX MOTION OK Z=%.3fmm Zwork=%.3fmm S=%.3fmm D=%.3fdeg",
            static_cast<double>(mpos.z_pos_mm), static_cast<double>(mpos.z_work_zero_mm),
            static_cast<double>(mpos.s_pos_mm), static_cast<double>(mpos.d_pos_deg));
        return;
    }

    if(serial_frame_is_sensor_report(frame, len)) {
        UPC_LOG_MCU_LINK(Warn, "RX SENSOR FAIL len=%zu err=%s",
            len, serial_sensor_err_string(serr));
        return;
    }

    const serial_event_t evt = serial_parse_frame(frame, len);
    if(evt != SERIAL_EVT_NONE) {
        const char* evt_name = "UNKNOWN";
        switch(evt) {
            case SERIAL_EVT_PRINT_PREPARING:    evt_name = "PRINT_PREPARING";    break;
            case SERIAL_EVT_PRINT_READY:        evt_name = "PRINT_READY";        break;
            case SERIAL_EVT_PRINT_CONDITION_OK: evt_name = "PRINT_CONDITION_OK"; break;
            default: break;
        }
        UPC_LOG_MCU_LINK(Info, "RX LAYER event=%s len=%zu", evt_name, len);
        return;
    }

    if(len >= 3 && frame[0] == 0x5A && frame[1] == 0xA5 && frame[2] == 0xBB) {
        UPC_LOG_MCU_LINK(Warn,
            "RX SERIAL UNKNOWN len=%zu port=%02X cmd=%02X seq=%u len_field=%u",
            len,
            len >= 4 ? frame[3] : 0,
            len >= 5 ? frame[4] : 0,
            len >= 6 ? static_cast<unsigned>(frame[5]) : 0u,
            len >= 7 ? static_cast<unsigned>(frame[6]) : 0u);
        return;
    }

    UPC_LOG_MCU_LINK(Warn, "RX SERIAL BAD len=%zu", len);
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
 * @brief TCP 帧处理函数（MCU→SOC，Length+ID 格式）
 */
void tcp_handle_frame(const uint8_t* frame, size_t len)
{
#if UPC_DEBUG && UPC_MOD_MCU_LINK
    /* 下位机 TCP 初始化完成主动上报：BE EF 01 0F（见 doc/SOC-_MCU TCP报文.xlsx） */
    if(len == 4 && frame[0] == TCP_SOF0 && frame[1] == TCP_SOF1 &&
       frame[2] == 0x01 && frame[3] == 0x0F) {
        UPC_LOG_MCU_LINK(Info, "RX TCP link ready (BE EF 01 0F)");
        return;
    }
#endif

    if(len >= 19 && frame[0] == 0xBE && frame[1] == 0xEF &&
       frame[2] == 0x10 && frame[3] == 0x01) {
        tcp_response_t resp{};
        if(parse_tcp_response(frame, len, &resp) == 0) {
            McuLink::instance().on_tcp_status_response(resp);
        }
    }
}

/**
 * @brief 串口帧处理函数
 * @param frame 完整帧数据指针
 * @param len   帧长度
 *
 * 解析帧为串口事件，并通过 McuLink 单例分发到 notify_serial_event，
 * 驱动串口握手状态机前进。
 */
void serial_handle_frame(const uint8_t* frame, size_t len)
{
    McuLink::instance().handle_serial_rx_frame(frame, len);
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
        // 防溢出：追加前必须腾出空间（旧逻辑在 accum_len 较小时会越界写）
        if((size_t)n > accum_cap) {
            std::memcpy(accum, chunk + n - accum_cap, accum_cap);
            *accum_len = accum_cap;
        } else {
            while(*accum_len + (size_t)n > accum_cap) {
                const size_t drop = *accum_len + (size_t)n - accum_cap;
                if(drop >= *accum_len) {
                    *accum_len = 0;
                    break;
                }
                std::memmove(accum, accum + drop, *accum_len - drop);
                *accum_len -= drop;
            }
            std::memcpy(accum + *accum_len, chunk, (size_t)n);
            *accum_len += (size_t)n;
        }

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

McuLink& McuLink::instance()
{
    static McuLink inst;
    return inst;
}

McuLink::McuLink() = default;

McuLink::~McuLink()
{
    stop(); // 确保析构时线程已结束、fd 已关闭
}

/* ---------- 依赖注入与回调设置 ---------- */

void McuLink::bind_ring_buffer(parser::RingBuffer* rb)
{
    rb_ = rb;
}

void McuLink::set_job_event_callback(McuLinkEventCb cb)
{
    event_cb_ = std::move(cb);
}

void McuLink::set_progress_callback(McuLinkProgressCb cb)
{
    progress_cb_ = std::move(cb);
}

/* ---------- 静态工具 ---------- */

bool McuLink::set_tcp_nodelay(int sockfd)
{
    int flag = 1;
    return setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

/* ---------- TCP 连接 ---------- */

int McuLink::connect_tcp()
{
    const char* ip = GLAVO_TCP_DEFAULT_IP;
    const int   port = GLAVO_TCP_DEFAULT_PORT;

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
        UPC_LOG_MCU_LINK(Warn, "TCP_NODELAY set failed");
    }

    // 设为非阻塞模式，方便链路线程做轮询式收发
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    tcp_fd_ = fd;
    tcp_rx_accum_len_ = 0; // 清空接收累积区

    UPC_LOG_MCU_LINK(Info, "TCP connected %s:%d (NODELAY=1)", ip, port);
    return 0;
}

/* ---------- 串口打开与配置 ---------- */

int McuLink::open_serial(const char* device, int baud)
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
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN); // 原始输入（非规范模式）
    /* 完全原始输入：务必清 ICRNL/INLCR/IGNCR，否则收到的 0x0D 会被内核转成 0x0A，
     * 破坏二进制帧内容与校验和（曾导致传感器帧 TLV-len 0x0D 被收成 0x0A）。 */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR |
                     ISTRIP | INPCK | PARMRK | BRKINT | IGNBRK);
    tty.c_oflag &= ~OPOST;                     // 原始输出
    tty.c_cc[VMIN]  = 0;                       // 非阻塞读的最小字符数
    tty.c_cc[VTIME] = 0;                       // 非阻塞读的超时时间（deciseconds）

    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH); // 清空收发缓冲区

    serial_fd_ = fd;
    serial_rx_accum_len_ = 0;

    UPC_LOG_MCU_LINK(Info, "serial open %s @ %d", device, baud);
    return 0;
}

/* ---------- 链路线程控制 ---------- */

void McuLink::start_link_thread()
{
    if(link_run_.load()) {
        return; // 已在运行
    }
    link_stop_.store(false);
    link_run_.store(true);
    if(link_th_.joinable()) {
        link_th_.join(); // 等待旧线程结束（理论上不应发生）
    }
    link_th_ = std::thread(&McuLink::link_thread_fn, this);
}

void McuLink::stop()
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
    serial_clear_reliable();
}

/* ---------- 状态查询 ---------- */

bool McuLink::is_tcp_connected() const { return tcp_fd_ >= 0; }
bool McuLink::is_sending() const { return is_sending_.load(); }

/* ---------- 事件与回调 ---------- */

void McuLink::emit_event(McuLinkEvent ev)
{
    if(event_cb_) {
        event_cb_(ev);
    }
}

void McuLink::enqueue_serial_tx(const uint8_t* data, size_t len, bool reliable)
{
    if(!data || len == 0 || len > sizeof(SerialTxItem::data)) {
        return;
    }
    SerialTxItem item{};
    std::memcpy(item.data, data, len);
    item.len = len;
    item.reliable = reliable;
    std::lock_guard<std::mutex> lock(serial_tx_mtx_);
    if(serial_tx_queue_.size() >= kSerialTxQueueMax) {
        serial_tx_queue_.pop_front();
    }
    serial_tx_queue_.push_back(item);
}

EnvSnapshotCache McuLink::env_snapshot() const
{
    std::lock_guard<std::mutex> lock(env_mtx_);
    return env_cache_;
}

void McuLink::drain_serial_tx_queue()
{
    if(serial_fd_ < 0) {
        return;
    }
    for(;;) {
        SerialTxItem item{};
        {
            std::lock_guard<std::mutex> lock(serial_tx_mtx_);
            if(serial_tx_queue_.empty()) {
                break;
            }
            item = serial_tx_queue_.front();
            serial_tx_queue_.pop_front();
        }
        /* reliable 项走带 ACK 的可靠通道（本函数在 link 线程内调用，
         * 与 serial_reliable_* 状态机同线程，无需额外加锁） */
        if(item.reliable) {
            serial_send_aa_reliable(item.data, item.len, false);
        } else {
            serial_send_raw(item.data, item.len);
        }
    }
}

void McuLink::on_sensor_report(const serial_sensor_report_t& report)
{
    {
        std::lock_guard<std::mutex> lock(env_mtx_);
        env_cache_.pm25_cabin      = report.pm25_ug_m3;
        env_cache_.pm25_indoor     = report.pm25_ug_m3;
        env_cache_.temperature_c   = report.temperature_c;
        env_cache_.oxygen_percent  = report.oxygen_percent;
        env_cache_.humidity_percent = report.humidity_percent;
        env_cache_.valid           = true;
    }
    publish_env_update_ntf();
    publish_debug_sensor_ntf(report);   /* 调试页顶部 7 路传感器回读 */

    UPC_LOG_MCU_LINK(Info,
        "sensor: O2=%.1f%% P=%dmbar T=%.1fC RH=%.1f%% PM2.5=%.1f base=%.1fC powder=%d",
        static_cast<double>(report.oxygen_percent), report.pressure_mbar,
        static_cast<double>(report.temperature_c),
        static_cast<double>(report.humidity_percent),
        static_cast<double>(report.pm25_ug_m3),
        static_cast<double>(report.substrate_temp_c), report.powder_present);
}

void McuLink::on_motion_pos_report(const serial_motion_pos_t& pos)
{
    publish_debug_axis_ntf(pos);        /* 调试页三轴位置 + Z 工作坐标回读 */

    UPC_LOG_MCU_LINK(Info,
        "motion: Z=%.3fmm Zwork=%.3fmm S=%.3fmm D=%.3fdeg",
        static_cast<double>(pos.z_pos_mm), static_cast<double>(pos.z_work_zero_mm),
        static_cast<double>(pos.s_pos_mm), static_cast<double>(pos.d_pos_deg));
}

void McuLink::on_laser_report(const serial_laser_report_t& report)
{
    publish_debug_laser_ntf(report);    /* 调试页 Laser 标签状态回读 */

    UPC_LOG_MCU_LINK(Info,
        "laser: status=0x%04X alarm=0x%04X power=%u pump=%u",
        report.status, report.alarm, report.power_raw, report.pump_temp_raw);
}

void McuLink::publish_env_update_ntf()
{
    const EnvSnapshotCache snap = env_snapshot();
    if(!snap.valid) {
        return;
    }

    const auto job = PrintJobState::instance().snapshot();
    middleware::UserHomeStatus status = middleware::UserHomeStatus::Idle;
    if(job.is_sending) {
        status = middleware::UserHomeStatus::Printing;
    } else if(job.job_error) {
        status = middleware::UserHomeStatus::Error;
    } else if(job.job_completed) {
        status = middleware::UserHomeStatus::Ready;
    } else if(is_tcp_connected()) {
        status = middleware::UserHomeStatus::Ready;
    }

    middleware::EnvSnapshotPayload p{};
    std::snprintf(p.device_name, sizeof(p.device_name), "%s", "Rongsu-3D");
    p.status         = static_cast<uint32_t>(status);
    p.wifi_connected = is_tcp_connected() ? 1 : 0;
    p.signal_bars    = p.wifi_connected ? 3 : 0;
    p.pm25_indoor    = snap.pm25_indoor;
    p.pm25_cabin     = snap.pm25_cabin;
    p.temperature_c  = snap.temperature_c;
    p.oxygen_percent = snap.oxygen_percent;

    middleware::Message ntf(middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_GLAVO,
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MCU_LINK(Warn, "APP_Update_Ntf publish_async failed");
    }
}

void McuLink::publish_debug_sensor_ntf(const serial_sensor_report_t& report)
{
    middleware::DebugSensorPayload p{};
    p.oxygen_percent   = report.oxygen_percent;
    p.pressure_kpa     = static_cast<float>(report.pressure_mbar) / 10.0f; /* mbar→kPa */
    p.pm25             = report.pm25_ug_m3;
    p.humidity_pct     = report.humidity_percent;
    p.cabin_temp_c     = report.temperature_c;
    p.baseplate_temp_c = report.substrate_temp_c;
    p.powder_status    = report.powder_present
        ? static_cast<uint8_t>(middleware::DebugPowderStatus::Available)
        : static_cast<uint8_t>(middleware::DebugPowderStatus::Fault);

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Sensor_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    middleware::MsgPort::instance().publish_async(ntf);
}

void McuLink::publish_debug_axis_ntf(const serial_motion_pos_t& pos)
{
    middleware::DebugAxisStatePayload p{};
    p.z_pos_mm       = pos.z_pos_mm;
    p.s_pos_mm       = pos.s_pos_mm;
    p.d_pos_deg      = pos.d_pos_deg;
    p.z_work_zero_mm = pos.z_work_zero_mm;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Axis_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    middleware::MsgPort::instance().publish_async(ntf);
}

void McuLink::publish_debug_laser_ntf(const serial_laser_report_t& report)
{
    middleware::DebugLaserStatePayload p{};
    p.status        = report.status;
    p.alarm         = report.alarm;
    p.power_raw     = report.power_raw;
    p.pump_temp_raw = report.pump_temp_raw;

    middleware::Message ntf(middleware::MsgType::GLAVO_Debug_Laser_Ntf,
        middleware::ModuleId::APP_GLAVO, middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    middleware::MsgPort::instance().publish_async(ntf);
}

void McuLink::notify_serial_event(serial_event_t evt)
{
    const char* evt_name = "UNKNOWN";
    switch(evt) {
        case SERIAL_EVT_PRINT_PREPARING:    evt_name = "PRINT_PREPARING";    break;
        case SERIAL_EVT_PRINT_CONDITION_OK: evt_name = "PRINT_CONDITION_OK"; break;
        case SERIAL_EVT_PRINT_READY:        evt_name = "PRINT_READY";        break;
        default:                            evt_name = "NONE";               break;
    }
    UPC_LOG_MCU_LINK(Info, "notify_serial_event: %s", evt_name);
    if(evt == SERIAL_EVT_PRINT_PREPARING) {
        return;
    }
    if(evt == SERIAL_EVT_PRINT_CONDITION_OK) {
        serial_evt_condition_ok_.store(true);
    } else if(evt == SERIAL_EVT_PRINT_READY) {
        serial_evt_print_ready_.store(true);
    }
}

void McuLink::handle_serial_rx_frame(const uint8_t* frame, size_t len)
{
#if UPC_DEBUG && UPC_MOD_MCU_LINK
    serial_log_rx_frame(frame, len);
#endif

    if(serial_is_mcu_ack(frame, len)) {
        on_serial_mcu_ack(frame, len);
        return;
    }

    if(!serial_is_mcu_data(frame, len)) {
        return;
    }

#if GLAVO_SERIAL_ACK_ENABLE
    send_soc_ack_for_bb(frame, len);
#endif

    serial_sensor_report_t sensor{};
    serial_sensor_err_t serr = SERIAL_SENSOR_ERR_NONE;
    if(serial_parse_sensor_report_detail(frame, len, &sensor, &serr) == 0) {
        on_sensor_report(sensor);
        return;
    }

    /* 运动轴位置上报（0x9B + TLV 0x05）：Z/S/D 位置 + Z 工作坐标 */
    serial_motion_pos_t mpos{};
    if(serial_parse_motion_pos_report(frame, len, &mpos) == 0) {
        on_motion_pos_report(mpos);
        return;
    }

    /* 激光器状态上报（0x9B + TLV 0x03）：状态/报警/功率/泵浦温度 */
    serial_laser_report_t laser{};
    if(serial_parse_laser_report(frame, len, &laser) == 0) {
        on_laser_report(laser);
        return;
    }

    const serial_event_t evt = serial_parse_frame(frame, len);
    if(evt != SERIAL_EVT_NONE) {
        notify_serial_event(evt);
    }
}

/* ---------- TCP 原始收发 ---------- */

int McuLink::tcp_send_raw(const uint8_t* data, size_t len)
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
        }
        return -1;
    }
    return 0;
}

int McuLink::tcp_recv_nonblock(uint8_t* buf, size_t buf_size)
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
        return -1;
    }
    return (int)n;
}

void McuLink::tcp_drain_rx()
{
    uint8_t chunk[256];
    auto recv_fn = +[](uint8_t* b, size_t s) -> int {
        return McuLink::instance().tcp_recv_nonblock(b, s);
    };
    drain_framed_rx(recv_fn, chunk, sizeof(chunk),
                    tcp_rx_accum_, sizeof(tcp_rx_accum_), &tcp_rx_accum_len_,
                    tcp_guess_rx_frame_len,
                    +[](const uint8_t* f, size_t l) { tcp_handle_frame(f, l); });
}

/* ---------- 串口原始收发 ---------- */

int McuLink::serial_send_raw(const uint8_t* data, size_t len)
{
    if(serial_fd_ < 0 || !data || len == 0) {
        return -1;
    }
#if UPC_DEBUG && UPC_MOD_MCU_LINK
    serial_log_frame_hex("TX", data, len);
#endif
    const ssize_t n = write(serial_fd_, data, len);
    return n == (ssize_t)len ? 0 : -1;
}

void McuLink::serial_clear_reliable()
{
    serial_reliable_pending_ = {};
    serial_reliable_queue_.clear();
}

void McuLink::serial_start_reliable_item(const SerialReliableItem& item)
{
    if(item.len == 0 || item.len > sizeof(item.data)) {
        return;
    }

    serial_reliable_pending_.active = true;
    std::memcpy(serial_reliable_pending_.data, item.data, item.len);
    serial_reliable_pending_.len = item.len;
    serial_reliable_pending_.retry_count = 0;
    serial_reliable_pending_.abort_print_on_fail = item.abort_print_on_fail;

    serial_ack_key_t key{};
    if(serial_extract_ack_key(item.data, item.len, &key) != 0) {
        serial_reliable_pending_.active = false;
        UPC_LOG_MCU_LINK(Warn, "serial reliable: bad AA key");
        return;
    }
    serial_reliable_pending_.key = key;

    serial_send_raw(item.data, item.len);
    serial_reliable_pending_.deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(GLAVO_SERIAL_ACK_TIMEOUT_MS);

    UPC_LOG_MCU_LINK(Debug,
        "serial reliable TX port=%02X cmd=%02X type=%02X abort=%d",
        key.port, key.cmd, key.type, item.abort_print_on_fail ? 1 : 0);
}

void McuLink::serial_advance_reliable_queue()
{
    serial_reliable_pending_.active = false;
    if(serial_reliable_queue_.empty()) {
        return;
    }
    const SerialReliableItem next = serial_reliable_queue_.front();
    serial_reliable_queue_.pop_front();
    serial_start_reliable_item(next);
}

void McuLink::serial_send_aa_reliable(const uint8_t* data, size_t len,
                                      bool abort_print_on_fail)
{
    if(!data || len == 0 || len > sizeof(SerialReliableItem::data)) {
        return;
    }

#if !GLAVO_SERIAL_ACK_ENABLE
    serial_send_raw(data, len);
    return;
#endif

    if(!serial_is_soc_data(data, len)) {
        serial_send_raw(data, len);
        return;
    }

    SerialReliableItem item{};
    std::memcpy(item.data, data, len);
    item.len = len;
    item.abort_print_on_fail = abort_print_on_fail;

    if(serial_reliable_pending_.active) {
        serial_reliable_queue_.push_back(item);
        return;
    }
    serial_start_reliable_item(item);
}

void McuLink::on_serial_mcu_ack(const uint8_t* frame, size_t len)
{
#if !GLAVO_SERIAL_ACK_ENABLE
    (void)frame;
    (void)len;
    return;
#endif
    if(!serial_reliable_pending_.active) {
        return;
    }

    serial_ack_key_t key{};
    if(serial_extract_ack_key(frame, len, &key) != 0) {
        return;
    }
    if(!serial_ack_keys_match(&key, &serial_reliable_pending_.key)) {
        return;
    }

    UPC_LOG_MCU_LINK(Debug,
        "serial reliable ACK port=%02X cmd=%02X type=%02X",
        key.port, key.cmd, key.type);
    serial_advance_reliable_queue();
}

void McuLink::send_soc_ack_for_bb(const uint8_t* frame, size_t len)
{
#if !GLAVO_SERIAL_ACK_ENABLE
    (void)frame;
    (void)len;
    return;
#endif
    uint8_t ack[16];
    const int alen = serial_build_soc_ack(frame, len, ack, sizeof(ack));
    if(alen > 0) {
        serial_send_raw(ack, (size_t)alen);
    }
}

void McuLink::link_process_serial_reliable_tx()
{
#if !GLAVO_SERIAL_ACK_ENABLE
    return;
#endif
    if(!serial_reliable_pending_.active) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if(now < serial_reliable_pending_.deadline) {
        return;
    }

    if(serial_reliable_pending_.retry_count >= GLAVO_SERIAL_ACK_MAX_RETRY) {
        UPC_LOG_MCU_LINK(Warn,
            "serial reliable timeout port=%02X cmd=%02X type=%02X retries=%d",
            serial_reliable_pending_.key.port,
            serial_reliable_pending_.key.cmd,
            serial_reliable_pending_.key.type,
            serial_reliable_pending_.retry_count);

        const bool abort = serial_reliable_pending_.abort_print_on_fail;
        serial_clear_reliable();

        if(abort) {
            PrintJobState::instance().set_job_error(true);
            parser::ParserManager::instance().stop_parse();
            request_terminate();
        }
        return;
    }

    ++serial_reliable_pending_.retry_count;
    serial_send_raw(serial_reliable_pending_.data,
                    serial_reliable_pending_.len);
    serial_reliable_pending_.deadline =
        now + std::chrono::milliseconds(GLAVO_SERIAL_ACK_TIMEOUT_MS);

    UPC_LOG_MCU_LINK(Warn,
        "serial reliable retry #%d port=%02X cmd=%02X type=%02X",
        serial_reliable_pending_.retry_count,
        serial_reliable_pending_.key.port,
        serial_reliable_pending_.key.cmd,
        serial_reliable_pending_.key.type);
}

int McuLink::serial_recv_nonblock(uint8_t* buf, size_t buf_size)
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

void McuLink::serial_drain_rx()
{
    uint8_t chunk[64];
    auto recv_fn = +[](uint8_t* b, size_t s) -> int {
        return McuLink::instance().serial_recv_nonblock(b, s);
    };
    drain_framed_rx(recv_fn, chunk, sizeof(chunk),
                    serial_rx_accum_, sizeof(serial_rx_accum_), &serial_rx_accum_len_,
                    serial_guess_rx_frame_len,
                    +[](const uint8_t* f, size_t l) { serial_handle_frame(f, l); });
}

/* ---------- tcp_stage_ 队列操作 ---------- */

void McuLink::tcp_stage_clear()
{
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    tcp_stage_.clear();
}

size_t McuLink::tcp_stage_count()
{
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    return tcp_stage_.size();
}

bool McuLink::tcp_stage_peek_type(parser::rb_pkt_type_t* ptype)
{
    if(!ptype) {
        return false;
    }
    std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
    if(tcp_stage_.empty()) {
        return false;
    }
    *ptype = static_cast<parser::rb_pkt_type_t>(tcp_stage_.front().type);
    return true;
}

bool McuLink::tcp_stage_pop_front(parser::rb_packet_t* pkt)
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
void McuLink::refill_tcp_stage_from_ring()
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

        parser::rb_pkt_type_t ptype;
        if(!rb_->peek_type(ptype)) {
            break; // RingBuffer 已空
        }

        parser::rb_packet_t pkt;
        if(!rb_->get(pkt, 5)) {
            break; // 获取失败（超时或无数据）
        }

        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        tcp_stage_.push_back(pkt);
        moved++;
    }
}

/* ---------- 状态辅助 ---------- */

bool McuLink::mcu_rates_drained() const
{
    const auto snap = PrintJobState::instance().snapshot();
    return snap.cmd_cache_rate  <= static_cast<uint16_t>(MCU_DRAIN_MAX_RATE) &&
           snap.data_cache_rate <= static_cast<uint16_t>(MCU_DRAIN_MAX_RATE);
}

bool McuLink::motion_stream_active() const
{
    /* natural_complete_pending 阶段须发心跳刷新 rate，不能视为活跃下发 */
    return is_sending_.load() && !waiting_layer_.load() &&
           !terminate_pending_.load() && !natural_complete_pending_.load();
}

void McuLink::poll_mcu_drain_heartbeat()
{
    if(tcp_fd_ < 0) {
        return;
    }
    uint8_t hb[64];
    const int hblen = build_heartbeat_frame(hb, sizeof(hb));
    if(hblen > 0) {
        tcp_send_raw(hb, (size_t)hblen);
        tcp_drain_rx();
    }
}

/* ---------- 开始阈值判断 ---------- */

/**
 * @brief 判断 RingBuffer 是否满足开始发送的阈值条件
 * @return true 表示可以立即启动发送
 *
 * 判定逻辑：
 * 1. 若 RingBuffer 中已缓存 >= TCP_BATCH_SIZE 个包，直接满足；
 * 2. 若解析器仍在解析中：
 *    - 若总运动包数 >= batch 且当前有包，则满足；
 *    - 若处于 start_pending 挂起状态，且文件已读完、总包数 < batch 但 > 0，
 *      说明文件很小，也满足；
 * 3. 若解析器已结束解析但文件已打开：只要总包数 > 0 且有包，即满足。
 */
bool McuLink::ring_meets_start_threshold() const
{
    if(!rb_) {
        return false;
    }

    parser::IParserWorker* parser = active_parser();
    if(!parser) {
        return false;
    }

    const size_t count = rb_->count();
    const int batch = TCP_BATCH_SIZE;
    const int total = parser->get_total_motion_packets();

    if(count >= static_cast<size_t>(batch)) {
        return true;
    }
    if(parser->is_parsing()) {
        if(total >= batch && count > 0) {
            return true;
        }
        if(start_pending_.load() && parser->is_file_read_exhausted() &&
           total > 0 && total < batch && count > 0) {
            return true; // 小文件特例
        }
        return false;
    }
    if(parser->is_file_open()) {
        return total > 0 && count > 0;
    }
    return false;
}

/* ---------- 运动状态管理 ---------- */

void McuLink::reset_motion_state(bool clear_start_pending)
{
    if(clear_start_pending) {
        start_pending_.store(false);
    }
    is_sending_.store(false);
    waiting_layer_.store(false);
    layer_drain_pending_.store(false);
    natural_complete_pending_.store(false);
    natural_complete_since_ = {};
    serial_handshake_phase_.store(SERIAL_HS_NONE);
    serial_evt_condition_ok_.store(false);
    serial_evt_print_ready_.store(false);
    sent_at_layer_ready_ = 0;
    current_layer_snapped_ = false;
    tcp_stage_clear();
    serial_clear_reliable();
    PrintJobState::instance().set_sending(false);
}

void McuLink::begin_sending_internal()
{
    is_sending_.store(true);
    waiting_layer_.store(false);
    layer_drain_pending_.store(false);
    natural_complete_pending_.store(false);
    natural_complete_since_ = {};
    tcp_stage_clear();

    // 记录首层高度，用于串口首次握手
    parser::IParserWorker* parser = active_parser();
    first_layer_height_um_  = parser ? parser->get_layer_height_um() : GCODE_DEFAULT_LAYER_UM;
    active_layer_height_um_ = first_layer_height_um_;
    current_layer_snapped_  = false;

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
            UPC_LOG_MCU_LINK(Info, "serial TX cmd: PRINT_REQUEST");
            serial_send_aa_reliable(sbuf, (size_t)slen, true);
        }
        waiting_layer_.store(true);
        serial_handshake_phase_.store(SERIAL_HS_WAIT_CONDITION_OK);
        serial_evt_condition_ok_.store(false);
        serial_evt_print_ready_.store(false);
    }
#endif

    UPC_LOG_MCU_LINK(Info, "begin_sending");
}

void McuLink::begin_sending()
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

void McuLink::try_complete_pending_start()
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

void McuLink::request_terminate()
{
    terminate_pending_.store(true);
    is_sending_.store(false);
    start_pending_.store(false);
    PrintJobState::instance().set_sending(false);
}

/* ---------- 清理辅助：协议帧发送 ---------- */

void McuLink::send_tcp_reset()
{
    uint8_t frame[64];
    const int flen = build_reset_frame(frame, sizeof(frame));
    if(flen > 0) {
        tcp_send_raw(frame, (size_t)flen);
    }
}

void McuLink::send_serial_print_terminate()
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_terminate(sbuf, sizeof(sbuf));
        if(slen > 0) {
            UPC_LOG_MCU_LINK(Info, "serial TX cmd: PRINT_TERMINATE");
            serial_send_aa_reliable(sbuf, (size_t)slen, false);
        }
    }
#endif
}

void McuLink::send_serial_print_finished()
{
#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_finished(sbuf, sizeof(sbuf));
        if(slen > 0) {
            UPC_LOG_MCU_LINK(Info, "serial TX cmd: PRINT_FINISHED");
            serial_send_aa_reliable(sbuf, (size_t)slen, false);
        }
    }
#endif
}

/* ---------- 终止/完成清理流程 ---------- */

void McuLink::run_manual_terminate_cleanup()
{
    // 1. 向振镜发 Reset，向 MCU 发终止通知
    send_tcp_reset();
    send_serial_print_terminate();

    // 2. 停止解析、清空 RingBuffer 与 stage 队列
    parser::ParserManager::instance().stop_parse();
    if(rb_) {
        rb_->clear();
    }
    tcp_stage_clear();

    // 3. 判断是否是错误导致的终止，选择对应事件上报
    const bool was_error = PrintJobState::instance().snapshot().job_error;
    reset_motion_state(true);
    PrintJobState::instance().reset_job_counters();

    emit_event(was_error ? McuLinkEvent::PrintError : McuLinkEvent::TerminateDone);
}

void McuLink::link_try_final_layer_snap()
{
#if !GLAVO_ENABLE_LAYER_SNAP
    return;
#endif
    if(current_layer_snapped_) {
        return;
    }
    if(PrintJobState::instance().snapshot().sent_packets <= sent_at_layer_ready_) {
        return;
    }
    PrintVideoNotify::instance().on_layer_snap(active_layer_height_um_);
    current_layer_snapped_ = true;
}

void McuLink::run_natural_complete_cleanup()
{
    // 1. 末层无 M3000 时补 Snap，再通知 MCU 打印完成
    link_try_final_layer_snap();
    send_serial_print_finished();
    send_tcp_reset();

    // 2. 重置状态并上报完成事件
    reset_motion_state(true);
    PrintJobState::instance().set_job_completed(true);
    PrintJobState::instance().set_sending(false);

    emit_event(McuLinkEvent::PrintCompleted);
}

/* ---------- 串口握手与换层处理 ---------- */

void McuLink::link_process_serial_handshake()
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
            UPC_LOG_MCU_LINK(Info, "serial TX cmd: PRINT_START (layer=%u um)",
                static_cast<unsigned>(first_layer_height_um_));
            serial_send_aa_reliable(sbuf, (size_t)slen, true);
        }
        serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
        serial_evt_print_ready_.store(false);
        return;
    }

    // 阶段二：收到 PRINT_READY 后，解除等待，开始下发本层运动包
    if(phase == SERIAL_HS_WAIT_PRINT_READY &&
       serial_evt_print_ready_.exchange(false)) {
        sent_at_layer_ready_ = PrintJobState::instance().snapshot().sent_packets;
        current_layer_snapped_ = false;
        waiting_layer_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
    }
}

bool McuLink::link_handle_layer_mark()
{
    parser::rb_pkt_type_t ptype;
    if(!tcp_stage_peek_type(&ptype) || ptype != parser::RB_PKT_LAYER_MARK) {
        return false; // 队列头部不是换层标记
    }

    parser::rb_packet_t mark;
    if(!tcp_stage_pop_front(&mark)) {
        return false;
    }

#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ < 0) {
        return false; // 无串口时不进行换层握手
    }
    // 读取换层标记中的下层成高（协议 0x05 载荷）
    const uint16_t mark_um = parser::rb_layer_mark_get_height_um(mark);
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

void McuLink::link_try_layer_serial_dispatch()
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

#if GLAVO_ENABLE_LAYER_SNAP
    PrintVideoNotify::instance().on_layer_snap(active_layer_height_um_);
    current_layer_snapped_ = true;
#endif

    // 发送 layer_done 帧，通知 MCU 当前层已结束
    uint8_t sbuf[32];
    const int slen = serial_build_layer_done(active_layer_height_um_, sbuf, sizeof(sbuf));
    if(slen > 0) {
        UPC_LOG_MCU_LINK(Info, "serial TX cmd: LAYER_DONE (next_layer=%u um)",
            static_cast<unsigned>(active_layer_height_um_));
        serial_send_aa_reliable(sbuf, (size_t)slen, true);
    }
    layer_drain_pending_.store(false);
    // 转入等待 PRINT_READY 状态
    serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
    serial_evt_print_ready_.store(false);
}

/* ---------- 运动包批量下发 ---------- */

int McuLink::link_send_one_motion_batch()
{
    if(tcp_stage_count() == 0) {
        return 0; // 暂存队列为空
    }

    parser::rb_packet_t staged[kMaxMotionBatch];
    int n = 0;

    // 从 tcp_stage_ 头部连续取出运动包，遇到 LayerMark 或非运动包则停止
    for(int i = 0; i < kMaxMotionBatch; i++) {
        parser::rb_pkt_type_t ptype;
        if(!tcp_stage_peek_type(&ptype)) {
            break; // 队列为空
        }
        if(ptype == parser::RB_PKT_LAYER_MARK) {
            break; // 遇到换层标记，留待外层处理
        }
        if(ptype != parser::RB_PKT_MOTION) {
            // 遇到未知类型包，丢弃并终止本轮批量
            parser::rb_packet_t discard;
            tcp_stage_pop_front(&discard);
            break;
        }
        parser::rb_packet_t rp;
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

    // 触发进度回调
    if(progress_cb_) {
        progress_cb_();
    }
    return n;
}

/* ---------- 链路主逻辑：运动队列排空与完成检测 ---------- */

void McuLink::link_drain_motion_queue()
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

void McuLink::link_try_finish_sending()
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
    parser::IParserWorker* parser = active_parser();
    if(!parser) {
        return;
    }
    if(parser->is_parsing()) {
        return; // 解析器仍在解析，可能还有新数据产生
    }
    if(!parser->is_file_read_exhausted()) {
        return; // 文件尚未读完
    }

    const auto now = std::chrono::steady_clock::now();

    // 所有运动包已下发，但需等 MCU 缓存排空才算真正完成
    if(!mcu_rates_drained()) {
        if(!natural_complete_pending_.load()) {
            natural_complete_since_ = now;
        }
        natural_complete_pending_.store(true);
        poll_mcu_drain_heartbeat();

        if(MCU_DRAIN_TIMEOUT_MS > 0 &&
           natural_complete_since_.time_since_epoch().count() != 0 &&
           now - natural_complete_since_ >=
               std::chrono::milliseconds(MCU_DRAIN_TIMEOUT_MS)) {
            const auto snap = PrintJobState::instance().snapshot();
            UPC_LOG_MCU_LINK(Warn,
                "finish wait: drain timeout (%d ms), force complete cmd_rate=%u data_rate=%u",
                MCU_DRAIN_TIMEOUT_MS,
                static_cast<unsigned>(snap.cmd_cache_rate),
                static_cast<unsigned>(snap.data_cache_rate));
        } else {
            return;
        }
    }

    // 真正自然完成：执行清理并上报事件
    natural_complete_pending_.store(false);
    natural_complete_since_ = {};
    is_sending_.store(false);
    run_natural_complete_cleanup();
}

/* ---------- 链路线程休眠策略 ---------- */

int McuLink::link_thread_sleep_ms() const
{
    if(ota_session_.is_active()) {
        return TCP_SEND_INTERVAL_MS;
    }
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

/* ---------- OTA（测试） ---------- */

void McuLink::set_ota_status_callback(McuOtaStatusCb cb)
{
    ota_session_.set_status_callback(std::move(cb));
}

bool McuLink::start_mcu_ota(const char* image_dir)
{
    if(!is_tcp_connected()) {
        UPC_LOG_MCU_OTA(Error, "OTA start: TCP not connected");
        return false;
    }
    if(is_sending_.load()) {
        UPC_LOG_MCU_OTA(Warn, "OTA start: print active");
        return false;
    }
    return ota_session_.start(image_dir);
}

void McuLink::cancel_mcu_ota()
{
    ota_session_.cancel();
}

bool McuLink::ota_active() const
{
    return ota_session_.is_active();
}

void McuLink::on_tcp_status_response(const tcp_response_t& resp)
{
    ota_session_.on_status_response(resp);
#if GLAVO_OTA_POST_RECONNECT_ENABLE
    if(ota_session_.take_post_success_reconnect()) {
        ota_post_reconnect_.store(true);
        UPC_LOG_MCU_OTA(Info, "OTA success: schedule TCP post-reconnect");
    }
#endif
    if(!ota_session_.is_active()) {
        PrintJobState::instance().set_mcu_rates(resp.cmd_rate, resp.data_rate);
    }
}

void McuLink::ota_tick()
{
    ota_session_.tick(
        +[](const uint8_t* data, size_t len) -> int {
            return McuLink::instance().tcp_send_raw(data, len);
        },
        +[]() { McuLink::instance().tcp_drain_rx(); },
        tcp_fd_ >= 0);
}

void McuLink::link_try_ota_post_reconnect()
{
    const bool ota_pending = ota_post_reconnect_.load();
    if(ota_pending && ota_session_.is_active()) {
        return;
    }

    const int rc = connect_tcp();
    if(rc == 0) {
        if(ota_pending) {
            ota_post_reconnect_.store(false);
            UPC_LOG_MCU_OTA(Info, "OTA post-reconnect TCP OK %s:%d",
                            GLAVO_TCP_DEFAULT_IP, GLAVO_TCP_DEFAULT_PORT);
        }
        return;
    }

    if(ota_pending) {
        UPC_LOG_MCU_OTA(Warn, "OTA post-reconnect TCP fail %s:%d rc=%d",
                        GLAVO_TCP_DEFAULT_IP, GLAVO_TCP_DEFAULT_PORT, rc);
    }
}

/* ---------- 链路线程主循环 ---------- */

void McuLink::link_thread_fn()
{
    using clock = std::chrono::steady_clock;
    auto next_hb = clock::now(); // 下次心跳发送时间点
    auto next_tcp_retry = clock::now(); // 下次 TCP 重连尝试时间点

    while(link_run_.load() && !link_stop_.load()) {
        const auto now = clock::now();

        // ---------- 串口 TX/RX/可靠重传：独立于 TCP，始终处理 ----------
        // 串口（MCU 调试链路）与 TCP（振镜）相互独立；即使振镜未连接，
        // 调试命令（UI_Debug_*）与传感器上报也必须照常收发。
        drain_serial_tx_queue();
        serial_drain_rx();
        link_process_serial_reliable_tx();

        // ---------- TCP RX 排空与断线检测/重连（不阻塞串口） ----------
        if(tcp_fd_ >= 0) {
            tcp_drain_rx();
        } else if(is_sending_.load() && !ota_session_.is_active()) {
            // TCP 在发送过程中断开：标记错误并触发终止流程
            PrintJobState::instance().set_job_error(true);
            is_sending_.store(false);
            terminate_pending_.store(true);
        } else if(!terminate_pending_.load() && !ota_session_.is_active()) {
            // 空闲断线或 OTA 成功后：节流式 connect_tcp（固定 IP/PORT）
            if(tcp_fd_ < 0 || ota_post_reconnect_.load()) {
                if(ota_post_reconnect_.load() || now >= next_tcp_retry) {
                    link_try_ota_post_reconnect();
                    next_tcp_retry =
                        now + std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS);
                }
            }
        }

        if(ota_session_.is_active()) {
            ota_tick();
        } else if(terminate_pending_.load()) {
            run_manual_terminate_cleanup();
            terminate_pending_.store(false);
        } else {
            // ---------- 心跳 ----------
            const bool stream = motion_stream_active();
            if(!stream && !ota_session_.suppress_idle_heartbeat() &&
               tcp_fd_ >= 0 && now >= next_hb) {
                uint8_t hb[64];
                const int hblen = build_heartbeat_frame(hb, sizeof(hb));
                if(hblen > 0) {
                    tcp_send_raw(hb, (size_t)hblen);
                }
                next_hb = now + std::chrono::milliseconds(TCP_HEARTBEAT_INTERVAL_MS);
                tcp_drain_rx(); // 心跳发送后再排空一次 RX
            }

            // ---------- 串口换层握手（打印流程相关） ----------
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

} // namespace upc::glavo
