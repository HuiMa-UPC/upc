#include "ctrl_backend.hpp"
#include "protocol_mcu.hpp"
#include "job_status.hpp"
#include "gcode_parse.hpp"
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
static constexpr int HEARTBEAT_INTERVAL_MS = 150;
static std::atomic<bool> s_is_sending{false};
static std::atomic<bool> s_is_paused{false};
static std::atomic<int>  s_sent_count{0};

static int              s_batch_size = 100;
static int              s_interval_ms = 3;
static uint16_t         s_layer_height_um = 100;
static int              s_enable_layer_change = 0;

static std::atomic<bool> s_waiting_layer{false};
/** 串口换层握手阶段（见 serial_handshake_phase_t） */
static std::atomic<int>  s_serial_handshake_phase{0};
static std::atomic<bool> s_serial_evt_condition_ok{false};
static std::atomic<bool> s_serial_evt_print_ready{false};

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

/** 串口自动打印握手阶段 */
typedef enum {
    SERIAL_HS_NONE = 0,
    /** 已发 PRINT_REQUEST，等待「打印条件满足」 */
    SERIAL_HS_WAIT_CONDITION_OK,
    /** 已发 PRINT_START / LAYER_DONE，等待「打印准备完成」 */
    SERIAL_HS_WAIT_PRINT_READY,
} serial_handshake_phase_t;

static void serial_handshake_reset(void)
{
    s_serial_handshake_phase.store(SERIAL_HS_NONE);
    s_serial_evt_condition_ok.store(false);
    s_serial_evt_print_ready.store(false);
}
/** Start 已点击但 Ring 未满：预填完成后自动开始下发 */
static std::atomic<bool>       s_start_pending{false};

static void ctrl_begin_sending(void);
static void ctrl_try_complete_pending_start(void);

using frame_guess_fn  = size_t (*)(const uint8_t *, size_t);
using frame_handle_fn = void (*)(const uint8_t *, size_t);

static int  serial_send_raw(const uint8_t *data, size_t len);
static void tcp_stage_clear(void);

static void drain_framed_rx(int (*recv_fn)(uint8_t *, size_t),
                            uint8_t *chunk, size_t chunk_sz,
                            uint8_t *accum, size_t accum_cap, size_t *accum_len,
                            frame_guess_fn guess, frame_handle_fn handle)
{
    for(;;) {
        const int n = recv_fn(chunk, chunk_sz);
        if(n <= 0) {
            break;
        }

        if(*accum_len + (size_t)n > accum_cap) {
            const size_t keep = accum_cap / 2;
            if(*accum_len > keep) {
                memmove(accum, accum + *accum_len - keep, keep);
                *accum_len = keep;
            }
        }
        memcpy(accum + *accum_len, chunk, (size_t)n);
        *accum_len += (size_t)n;

        size_t pos = 0;
        while(pos < *accum_len) {
            const size_t avail = *accum_len - pos;
            const size_t flen = guess(accum + pos, avail);
            if(flen == 0) {
                break;
            }
            if(flen == 1) {
                pos++;
                continue;
            }
            handle(accum + pos, flen);
            pos += flen;
        }

        if(pos > 0) {
            memmove(accum, accum + pos, *accum_len - pos);
            *accum_len -= pos;
        }
    }
}

static void serial_send_buf(const uint8_t *buf, int len)
{
    if(len > 0) {
        serial_send_raw(buf, (size_t)len);
    }
}

static void job_status_get_mcu_rates(uint16_t *cmd, uint16_t *data)
{
    JOB_LOCK();
    if(cmd) {
        *cmd = g_job_status.cmd_cache_rate;
    }
    if(data) {
        *data = g_job_status.data_cache_rate;
    }
}

static int job_total_packets(void)
{
    JOB_LOCK();
    return g_job_status.total_packets;
}

static void ctrl_notify_ui(void)
{
    job_status_ui_bump();
}

/** 停止运动下发相关状态（不关 TCP/串口） */
static void ctrl_reset_motion_job_state(bool clear_start_pending)
{
    if(clear_start_pending) {
        s_start_pending.store(false);
    }
    s_is_sending.store(false);
    s_is_paused.store(false);
    s_waiting_layer.store(false);
    s_layer_drain_pending.store(false);
    serial_handshake_reset();
    tcp_stage_clear();
    JOB_LOCK();
    g_job_status.is_sending = false;
    g_job_status.mcu_cmd_tx_hold = false;
    ctrl_notify_ui();
}

/** 清空 TCP Stage 二级队列（线程安全） */
static void tcp_stage_clear(void)
{
    std::lock_guard<std::mutex> lock(s_tcp_stage_mtx);
    s_tcp_stage.clear();
}

/** @return Stage 队列当前包数（线程安全） */
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

/**
 * @brief 偷看 Stage 队首包类型（不弹出）
 * @return true 成功，false 队列为空
 */
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

/**
 * @brief 弹出 Stage 队首包
 * @return true 成功，false 队列为空
 */
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
/** @brief 通过 TCP socket 发送原始数据（线程安全锁） */
static int tcp_send_raw(const uint8_t *data, size_t len)
{
    if(s_tcp_fd < 0 || !data || len == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(s_tcp_send_mtx);
    const ssize_t n = send(s_tcp_fd, data, len, 0);
    return (n == (ssize_t)len) ? 0 : -1;
}

/** @brief 非阻塞接收 TCP 数据 */
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

/**
 * @brief 解析并处理一帧完整的 TCP 应答
 *
 * 支持两种帧：
 * - 81B 读参应答：解析板参数并存入 job_status
 * - 19B 常态/心跳应答：更新 cmd/data/坐标/激光/背压状态
 */
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
            job_status_ui_bump();
        }
        return;
    }

    if(len >= 19 && frame[0] == 0xBE && frame[1] == 0xEF &&
       frame[2] == 0x10 && frame[3] == 0x01) {
        tcp_response_t resp;
        /* 心跳 / 运动帧下发后下位机均回 19B 常态应答（含 cmd/data/坐标/激光） */
        if(parse_tcp_response(frame, len, &resp) == 0) {
            JOB_LOCK();
            g_job_status.cmd_cache_rate  = resp.cmd_rate;
            g_job_status.data_cache_rate = resp.data_rate;
            g_job_status.laser_state     = resp.laser_state;
            if(resp.flash_flag == 0x0D) {
                g_job_status.flash_saved     = 1;
            }
            job_status_sync_mcu_cmd_backpressure_locked();
            job_status_apply_mcu_position_locked(resp.current_x, resp.current_y, resp.current_z);
        }
        if(s_rb) {
            job_status_update_ring();
        }
    }
}

static void tcp_drain_rx(void)
{
    uint8_t chunk[256];
    drain_framed_rx(tcp_recv_nonblock, chunk, sizeof(chunk),
                    s_tcp_rx_accum, sizeof(s_tcp_rx_accum), &s_tcp_rx_accum_len,
                    tcp_guess_frame_len, tcp_handle_frame);
}

/* ---------- 串口辅助 ---------- */
/** @brief 通过串口发送原始数据 */
static int serial_send_raw(const uint8_t *data, size_t len)
{
    if (s_serial_fd < 0 || !data || len == 0) return -1;
    ssize_t n = write(s_serial_fd, data, len);
    return (n == (ssize_t)len) ? 0 : -1;
}

/** @brief 非阻塞读取串口数据 */
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
    const size_t frame_len = 8u + (size_t)payload_len;
    if(avail < frame_len) {
        return 0;
    }
    return frame_len;
}

/** @brief 解析串口帧，记录打印条件满足 / 打印准备完成 */
static void serial_handle_frame(const uint8_t *frame, size_t len)
{
    const serial_event_t evt = serial_parse_frame(frame, len);
    switch(evt) {
    case SERIAL_EVT_PRINT_CONDITION_OK:
        s_serial_evt_condition_ok.store(true);
        break;
    case SERIAL_EVT_PRINT_READY:
        s_serial_evt_print_ready.store(true);
        break;
    default:
        break;
    }
}

/**
 * @brief 根据串口上报推进握手：首层 REQUEST→条件满足→START→准备完成；换层 LAYER_DONE→准备完成
 */
static void link_process_serial_handshake(void)
{
    if(!s_waiting_layer.load() || !s_enable_layer_change || s_serial_fd < 0) {
        return;
    }

    const int phase = s_serial_handshake_phase.load();

    if(phase == SERIAL_HS_WAIT_CONDITION_OK && s_serial_evt_condition_ok.exchange(false)) {
        uint8_t sbuf[32];
        serial_send_buf(sbuf, serial_build_print_start(s_layer_height_um, sbuf, sizeof(sbuf)));
        s_serial_handshake_phase.store(SERIAL_HS_WAIT_PRINT_READY);
        s_serial_evt_print_ready.store(false);
        return;
    }

    if(phase == SERIAL_HS_WAIT_PRINT_READY && s_serial_evt_print_ready.exchange(false)) {
        s_waiting_layer.store(false);
        serial_handshake_reset();
    }
}

static void serial_drain_rx(void)
{
    uint8_t chunk[64];
    drain_framed_rx(serial_recv_nonblock, chunk, sizeof(chunk),
                    s_serial_rx_accum, sizeof(s_serial_rx_accum), &s_serial_rx_accum_len,
                    serial_guess_frame_len, serial_handle_frame);
}

static void link_thread_fn(void);

/** 启动链路线程（若已在运行则忽略） */
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
static bool link_handle_layer_mark(void)
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
    serial_handshake_reset();
    s_layer_drain_pending.store(s_enable_layer_change != 0);
    if(!s_enable_layer_change) {
        s_waiting_layer.store(false);
    }
    return true;
}

/** cmd/data 均排空后下发「单层扫描完成」，再等打印准备完成 */
static void link_try_layer_serial_dispatch(void)
{
    if(!s_waiting_layer.load() || !s_layer_drain_pending.load()) {
        return;
    }
    if(!s_enable_layer_change || s_serial_fd < 0) {
        s_layer_drain_pending.store(false);
        s_waiting_layer.store(false);
        serial_handshake_reset();
        return;
    }

    uint16_t cmd_snap = 0;
    uint16_t data_snap = 0;
    job_status_get_mcu_rates(&cmd_snap, &data_snap);
    if(cmd_snap > MCU_LAYER_DRAIN_MAX_RATE || data_snap > MCU_LAYER_DRAIN_MAX_RATE) {
        return;
    }

    uint8_t sbuf[32];
    serial_send_buf(sbuf, serial_build_layer_done(s_layer_height_um, sbuf, sizeof(sbuf)));

    s_layer_drain_pending.store(false);
    s_serial_handshake_phase.store(SERIAL_HS_WAIT_PRINT_READY);
    s_serial_evt_print_ready.store(false);
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
 * @brief 运动帧发送（单轮单次 batch）
 *
 * 简化后的发送逻辑：
 *   1. RingBuffer → Stage 限量搬运
 *   2. 换层队列排空检测 + 串口 LAYER_DONE
 *   3. 若处于换层等待则暂停
 *   4. 背压：cmd ≥ 80% 时停止下发
 *   5. 每轮只发送一个 batch（batch_size 个包），然后进入下一轮 interval_ms 轮询
 */
static void link_drain_motion_queue(void)
{
    if(!s_is_sending.load() || s_is_paused.load()) {
        return;
    }

    refill_tcp_stage_from_ring();
    link_try_layer_serial_dispatch();

    if(s_waiting_layer.load()) {
        return;
    }

    uint16_t cmd_snap = 0;
    job_status_get_mcu_rates(&cmd_snap, nullptr);
    if(cmd_snap >= (65535 * 80 / 100)) {
        return;
    }

    /* 每轮只发送一个 batch，然后进入下一轮 interval_ms 轮询 */
    if(link_handle_layer_mark()) {
        link_try_layer_serial_dispatch();
        return;
    }

    if(tcp_stage_count() == 0) {
        return;
    }

    link_send_one_motion_batch();
}

/** 文件仍打开且未在解析时，从头预填 Ring（Reset / 打印完成共用） */
static void ctrl_prefill_gcode_ring(void)
{
    if(!s_rb || !gcode_is_file_open() || gcode_is_parsing()) {
        return;
    }
    /* 可能 join 解析线程，且 notify 会触发 Files 页 JOB_LOCK；调用方勿持 JOB_LOCK */
    gcode_start_parse(s_rb);
    page_files_notify_parse_restarted();
}

/**
 * @brief 空闲态下从头重新解析当前 G-code（清空 Stage，重启解析线程）
 *
 * 用于校准使能切换、Parse 增益变更等需重建 Ring 内容的场景。
 */
void ctrl_reparse_gcode(void)
{
    if(!s_rb || s_tcp_fd < 0) {
        return;
    }
    if(s_is_sending.load() || s_start_pending.load()) {
        return;
    }
    if(!gcode_is_file_open()) {
        return;
    }

    tcp_stage_clear();
    ctrl_prefill_gcode_ring();
}

/** 真正进入发送态（假定 Ring 已 ≥ 一批） */
static void ctrl_begin_sending(void)
{
    if(!s_link_run.load()) {
        link_thread_start();
    }

    s_is_sending.store(true);
    s_is_paused.store(false);
    s_sent_count.store(0);
    tcp_stage_clear();

    {
        JOB_LOCK();
        g_job_status.is_sending = true;
        g_job_status.sent_packets = 0;
        g_job_status.mcu_cmd_tx_hold = false;
        g_job_status.job_print_completed = false;
    }
    ctrl_notify_ui();

    /* 首层：1 打印请求 → 条件满足 → 4 打印开始 → 准备完成，再下发第一层运动帧 */
    if(s_enable_layer_change && s_serial_fd >= 0) {
        uint8_t sbuf[32];
        serial_send_buf(sbuf, serial_build_print_request(sbuf, sizeof(sbuf)));
        s_waiting_layer.store(true);
        s_layer_drain_pending.store(false);
        serial_handshake_reset();
        s_serial_handshake_phase.store(SERIAL_HS_WAIT_CONDITION_OK);
    }
}

/** Ring 是否满足 Start 门槛：≥1 批，或小文件已全部解析入队 */
static bool ring_meets_start_threshold(void)
{
    if(!s_rb) {
        return false;
    }
    job_status_update_ring();
    const size_t count = s_rb->count();
    const int batch = s_batch_size > 0 ? s_batch_size : 100;

    if(count >= (size_t)batch) {
        return true;
    }
    const int total = job_total_packets();
    if(gcode_is_parsing()) {
        if(total >= batch && count > 0) {
            return true;
        }
        if(s_start_pending.load() && gcode_is_file_read_exhausted() &&
           total > 0 && total < batch && count > 0) {
            return true;
        }
        return false;
    }
    if(gcode_is_file_open()) {
        return total > 0 && count > 0;
    }
    return false;
}

int ctrl_ring_ready_for_start(void)
{
    return ring_meets_start_threshold() ? 1 : 0;
}

int ctrl_is_start_pending(void)
{
    return s_start_pending.load() ? 1 : 0;
}

/** Ring 蓄满后完成用户已点击的 Start */
static void ctrl_try_complete_pending_start(void)
{
    if(!s_start_pending.load() || s_is_sending.load() || s_tcp_fd < 0) {
        return;
    }

    if(!ring_meets_start_threshold()) {
        return;
    }

    s_start_pending.store(false);
    ctrl_begin_sending();
}

/**
 * @brief Ring/Stage 已空、解析读完、且下位机 CMD/DATA 均排空时，判定为打印完成
 *
 * 完成判断标准（须全部满足）：
 *   1. 主机端 RingBuffer 为空
 *   2. 主机端 Stage 队列为空
 *   3. 解析线程已退出
 *   4. 文件已读到 EOF
 *   5. 下位机 cmd_cache_rate == 0
 *   6. 下位机 data_cache_rate == 0
 *
 * 换层使能且串口已连接时，下发串口「打印全部完成」(0xA4/0x01/0x06)。
 *
 * 注意：此处**不再**自动调用 ctrl_prefill_gcode_ring() 重新预填。
 */
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

    uint16_t cmd_snap = 0;
    uint16_t data_snap = 0;
    job_status_get_mcu_rates(&cmd_snap, &data_snap);
    if(cmd_snap > MCU_LAYER_DRAIN_MAX_RATE || data_snap > MCU_LAYER_DRAIN_MAX_RATE) {
        return;
    }

    if(s_enable_layer_change && s_serial_fd >= 0) {
        uint8_t sbuf[32];
        serial_send_buf(sbuf, serial_build_print_finished(sbuf, sizeof(sbuf)));
    }

    s_is_sending.store(false);
    s_is_paused.store(false);
    JOB_LOCK();
    g_job_status.is_sending = false;
    g_job_status.mcu_cmd_tx_hold = false;
    g_job_status.job_print_completed = true;

    /* 不再自动预填：避免下位机 Data 未排空时误判完成并重新解析 */
    ctrl_notify_ui();
}

/** 请求链路线程停止并等待其结束 */
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
static int link_thread_sleep_ms(void)
{
    if(s_is_sending.load() && !s_is_paused.load()) {
        return (s_interval_ms > 0) ? s_interval_ms : 3;
    }
    if(s_waiting_layer.load() || s_layer_drain_pending.load()) {
        return 10;
    }
    if(s_tcp_fd >= 0) {
        return HEARTBEAT_INTERVAL_MS;
    }
    return 50;
}

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
        const bool local_data_pending = (s_rb && !s_rb->empty()) || gcode_is_parsing();
        if((!motion_stream_active || !local_data_pending) && now >= next_hb && s_tcp_fd >= 0) {
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
        link_process_serial_handshake();

        /* 4. Ring 蓄满后自动完成挂起的 Start */
        ctrl_try_complete_pending_start();

        /* 5. 运动帧：①→② refill + ②→TCP consume（对齐 Qt 定时器周期） */
        link_drain_motion_queue();
        link_try_finish_sending();

        const int sleep_ms = link_thread_sleep_ms();
        if(sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

}

/* ---------- 公共接口 ---------- */

/** 初始化后端状态：重置所有标志、清空 Stage、设置默认值 */
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
    s_layer_drain_pending.store(false);
    serial_handshake_reset();
    s_start_pending.store(false);
    s_tcp_rx_accum_len = 0;
    s_serial_rx_accum_len = 0;
    tcp_stage_clear();

    JOB_LOCK();
    g_job_status.batch_size           = 100;
    g_job_status.interval_ms          = 3;
    g_job_status.layer_height_um      = s_layer_height_um;
    g_job_status.enable_layer_change  = false;
    g_job_status.cal_file[0]          = '\0';
    g_job_status.cal_file_loaded      = false;
    g_job_status.enable_cal_apply     = false;
    g_job_status.cal_has_gains        = false;
    g_job_status.cal_grid_nx          = 0;
    g_job_status.cal_grid_ny          = 0;
    g_job_status.job_print_completed  = false;
    g_job_status.current_x            = MCU_COORD_ZERO_OFFSET;
    g_job_status.current_y            = MCU_COORD_ZERO_OFFSET;
    g_job_status.current_z            = MCU_COORD_ZERO_OFFSET;
}

/* TCP */
/**
 * @brief 建立 TCP 连接到下位机
 * @param ip   目标 IP 地址
 * @param port 目标端口
 * @return 0 成功，负数失败
 *
 * 连接成功后：设为非阻塞、启动链路线程、下发读参请求。
 */
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
    ctrl_notify_ui();

    link_thread_start();

    /* 连接后读一次板参（应答在下次 ETH_report 中返回 81B 帧） */
    ctrl_request_read_params();
    return 0;
}

/** 断开 TCP 连接并清理相关状态 */
void ctrl_tcp_disconnect(void)
{
    ctrl_stop_sending();
    link_thread_stop();
    s_tcp_rx_accum_len = 0;
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
        g_job_status.enable_cal_apply = false;
    }
    ctrl_notify_ui();
}

/** @return 1 TCP 已连接，0 未连接 */
int ctrl_tcp_is_connected(void) { return s_tcp_fd >= 0; }

/* Serial */
/**
 * @brief 打开串口设备
 * @param device 设备路径，如 /dev/ttyUSB0
 * @param baud   波特率
 * @return 0 成功，-1 失败
 */
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
    ctrl_notify_ui();
    return 0;
}

/** 关闭串口设备 */
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
    ctrl_notify_ui();
}

/** @return 1 串口已打开，0 未打开 */
int ctrl_serial_is_open(void) { return s_serial_fd >= 0; }

/* RingBuffer */
/** 绑定全局 RingBuffer，供解析线程写入、发送线程读取 */
void ctrl_set_ring_buffer(RingBuffer *rb) { s_rb = rb; }

/* 发送控制 */
/**
 * @brief 开始向下位机发送运动帧
 *
 * Ring 不足一批时会先预填并挂起 Start；满一批或小文件全部入队后由 link 线程自动开始下发。
 * 若启用换层且串口已连接，开始下发时会先发 PRINT_REQUEST。
 */
void ctrl_start_sending(void)
{
    if(s_tcp_fd < 0) {
        return;
    }

    if(!ring_meets_start_threshold()) {
        if(gcode_is_file_open() && !gcode_is_parsing()) {
            ctrl_prefill_gcode_ring();
        }
        s_start_pending.store(true);
        if(!s_link_run.load()) {
            link_thread_start();
        }
        return;
    }

    s_start_pending.store(false);
    ctrl_begin_sending();
}

/** 立即停止发送，清空 Stage，重置所有换层/暂停状态 */
void ctrl_stop_sending(void)
{
    ctrl_reset_motion_job_state(true);
}

/** 暂停发送（运动帧暂停下发，链路线程和心跳继续） */
void ctrl_pause_sending(void)
{
    s_is_paused.store(true);
    ctrl_notify_ui();
}
/** 恢复发送 */
void ctrl_resume_sending(void)
{
    s_is_paused.store(false);
    ctrl_notify_ui();
}
/** @return 1 正在发送运动帧，0 未发送 */
int  ctrl_is_sending(void) { return s_is_sending.load(); }
/** @return 1 处于暂停状态，0 未暂停 */
int  ctrl_is_paused(void) { return s_is_paused.load(); }

int ctrl_is_waiting_layer(void)
{
    return s_waiting_layer.load() ? 1 : 0;
}

/* 复位 */
/**
 * @brief 换层打印进行中：经串口下发「打印终止」(0xA2/0x40/type=0x0B)
 *
 * 条件：换层已启用、串口已连接，且处于发送/换层等待/队列排空等待之一。
 */
static void ctrl_send_serial_print_terminate_if_needed(void)
{
    if(!s_enable_layer_change || s_serial_fd < 0) {
        return;
    }
    const bool layer_job_active =
        s_is_sending.load() || s_waiting_layer.load() || s_layer_drain_pending.load();
    if(!layer_job_active) {
        return;
    }

    uint8_t sbuf[32];
    serial_send_buf(sbuf, serial_build_print_terminate(sbuf, sizeof(sbuf)));
}

/**
 * @brief 发送复位帧并重置所有状态
 *
 * 换层打印中：先串口打印终止，再 TCP 复位并清 Ring/Stage、重新预填解析。
 * 通信线程保持运行，心跳继续。
 */
void ctrl_send_reset(void)
{
    ctrl_send_serial_print_terminate_if_needed();

    uint8_t frame[64];
    const int flen = build_reset_frame(frame, sizeof(frame));
    if(flen > 0) {
        tcp_send_raw(frame, (size_t)flen);
    }

    ctrl_reset_motion_job_state(true);
    s_sent_count.store(0);

    /* 先停解析线程并 join，避免与清 Ring / 重解析竞态；同时解除 put_wait 阻塞 */
    gcode_stop_parse();
    if(s_rb) {
        s_rb->clear();
    }

    {
        JOB_LOCK();
        g_job_status.sent_packets = 0;
        g_job_status.cmd_cache_rate  = 0;
        g_job_status.data_cache_rate = 0;
        g_job_status.current_x       = MCU_COORD_ZERO_OFFSET;
        g_job_status.current_y       = MCU_COORD_ZERO_OFFSET;
        g_job_status.current_z       = MCU_COORD_ZERO_OFFSET;
        g_job_status.laser_state     = 0;
        g_job_status.job_print_completed = false;
    }

    /* 勿在 JOB_LOCK 内调用：page_files_notify → refresh_detail_panel 会再次 JOB_LOCK（死锁） */
    ctrl_prefill_gcode_ring();
}

/* 参数设置 */
/** 设置每批运动帧的包数上限（受 kMaxMotionBatch=100 限制） */
void ctrl_set_batch_size(int n)
{
    s_batch_size = (n > 0 && n <= kMaxMotionBatch) ? n : kMaxMotionBatch;
    JOB_LOCK();
    g_job_status.batch_size = s_batch_size;
}

/** 设置链路线程轮询周期（影响运动帧发送频率） */
void ctrl_set_interval_ms(int ms)
{
    s_interval_ms = (ms > 0) ? ms : 3;
    JOB_LOCK();
    g_job_status.interval_ms = s_interval_ms;
}

/** 开启/关闭换层功能（开关状态同步到 job_status） */
void ctrl_set_enable_layer_change(int en)
{
    s_enable_layer_change = en ? 1 : 0;
    JOB_LOCK();
    g_job_status.enable_layer_change = en ? true : false;
}

void ctrl_set_enable_cal_apply(int en)
{
    JOB_LOCK();
    if(en && (!g_job_status.tcp_connected || !g_job_status.cal_file_loaded ||
              !g_job_status.cal_has_gains)) {
        g_job_status.enable_cal_apply = false;
        return;
    }
    g_job_status.enable_cal_apply = en ? true : false;
}

int ctrl_get_enable_cal_apply(void)
{
    JOB_LOCK();
    return g_job_status.enable_cal_apply ? 1 : 0;
}

void ctrl_center_offset_nudge(float dx_gcode, float dy_gcode)
{
    if(s_tcp_fd < 0) {
        return;
    }
    if(s_is_sending.load() || s_start_pending.load()) {
        return;
    }
    if(dx_gcode == 0.f && dy_gcode == 0.f) {
        return;
    }

    {
        JOB_LOCK();
        g_job_status.center_offset_x += dx_gcode;
        g_job_status.center_offset_y += dy_gcode;
    }
    ctrl_notify_ui();
}

void ctrl_center_offset_reset(void)
{
    if(s_tcp_fd < 0) {
        return;
    }
    if(s_is_sending.load() || s_start_pending.load()) {
        return;
    }

    {
        JOB_LOCK();
        g_job_status.center_offset_x = 0.f;
        g_job_status.center_offset_y = 0.f;
    }
    ctrl_notify_ui();
}

/* 板参数读写 */
/** 向下位机发送读参请求（应答为 81B 帧） */
void ctrl_request_read_params(void)
{
    if(s_tcp_fd < 0) {
        return;
    }
    {
        JOB_LOCK();
        g_job_status.param_read_valid = false;
    }
    job_status_ui_bump();
    uint8_t frame[64];
    int flen = build_readparam_frame(frame, sizeof(frame));
    if(flen > 0) {
        tcp_send_raw(frame, (size_t)flen);
    }
}

/** @brief 下发单条写参命令 */
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

/**
 * @brief 向下位机提交板参数
 * @return 0 成功，-1 TCP 未连接，-2 曾处于发送态已先 Stop
 *
 * 若当前正在发送，会先调用 ctrl_stop_sending()，然后逐条下发写参命令，
 * 最后发送读参请求用于验证。
 */
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
        g_job_status.flash_saved = 0;
    }

    auto write_or_fail = [&](uint8_t obj, uint16_t wp0, uint16_t wp1,
                             uint16_t wp2, uint16_t wp3) -> bool {
        if(tcp_send_writeparam(obj, wp0, wp1, wp2, wp3) != 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        return true;
    };

    /* 与读回一致：线上 float = 内部 a_max / MCU_PARAM_A_MAX_SCALE（勿再除一次） */
    float_to_u16_pair(a_max, &p0, &p1);
    if(!write_or_fail(0, p0, p1, 0, 0)) {
        return -1;
    }
    if(!write_or_fail(1, fly_en ? 1u : 0u, 0, 0, 0)) {
        return -1;
    }
    float_to_u16_pair(fly_gain, &p0, &p1);
    if(!write_or_fail(2, p0, p1, 0, 0)) {
        return -1;
    }
    if(!write_or_fail(3, acc_en ? 1u : 0u, 0, 0, 0)) {
        return -1;
    }
    float_to_u16_pair(acc_delat, &p0, &p1);
    if(!write_or_fail(4, p0, p1, 0, 0)) {
        return -1;
    }
    p0 = (uint16_t)((uint32_t)laser_start & 0xFFFFu);
    p1 = (uint16_t)(((uint32_t)laser_start >> 16) & 0xFFFFu);
    if(!write_or_fail(5, p0, p1, 0, 0)) {
        return -1;
    }
    p0 = (uint16_t)((uint32_t)laser_stop & 0xFFFFu);
    p1 = (uint16_t)(((uint32_t)laser_stop >> 16) & 0xFFFFu);
    if(!write_or_fail(6, p0, p1, 0, 0)) {
        return -1;
    }

    /* 写完后读回验证；Flash 保存标志在后续 19B 心跳里 byte[8]=0x0D */
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ctrl_request_read_params();

    return ret;
}
