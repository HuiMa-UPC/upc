#include "ctrl_backend.hpp"
#include "protocol_mcu.hpp"
#include "job_status.hpp"
#include "gcode_parse_gains.hpp"
#include "gcode_worker.hpp"
#include "pipeline_tune.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

/* ---------- 内部状态 ---------- */
static int              s_tcp_fd = -1;
static int              s_serial_fd = -1;
static RingBuffer      *s_rb = nullptr;
static std::thread      s_link_th;
static std::atomic<bool> s_link_run{false};
static std::atomic<bool> s_link_stop_req{false};

/** TCP 连接后常态心跳间隔（与 UI「Intv(ms)」无关，后者仅控制发送线程轮询周期） */
static constexpr int HEARTBEAT_INTERVAL_MS = 100;
static std::atomic<bool> s_is_sending{false};
static std::atomic<bool> s_is_paused{false};
static std::atomic<int>  s_sent_count{0};

static int              s_batch_size = 100;
static int              s_interval_ms = 3;
static uint16_t         s_layer_height_um = 100;
static int              s_enable_layer_change = 0;

static std::atomic<bool> s_waiting_layer{false};
static std::atomic<bool> s_layer_handshake_done{false};
/** 换层会话：首层发 PRINT_START，Reset/再次 Start 须置 true */
static bool s_link_first_layer = true;

static uint8_t          s_tcp_rx_accum[1024];
static size_t           s_tcp_rx_accum_len = 0;
static std::mutex       s_tcp_send_mtx;

/** 单包运动子帧 11B，batch 最大 100 → 1103B 帧缓冲 */
static constexpr int    kMaxMotionBatch   = 100;
static constexpr size_t kMotionFrameSize = 3u + 11u * kMaxMotionBatch;

/* ---------- TCP 辅助 ---------- */
static int tcp_send_raw(const uint8_t *data, size_t len)
{
    if(s_tcp_fd < 0 || !data || len == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(s_tcp_send_mtx);
    const ssize_t n = send(s_tcp_fd, data, len, 0);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int tcp_recv_nonblock(uint8_t *buf, size_t buf_size)
{
    if (s_tcp_fd < 0) return -1;
    ssize_t n = recv(s_tcp_fd, buf, buf_size, MSG_DONTWAIT);
    return (int)n;
}

/** 根据下位机帧格式推断完整帧长；0 表示数据不足或需跳过一个坏字节 */
static size_t tcp_guess_frame_len(const uint8_t *buf, size_t avail)
{
    if(avail < 4) {
        return 0;
    }
    if(buf[0] != 0xBE || buf[1] != 0xEF) {
        return 1;
    }

    /* 读参应答 81B：BE EF 78 0E + 7×11 */
    if(buf[3] == 0x0E && buf[2] >= 78 && avail >= 81) {
        return 81;
    }

    /* 常态/心跳应答 19B：BE EF 10 01 ... */
    if(buf[2] == 0x10 && buf[3] == 0x01 && avail >= 19) {
        return 19;
    }

    /* 命令帧：BE EF pkg_num + pkg_num×11（单包 14B） */
    const uint8_t pkg_num = buf[2];
    if(pkg_num > 0 && pkg_num < 64) {
        const size_t need = 3u + (size_t)pkg_num * 11u;
        if(avail >= need) {
            return need;
        }
        return 0;
    }

    return 1;
}

static void tcp_handle_frame(const uint8_t *frame, size_t len)
{
    if(len >= 81 && frame[0] == 0xBE && frame[1] == 0xEF && frame[3] == 0x0E) {
        param_set_t ps;
        if(parse_readparam_response(frame, len, &ps) == 0) {
            JOB_LOCK();
            g_job_status.param_a_max       = ps.a_max;
            g_job_status.param_fly_enable  = ps.fly_enable;
            g_job_status.param_fly_gain    = ps.fly_gain;
            g_job_status.param_acc_enable  = ps.acc_enable;
            g_job_status.param_acc_delat   = ps.acc_delat;
            g_job_status.param_laser_start = ps.laser_start;
            g_job_status.param_laser_stop  = ps.laser_stop;
            g_job_status.param_read_valid  = true;
        }
        return;
    }

    if(len >= 19 && frame[0] == 0xBE && frame[1] == 0xEF &&
       frame[2] == 0x10 && frame[3] == 0x01) {
        tcp_response_t resp;
        if(parse_tcp_response(frame, len, &resp) == 0) {
            JOB_LOCK();
            g_job_status.cmd_cache_rate  = resp.cmd_rate;
            g_job_status.data_cache_rate = resp.data_rate;
            g_job_status.current_x       = resp.current_x;
            g_job_status.current_y       = resp.current_y;
            g_job_status.current_z       = resp.current_z;
            g_job_status.laser_state     = resp.laser_state;
            if(resp.flash_flag == 0x0D) {
                g_job_status.flash_saved     = 1;
                g_job_status.param_write_done = true;
            }
            job_status_sync_mcu_cmd_backpressure_locked();
        }
        if(s_rb) {
            job_status_update_ring();
        }
    }
}

static void tcp_drain_rx(void)
{
    uint8_t chunk[256];
    for(;;) {
        int n = tcp_recv_nonblock(chunk, sizeof(chunk));
        if(n <= 0) {
            break;
        }

        if(s_tcp_rx_accum_len + (size_t)n > sizeof(s_tcp_rx_accum)) {
            const size_t keep = sizeof(s_tcp_rx_accum) / 2;
            if(s_tcp_rx_accum_len > keep) {
                memmove(s_tcp_rx_accum, s_tcp_rx_accum + s_tcp_rx_accum_len - keep, keep);
                s_tcp_rx_accum_len = keep;
            }
        }
        memcpy(s_tcp_rx_accum + s_tcp_rx_accum_len, chunk, (size_t)n);
        s_tcp_rx_accum_len += (size_t)n;

        size_t pos = 0;
        while(pos < s_tcp_rx_accum_len) {
            const size_t avail = s_tcp_rx_accum_len - pos;
            const size_t flen = tcp_guess_frame_len(s_tcp_rx_accum + pos, avail);
            if(flen == 0) {
                break;
            }
            if(flen == 1) {
                pos++;
                continue;
            }
            tcp_handle_frame(s_tcp_rx_accum + pos, flen);
            pos += flen;
        }

        if(pos > 0) {
            memmove(s_tcp_rx_accum, s_tcp_rx_accum + pos, s_tcp_rx_accum_len - pos);
            s_tcp_rx_accum_len -= pos;
        }
    }
}

/* ---------- 串口辅助 ---------- */
static int serial_send_raw(const uint8_t *data, size_t len)
{
    if (s_serial_fd < 0 || !data || len == 0) return -1;
    ssize_t n = write(s_serial_fd, data, len);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int serial_recv_nonblock(uint8_t *buf, size_t buf_size)
{
    if (s_serial_fd < 0) return -1;
    ssize_t n = read(s_serial_fd, buf, buf_size);
    return (int)n;
}

static void link_thread_fn(void);

static void link_thread_start(void)
{
    if(s_link_run.load()) {
        return;
    }
    s_link_stop_req.store(false);
    s_link_run.store(true);
    if(s_link_th.joinable()) {
        s_link_th.join();
    }
    s_link_th = std::thread(link_thread_fn);
}

/** 处理队首换层标记；返回 true 表示本轮已处理且调用方应继续下一轮 */
static bool link_handle_layer_mark(bool *first_layer)
{
    rb_pkt_type_t ptype;
    if(!s_rb || !s_rb->peek_type(ptype) || ptype != RB_PKT_LAYER_MARK) {
        return false;
    }

    rb_packet_t dummy;
    s_rb->get(dummy, 50);

    if(s_enable_layer_change) {
        uint8_t sbuf[32];
        int slen;
        if(*first_layer) {
            slen = serial_build_print_start(s_layer_height_um, sbuf, sizeof(sbuf));
        } else {
            slen = serial_build_layer_done(s_layer_height_um, sbuf, sizeof(sbuf));
        }
        if(slen > 0) {
            serial_send_raw(sbuf, (size_t)slen);
        }

        s_waiting_layer.store(true);
        s_layer_handshake_done.store(false);
        *first_layer = false;
    }
    return true;
}

/** 从 Ring 组一批运动帧并 TCP 下发；返回本批包数，0 表示未发送 */
static int link_send_one_motion_batch(void)
{
    if(!s_rb || s_rb->empty()) {
        return 0;
    }

    motion_subpack_t pkts[kMaxMotionBatch];
    int n = 0;
    const int batch_lim = (s_batch_size > kMaxMotionBatch) ? kMaxMotionBatch : s_batch_size;
    if(batch_lim <= 0) {
        return 0;
    }

    for(int i = 0; i < batch_lim; i++) {
        rb_pkt_type_t ptype;
        if(!s_rb->peek_type(ptype)) {
            break;
        }
        if(ptype == RB_PKT_LAYER_MARK) {
            break;
        }
        if(ptype != RB_PKT_MOTION) {
            rb_packet_t discard;
            s_rb->get(discard, 5);
            break;
        }
        rb_packet_t rp;
        if(!s_rb->get(rp, 5)) {
            break;
        }
        pkts[n].cmd   = rp.data[0];
        pkts[n].x     = (uint16_t)(((uint16_t)rp.data[1] << 8) | rp.data[2]);
        pkts[n].y     = (uint16_t)(((uint16_t)rp.data[3] << 8) | rp.data[4]);
        pkts[n].z     = (uint16_t)(((uint16_t)rp.data[5] << 8) | rp.data[6]);
        pkts[n].speed = (uint16_t)(((uint16_t)rp.data[7] << 8) | rp.data[8]);
        pkts[n].ld    = (uint16_t)(((uint16_t)rp.data[9] << 8) | rp.data[10]);
        n++;
    }

    if(n <= 0) {
        return 0;
    }

    uint8_t frame[kMotionFrameSize];
    const int flen = build_motion_frame(pkts, n, frame, sizeof(frame));
    if(flen <= 0 || tcp_send_raw(frame, (size_t)flen) != 0) {
        return 0;
    }

    s_sent_count += n;
    {
        JOB_LOCK();
        g_job_status.sent_packets = s_sent_count.load();
    }
    job_status_update_ring();
    return n;
}

/**
 * 发送期尽量连续组帧；解析仍在进行时，队列低于低水位则暂缓（背压，等待 Ring 重新顶满）。
 */
static bool link_mcu_cmd_tx_hold_active(void)
{
    bool hold = false;
    JOB_LOCK();
    job_status_sync_mcu_cmd_backpressure_locked();
    hold = g_job_status.mcu_cmd_tx_hold;
    return hold;
}

static void link_drain_motion_queue(bool *first_layer)
{
    if(!s_is_sending.load() || s_is_paused.load() || !s_rb) {
        return;
    }

    const size_t cnt0 = s_rb->count();
    const size_t target = gcode_ring_target_fill();
    int burst_cap = LINK_SEND_BURST_MAX_BATCHES;
    if(cnt0 >= target) {
        burst_cap = LINK_SEND_BURST_ABOVE_TARGET;
    }

    int bursts = 0;
    while(bursts < burst_cap &&
          s_is_sending.load() && !s_is_paused.load() && !s_link_stop_req.load()) {

        if(s_waiting_layer.load()) {
            break;
        }

        const bool mcu_hold = link_mcu_cmd_tx_hold_active();

        if(link_handle_layer_mark(first_layer)) {
            bursts++;
            continue;
        }

        if(s_rb->empty()) {
            break;
        }

        /* 下位机 Cmd≥90%：仅停运动帧 TCP，解析与换层标记照常 */
        if(mcu_hold) {
            break;
        }

        if(gcode_is_parsing() && !gcode_is_file_read_exhausted()) {
            const size_t cnt = s_rb->count();
            if(cnt < gcode_ring_consumer_low_water()) {
                break;
            }
        }

        if(link_send_one_motion_batch() <= 0) {
            break;
        }
        bursts++;
    }
}

static void link_thread_stop(void)
{
    if(!s_link_run.load() && !s_link_th.joinable()) {
        return;
    }
    s_link_stop_req.store(true);
    s_link_run.store(false);
    if(s_link_th.joinable()) {
        s_link_th.join();
    }
    s_link_stop_req.store(false);
}

/* ---------- TCP 连接后的通信线程（心跳 + 收应答 + 可选发运动帧） ---------- */
static void link_thread_fn(void)
{
    using clock = std::chrono::steady_clock;
    auto next_hb = clock::now();
    uint8_t rx[256];
    uint8_t srx[64];
    s_sent_count.store(0);

    while(s_link_run.load() && !s_link_stop_req.load()) {
        auto now = clock::now();

        /* 1. 心跳：TCP 已连接即周期下发 */
        if(now >= next_hb && s_tcp_fd >= 0) {
            uint8_t hb[64];
            int hblen = build_heartbeat_frame(hb, sizeof(hb));
            if(hblen > 0) {
                tcp_send_raw(hb, (size_t)hblen);
            }
            next_hb = now + std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS);
        }

        /* 2. 接收 TCP（粘包拆帧：19B 常态 / 81B 读参） */
        tcp_drain_rx();
        (void)rx;

        /* 3. 非阻塞接收串口数据 */
        int srn = serial_recv_nonblock(srx, sizeof(srx));
        if (srn > 0) {
            serial_event_t evt = serial_parse_frame(srx, srn);
            if (evt == SERIAL_EVT_PRINT_READY) {
                s_layer_handshake_done.store(true);
            }
        }

        /* 4. 换层握手完成 → 继续发运动帧 */
        if(s_waiting_layer.load() && s_layer_handshake_done.load()) {
            s_waiting_layer.store(false);
            s_layer_handshake_done.store(false);
        }

        /* 5. 运动帧发送：连发多批；解析进行时低于低水位则背压等待 Ring 顶满 */
        link_drain_motion_queue(&s_link_first_layer);

        /* 6. 轮询：高于目标水位时亦按 Intv 节拍，避免 Ring 被抽至 ~1% */
        int sleep_ms = s_interval_ms;
        if(s_is_sending.load() && !s_is_paused.load() && s_rb) {
            const size_t cnt = s_rb->count();
            const size_t resume = gcode_ring_consumer_low_water();
            if(gcode_is_parsing() && cnt < resume) {
                sleep_ms = s_interval_ms > 0 ? s_interval_ms : 1;
            }
        }
        if(sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

}

/* ---------- 公共接口 ---------- */

void ctrl_backend_init(void)
{
    gcode_parse_gains_set_defaults();

    s_tcp_fd = -1;
    s_serial_fd = -1;
    s_rb = nullptr;
    s_link_run.store(false);
    s_link_stop_req.store(false);
    s_is_sending.store(false);
    s_is_paused.store(false);
    s_sent_count.store(0);
    s_waiting_layer.store(false);
    s_layer_handshake_done.store(false);
}

void ctrl_backend_deinit(void)
{
    ctrl_stop_sending();
    ctrl_tcp_disconnect();
    ctrl_serial_close();
    link_thread_stop();
}

/* TCP */
int ctrl_tcp_connect(const char *ip, int port)
{
    if (s_tcp_fd >= 0) ctrl_tcp_disconnect();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd); return -2;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -3;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    s_tcp_fd = fd;
    {
        JOB_LOCK();
        g_job_status.tcp_connected = true;
    }

    link_thread_start();

    /* 连接后读一次板参（应答在下次 ETH_report 中返回 81B 帧） */
    ctrl_request_read_params();
    return 0;
}

void ctrl_tcp_disconnect(void)
{
    ctrl_stop_sending();
    link_thread_stop();
    s_tcp_rx_accum_len = 0;

    if(s_tcp_fd >= 0) {
        close(s_tcp_fd);
        s_tcp_fd = -1;
    }
    {
        JOB_LOCK();
        g_job_status.tcp_connected = false;
        g_job_status.cmd_cache_rate  = 0;
        g_job_status.data_cache_rate = 0;
        g_job_status.param_read_valid = false;
        g_job_status.param_write_done = false;
    }
}

int ctrl_tcp_is_connected(void) { return s_tcp_fd >= 0; }

/* Serial */
int ctrl_serial_open(const char *device, int baud)
{
    if (s_serial_fd >= 0) ctrl_serial_close();

    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }

    speed_t spd = B115200;
    switch (baud) {
        case 9600:   spd = B9600; break;
        case 19200:  spd = B19200; break;
        case 38400:  spd = B38400; break;
        case 57600:  spd = B57600; break;
        case 115200: spd = B115200; break;
        case 230400: spd = B230400; break;
        case 460800: spd = B460800; break;
        default:     spd = B115200; break;
    }
    cfsetospeed(&tty, spd);
    cfsetispeed(&tty, spd);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);

    s_serial_fd = fd;
    {
        JOB_LOCK();
        g_job_status.serial_connected = true;
    }
    return 0;
}

void ctrl_serial_close(void)
{
    if (s_serial_fd >= 0) {
        close(s_serial_fd);
        s_serial_fd = -1;
    }
    {
        JOB_LOCK();
        g_job_status.serial_connected = false;
    }
}

int ctrl_serial_is_open(void) { return s_serial_fd >= 0; }

/* RingBuffer */
void ctrl_set_ring_buffer(RingBuffer *rb) { s_rb = rb; }

/* 发送控制 */
void ctrl_start_sending(void)
{
    if(s_tcp_fd < 0) {
        return;
    }
    if(!s_link_run.load()) {
        link_thread_start();
    }

    s_is_sending.store(true);
    s_is_paused.store(false);
    s_sent_count.store(0);
    s_link_first_layer = true;

    {
        JOB_LOCK();
        g_job_status.is_sending = true;
        g_job_status.sent_packets = 0;
    }

    /* 若启用换层且串口已连接，先发打印请求 */
    if(s_enable_layer_change && s_serial_fd >= 0) {
        uint8_t sbuf[32];
        int slen = serial_build_print_request(sbuf, sizeof(sbuf));
        if(slen > 0) {
            serial_send_raw(sbuf, (size_t)slen);
        }
        s_waiting_layer.store(true);
        s_layer_handshake_done.store(false);
    }
}

void ctrl_stop_sending(void)
{
    s_is_sending.store(false);
    s_is_paused.store(false);
    s_waiting_layer.store(false);
    s_layer_handshake_done.store(false);
    {
        JOB_LOCK();
        g_job_status.is_sending = false;
        g_job_status.mcu_cmd_tx_hold = false;
    }
}

void ctrl_pause_sending(void) { s_is_paused.store(true); }
void ctrl_resume_sending(void) { s_is_paused.store(false); }
int  ctrl_is_sending(void) { return s_is_sending.load(); }
int  ctrl_is_paused(void) { return s_is_paused.load(); }

/* 复位 */
void ctrl_send_reset(void)
{
    uint8_t frame[64];
    int flen = build_reset_frame(frame, sizeof(frame));
    if(flen > 0) {
        tcp_send_raw(frame, (size_t)flen);
    }

    s_sent_count.store(0);
    s_is_sending.store(false);
    s_is_paused.store(false);
    s_waiting_layer.store(false);
    s_layer_handshake_done.store(false);
    s_link_first_layer = true;

    {
        JOB_LOCK();
        g_job_status.is_sending = false;
        g_job_status.sent_packets = 0;
        g_job_status.mcu_cmd_tx_hold = false;
        /* 复位后下位机 Cmd 队列会清空；旧占用率若保留会导致再次 Start 时背压误判 */
        g_job_status.cmd_cache_rate  = 0;
        g_job_status.data_cache_rate = 0;
    }

    /* 仅清空待发队列；不重新解析 G-code（需新参数时在 Files 重新 Open） */
    if(s_rb) {
        s_rb->clear();
    }
    /* 通信线程保持运行，心跳继续 */
}

/* 参数设置 */
void ctrl_set_batch_size(int n)
{
    s_batch_size = (n > 0 && n <= kMaxMotionBatch) ? n : kMaxMotionBatch;
    JOB_LOCK();
    g_job_status.batch_size = s_batch_size;
}

void ctrl_set_interval_ms(int ms)
{
    s_interval_ms = (ms > 0) ? ms : 3;
    JOB_LOCK();
    g_job_status.interval_ms = s_interval_ms;
}

void ctrl_set_layer_height_um(uint16_t um)
{
    s_layer_height_um = um;
    JOB_LOCK();
    g_job_status.layer_height_um = um;
}

void ctrl_set_enable_layer_change(int en)
{
    s_enable_layer_change = en ? 1 : 0;
    JOB_LOCK();
    g_job_status.enable_layer_change = en ? true : false;
}

/* 板参数读写 */
void ctrl_request_read_params(void)
{
    if(s_tcp_fd < 0) {
        return;
    }
    uint8_t frame[64];
    int flen = build_readparam_frame(frame, sizeof(frame));
    if(flen > 0) {
        tcp_send_raw(frame, (size_t)flen);
    }
}

static int tcp_send_writeparam(uint8_t obj, uint16_t p0, uint16_t p1,
                               uint16_t p2, uint16_t p3)
{
    uint8_t frame[64];
    const int flen = build_writeparam_frame(obj, p0, p1, p2, p3, frame, sizeof(frame));
    if(flen <= 0) {
        return -1;
    }
    return tcp_send_raw(frame, (size_t)flen);
}

int ctrl_commit_params(float a_max, uint8_t fly_en, float fly_gain,
                       uint8_t acc_en, float acc_delat,
                       int32_t laser_start, int32_t laser_stop)
{
    if(s_tcp_fd < 0) {
        return -1;
    }

    int ret = 0;
    /* 下位机：运动模式(control_sta bit0=1)时忽略 0x0D 写参 */
    if(s_is_sending.load()) {
        ctrl_stop_sending();
        ret = -2;
    }

    uint16_t p0 = 0, p1 = 0;

    {
        JOB_LOCK();
        g_job_status.param_write_done = false;
        g_job_status.flash_saved = 0;
    }

    auto pause_between = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    };

    float_to_u16_pair(a_max / 100000000.0f, &p0, &p1);
    if(tcp_send_writeparam(0, p0, p1, 0, 0) != 0) return -1;
    pause_between();

    if(tcp_send_writeparam(1, fly_en ? 1u : 0u, 0, 0, 0) != 0) return -1;
    pause_between();

    float_to_u16_pair(fly_gain, &p0, &p1);
    if(tcp_send_writeparam(2, p0, p1, 0, 0) != 0) return -1;
    pause_between();

    if(tcp_send_writeparam(3, acc_en ? 1u : 0u, 0, 0, 0) != 0) return -1;
    pause_between();

    float_to_u16_pair(acc_delat, &p0, &p1);
    if(tcp_send_writeparam(4, p0, p1, 0, 0) != 0) return -1;
    pause_between();

    p0 = (uint16_t)((uint32_t)laser_start & 0xFFFFu);
    p1 = (uint16_t)(((uint32_t)laser_start >> 16) & 0xFFFFu);
    if(tcp_send_writeparam(5, p0, p1, 0, 0) != 0) return -1;
    pause_between();

    p0 = (uint16_t)((uint32_t)laser_stop & 0xFFFFu);
    p1 = (uint16_t)(((uint32_t)laser_stop >> 16) & 0xFFFFu);
    if(tcp_send_writeparam(6, p0, p1, 0, 0) != 0) return -1;

    /* 写完后读回验证；Flash 保存标志在后续 19B 心跳里 byte[8]=0x0D */
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ctrl_request_read_params();

    return ret;
}

int ctrl_get_sent_count(void) { return s_sent_count.load(); }
