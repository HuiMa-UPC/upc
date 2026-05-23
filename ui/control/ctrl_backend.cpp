#include "ctrl_backend.hpp"
#include "protocol_mcu.hpp"
#include "job_status.hpp"
#include "gcode_parse_gains.hpp"
#include "gcode_worker.hpp"
#include "page_files.hpp"
#include "pipeline_tune.hpp"

#include <sys/socket.h>
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
#include <deque>

/* ---------- 内部状态 ---------- */
static int              s_tcp_fd = -1;
static int              s_serial_fd = -1;
static RingBuffer      *s_rb = nullptr;
static std::thread      s_link_th;
static std::atomic<bool> s_link_run{false};
static std::atomic<bool> s_link_stop_req{false};

/** TCP 连接后常态心跳间隔（与 UI「Intv(ms)」无关，后者仅控制发送线程轮询周期） */
static constexpr int HEARTBEAT_INTERVAL_MS = 200;
static std::atomic<bool> s_is_sending{false};
static std::atomic<bool> s_is_paused{false};
static std::atomic<int>  s_sent_count{0};

static int              s_batch_size = 100;
static int              s_interval_ms = 3;
static uint16_t         s_layer_height_um = 100;
static int              s_enable_layer_change = 0;

static std::atomic<bool> s_waiting_layer{false};
static std::atomic<bool> s_layer_handshake_done{false};
/** 是否已收到至少一帧 19B 常态应答（用于启动期限速） */
static std::atomic<bool> s_mcu_eth_report_seen{false};
/** 换层会话：首层发 PRINT_START，Reset/再次 Start 须置 true */
static bool s_link_first_layer = true;

static uint8_t          s_tcp_rx_accum[1024];
static size_t           s_tcp_rx_accum_len = 0;
static std::mutex       s_tcp_send_mtx;

static uint8_t          s_serial_rx_accum[256];
static size_t           s_serial_rx_accum_len = 0;

/** 单包运动子帧 11B，batch 最大 100 → 1103B 帧缓冲 */
static constexpr int    kMaxMotionBatch   = 100;
static constexpr size_t kMotionFrameSize = 3u + 11u * kMaxMotionBatch;

/** ② m_tcpStageQueue：① Ring → Stage → 组帧 TCP（对齐 Qt refillTcpStageFromWorker） */
static std::deque<rb_packet_t> s_tcp_stage;
static std::mutex              s_tcp_stage_mtx;
/** M3000：已遇换层标记，等待下位机 cmd/data 均为 0 后再发串口（对齐 Qt enterLayerWait） */
static std::atomic<bool>       s_layer_drain_pending{false};

static void tcp_stage_clear(void)
{
    std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
    s_tcp_stage.clear();
}

static size_t tcp_stage_count(void)
{
    std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
    return s_tcp_stage.size();
}

/** ① → ②：每轮限量搬运，避免 Ring 预填一次灌满 Stage */
static void refill_tcp_stage_from_ring(void)
{
    if(!s_rb) {
        return;
    }

    size_t moved = 0;
    while(moved < REFILL_STAGE_MAX_PACKETS) {
        {
            std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
            if(s_tcp_stage.size() >= TCP_STAGE_MAX_PACKETS) {
                break;
            }
        }

        rb_pkt_type_t ptype;
        if(!s_rb->peek_type(ptype)) {
            break;
        }

        rb_packet_t pkt;
        if(!s_rb->get(pkt, 5)) {
            break;
        }

        std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
        s_tcp_stage.push_back(pkt);
        moved++;
    }

    job_status_update_ring();
}

static bool tcp_stage_peek_type(rb_pkt_type_t *ptype)
{
    if(!ptype) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
    if(s_tcp_stage.empty()) {
        return false;
    }
    *ptype = static_cast<rb_pkt_type_t>(s_tcp_stage.front().type);
    return true;
}

static bool tcp_stage_pop_front(rb_packet_t *pkt)
{
    if(!pkt) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
    if(s_tcp_stage.empty()) {
        return false;
    }
    *pkt = s_tcp_stage.front();
    s_tcp_stage.pop_front();
    return true;
}

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
        /* 心跳 / 运动帧下发后下位机均回 19B 常态应答（含 cmd/data/坐标/激光） */
        if(parse_tcp_response(frame, len, &resp) == 0) {
            s_mcu_eth_report_seen.store(true);
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

/** MCU→SOC 串口帧总长；0=数据不足，1=跳过一个坏字节 */
static size_t serial_guess_frame_len(const uint8_t *buf, size_t avail)
{
    if(avail < 7) {
        return 0;
    }
    if(buf[0] != 0x5A || buf[1] != 0xA5) {
        return 1;
    }
    if(buf[2] != 0xBB) {
        return 1;
    }

    const uint8_t payload_len = buf[6];
    const size_t frame_len = 9u + (size_t)payload_len;
    if(avail < frame_len) {
        return 0;
    }
    return frame_len;
}

static void serial_handle_frame(const uint8_t *frame, size_t len)
{
    serial_event_t evt = serial_parse_frame(frame, len);
    if(evt == SERIAL_EVT_PRINT_READY) {
        s_layer_handshake_done.store(true);
    }
}

static void serial_drain_rx(void)
{
    uint8_t chunk[64];
    for(;;) {
        const int n = serial_recv_nonblock(chunk, sizeof(chunk));
        if(n <= 0) {
            break;
        }

        if(s_serial_rx_accum_len + (size_t)n > sizeof(s_serial_rx_accum)) {
            const size_t keep = sizeof(s_serial_rx_accum) / 2;
            if(s_serial_rx_accum_len > keep) {
                memmove(s_serial_rx_accum,
                        s_serial_rx_accum + s_serial_rx_accum_len - keep,
                        keep);
                s_serial_rx_accum_len = keep;
            }
        }
        memcpy(s_serial_rx_accum + s_serial_rx_accum_len, chunk, (size_t)n);
        s_serial_rx_accum_len += (size_t)n;

        size_t pos = 0;
        while(pos < s_serial_rx_accum_len) {
            const size_t avail = s_serial_rx_accum_len - pos;
            const size_t flen = serial_guess_frame_len(s_serial_rx_accum + pos, avail);
            if(flen == 0) {
                break;
            }
            if(flen == 1) {
                pos++;
                continue;
            }
            serial_handle_frame(s_serial_rx_accum + pos, flen);
            pos += flen;
        }

        if(pos > 0) {
            memmove(s_serial_rx_accum, s_serial_rx_accum + pos, s_serial_rx_accum_len - pos);
            s_serial_rx_accum_len -= pos;
        }
    }
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

/** 遇到 M3000：从 Stage 取出标记并进入换层等待（对齐 Qt flush + enterLayerWait） */
static bool link_handle_layer_mark(bool *first_layer)
{
    rb_pkt_type_t ptype;
    if(!tcp_stage_peek_type(&ptype) || ptype != RB_PKT_LAYER_MARK) {
        return false;
    }

    rb_packet_t dummy;
    if(!tcp_stage_pop_front(&dummy)) {
        return false;
    }

    s_waiting_layer.store(true);
    s_layer_handshake_done.store(false);

    if(s_enable_layer_change) {
        s_layer_drain_pending.store(true);
    } else {
        s_layer_drain_pending.store(false);
        s_waiting_layer.store(false);
    }

    (void)first_layer;
    return true;
}

/** cmd/data 均为 0 后下发串口换层（对齐 Qt M3000 流程） */
static void link_try_layer_serial_dispatch(bool *first_layer)
{
    if(!s_waiting_layer.load() || !s_layer_drain_pending.load()) {
        return;
    }
    if(!s_enable_layer_change || s_serial_fd < 0) {
        s_layer_drain_pending.store(false);
        s_waiting_layer.store(false);
        return;
    }

    uint16_t cmd_snap = 0;
    uint16_t data_snap = 0;
    {
        JOB_LOCK();
        cmd_snap  = g_job_status.cmd_cache_rate;
        data_snap = g_job_status.data_cache_rate;
    }

    if(cmd_snap > MCU_LAYER_DRAIN_MAX_RATE || data_snap > MCU_LAYER_DRAIN_MAX_RATE) {
        return;
    }

    uint8_t sbuf[32];
    int slen = 0;
    if(*first_layer) {
        slen = serial_build_print_start(s_layer_height_um, sbuf, sizeof(sbuf));
    } else {
        slen = serial_build_layer_done(s_layer_height_um, sbuf, sizeof(sbuf));
    }
    if(slen > 0) {
        serial_send_raw(sbuf, (size_t)slen);
    }

    s_layer_drain_pending.store(false);
    *first_layer = false;
}

/** 从 Stage 组一批运动帧并 TCP 下发；返回本批包数，0 表示未发送 */
static int link_send_one_motion_batch(void)
{
    if(tcp_stage_count() == 0) {
        return 0;
    }

    rb_packet_t staged[kMaxMotionBatch];
    int n = 0;
    const int batch_lim = (s_batch_size > kMaxMotionBatch) ? kMaxMotionBatch : s_batch_size;
    if(batch_lim <= 0) {
        return 0;
    }

    for(int i = 0; i < batch_lim; i++) {
        rb_pkt_type_t ptype;
        if(!tcp_stage_peek_type(&ptype)) {
            break;
        }
        if(ptype == RB_PKT_LAYER_MARK) {
            break;
        }
        if(ptype != RB_PKT_MOTION) {
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

    uint8_t frame[kMotionFrameSize];
    uint8_t payload[kMaxMotionBatch * 11];
    for(int i = 0; i < n; i++) {
        memcpy(&payload[i * 11], staged[i].data, 11);
    }
    const int flen = build_motion_frame(payload, n, frame, sizeof(frame));
    if(flen <= 0 || tcp_send_raw(frame, (size_t)flen) != 0) {
        /* 发送失败：包已 pop，须回灌 Stage 队首，避免静默丢包 */
        std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
        for(int i = n - 1; i >= 0; i--) {
            s_tcp_stage.push_front(staged[i]);
        }
        return 0;
    }

    s_sent_count += n;
    {
        JOB_LOCK();
        g_job_status.sent_packets = s_sent_count.load();
    }
    return n;
}

/**
 * Qt 发送主循环：refillStage → consumeBatch → flushPacketBatch。
 * 背压仅看下位机 cmd/data 与 Cmd hold，不按 Ring 水位停 TCP。
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
    if(!s_is_sending.load() || s_is_paused.load()) {
        return;
    }

    refill_tcp_stage_from_ring();

    link_try_layer_serial_dispatch(first_layer);

    if(s_waiting_layer.load()) {
        return;
    }

    uint16_t cmd_snap = 0;
    uint16_t data_snap = 0;
    {
        JOB_LOCK();
        cmd_snap  = g_job_status.cmd_cache_rate;
        data_snap = g_job_status.data_cache_rate;
    }

    if(link_mcu_cmd_tx_hold_active()) {
        return;
    }

    int burst_cap = LINK_SEND_BURST_DEFAULT;
    if(!s_mcu_eth_report_seen.load()) {
        /* Reset/Start 后尚未收到新 19B：保守发送，勿按本地 data=0 猛灌 */
        burst_cap = LINK_SEND_BURST_UNTIL_MCU_REPORT;
    } else if(cmd_snap >= MCU_CMD_BACKPRESSURE_ON_RATE) {
        burst_cap = LINK_SEND_BURST_UNTIL_MCU_REPORT;
    } else if(cmd_snap >= MCU_CMD_BACKPRESSURE_WARN_RATE) {
        burst_cap = LINK_SEND_BURST_CMD_PRESSURE;
    } else if(data_snap < MCU_DATA_FEED_MIN_RATE &&
              cmd_snap >= MCU_DATA_FEED_MIN_CMD_RATE &&
              cmd_snap < MCU_CMD_BACKPRESSURE_WARN_RATE) {
        burst_cap = LINK_SEND_BURST_MAX_BATCHES;
    }

    {
        const size_t host_q = (s_rb ? s_rb->count() : 0) + tcp_stage_count();
        if(gcode_is_parsing() && host_q < RING_TCP_STARVE_MIN_PKTS &&
           burst_cap > LINK_SEND_BURST_UNTIL_MCU_REPORT) {
            burst_cap = LINK_SEND_BURST_UNTIL_MCU_REPORT;
        }
    }

    int bursts = 0;
    while(bursts < burst_cap &&
          s_is_sending.load() && !s_is_paused.load() && !s_link_stop_req.load()) {

        if(s_waiting_layer.load()) {
            break;
        }

        if(link_mcu_cmd_tx_hold_active()) {
            break;
        }

        if(link_handle_layer_mark(first_layer)) {
            link_try_layer_serial_dispatch(first_layer);
            break;
        }

        if(tcp_stage_count() == 0) {
            break;
        }

        if(link_send_one_motion_batch() <= 0) {
            break;
        }
        bursts++;
    }
}

/** 文件仍打开且未在解析时，从头预填 Ring（Reset / 打印完成共用） */
static void ctrl_prefill_gcode_ring(void)
{
    if(!s_rb || !gcode_is_file_open() || gcode_is_parsing()) {
        return;
    }
    gcode_start_parse(s_rb);
    page_files_notify_parse_restarted();
}

/** Ring/Stage 已空且解析读完时自动结束发送 */
static void link_try_finish_sending(void)
{
    if(!s_is_sending.load() || s_is_paused.load() || s_waiting_layer.load()) {
        return;
    }
    if(tcp_stage_count() > 0) {
        return;
    }
    if(s_rb && !s_rb->empty()) {
        return;
    }
    if(gcode_is_parsing()) {
        return;
    }
    if(!gcode_is_file_read_exhausted()) {
        return;
    }

    s_is_sending.store(false);
    s_is_paused.store(false);
    {
        JOB_LOCK();
        g_job_status.is_sending = false;
        g_job_status.mcu_cmd_tx_hold = false;
        g_job_status.job_print_completed = true;
    }

    ctrl_prefill_gcode_ring();
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
    s_sent_count.store(0);

    while(s_link_run.load() && !s_link_stop_req.load()) {
        auto now = clock::now();

        /* 1. 先收应答（含心跳 19B：更新 cmd/data/X/Y/Z/Laser） */
        tcp_drain_rx();

        /* 2. 未在发运动流时周期心跳（暂停 / 空闲 / 仅连接） */
        const bool motion_stream_active =
            s_is_sending.load() && !s_is_paused.load() && !s_waiting_layer.load();
        if(!motion_stream_active && now >= next_hb && s_tcp_fd >= 0) {
            uint8_t hb[64];
            int hblen = build_heartbeat_frame(hb, sizeof(hb));
            if(hblen > 0) {
                tcp_send_raw(hb, (size_t)hblen);
            }
            next_hb = now + std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS);
            tcp_drain_rx();
        }

        /* 3. 非阻塞接收串口数据（粘包拆帧） */
        serial_drain_rx();

        /* 4. 换层握手完成 → 继续发运动帧；首层在 PRINT_READY 后发 PRINT_START */
        if(s_waiting_layer.load() && s_layer_handshake_done.load()) {
            if(s_enable_layer_change && s_serial_fd >= 0 && s_link_first_layer) {
                uint8_t sbuf[32];
                const int slen = serial_build_print_start(s_layer_height_um, sbuf, sizeof(sbuf));
                if(slen > 0) {
                    serial_send_raw(sbuf, (size_t)slen);
                }
                s_link_first_layer = false;
            }
            s_waiting_layer.store(false);
            s_layer_handshake_done.store(false);
        }

        /* 5. 运动帧：①→② refill + ②→TCP consume（对齐 Qt 定时器周期） */
        link_drain_motion_queue(&s_link_first_layer);
        link_try_finish_sending();

        /* 6. 轮询周期 = Intv(ms) */
        if(s_interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(s_interval_ms));
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
    s_mcu_eth_report_seen.store(false);
    s_layer_drain_pending.store(false);
    s_serial_rx_accum_len = 0;
    tcp_stage_clear();

    JOB_LOCK();
    g_job_status.batch_size           = 100;
    g_job_status.interval_ms          = 3;
    g_job_status.enable_layer_change  = false;
    g_job_status.job_print_completed  = false;
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
    s_mcu_eth_report_seen.store(false);
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
    s_mcu_eth_report_seen.store(false);
    tcp_stage_clear();

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
    s_serial_rx_accum_len = 0;
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

    const int batch = s_batch_size > 0 ? s_batch_size : 100;
    if(s_rb && s_rb->count() < (size_t)batch) {
        ctrl_prefill_gcode_ring();
        return;
    }

    if(!s_link_run.load()) {
        link_thread_start();
    }

    s_is_sending.store(true);
    s_is_paused.store(false);
    s_sent_count.store(0);
    s_link_first_layer = true;
    s_mcu_eth_report_seen.store(false);
    tcp_stage_clear();

    {
        JOB_LOCK();
        g_job_status.is_sending = true;
        g_job_status.sent_packets = 0;
        g_job_status.mcu_cmd_tx_hold = false;
        g_job_status.job_print_completed = false;
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
    s_layer_drain_pending.store(false);
    tcp_stage_clear();
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
    s_layer_drain_pending.store(false);
    s_link_first_layer = true;
    s_mcu_eth_report_seen.store(false);
    tcp_stage_clear();

    {
        JOB_LOCK();
        g_job_status.is_sending = false;
        g_job_status.sent_packets = 0;
        g_job_status.mcu_cmd_tx_hold = false;
        g_job_status.cmd_cache_rate  = 0;
        g_job_status.data_cache_rate = 0;
    }

    /* 清空 ①②；始终从头重新解析 G-code 预填 Ring */
    if(s_rb) {
        s_rb->clear();
    }
    if(s_rb) {
        ctrl_prefill_gcode_ring();
    }
    {
        JOB_LOCK();
        g_job_status.job_print_completed = false;
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
