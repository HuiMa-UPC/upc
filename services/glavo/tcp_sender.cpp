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

enum SerialHandshakePhase : int {
    SERIAL_HS_NONE = 0,
    SERIAL_HS_WAIT_CONDITION_OK,
    SERIAL_HS_WAIT_PRINT_READY,
};

using frame_guess_fn  = size_t (*)(const uint8_t*, size_t);
using frame_handle_fn = void (*)(const uint8_t*, size_t);

size_t tcp_guess_frame_len(const uint8_t* buf, size_t avail)
{
    if(avail < 4) {
        return 0;
    }
    if(buf[0] != 0xBE || buf[1] != 0xEF) {
        return 1;
    }
    if(buf[2] == 0x10 && buf[3] == 0x01 && avail >= 19) {
        return 19;
    }
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

size_t serial_guess_frame_len(const uint8_t* buf, size_t avail)
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
    return avail < frame_len ? 0 : frame_len;
}

void serial_handle_frame(const uint8_t* frame, size_t len)
{
    const serial_event_t evt = serial_parse_frame(frame, len);
    if(evt != SERIAL_EVT_NONE) {
        TcpSender::instance().notify_serial_event(evt);
    }
}

void drain_framed_rx(int (*recv_fn)(uint8_t*, size_t),
                     uint8_t* chunk, size_t chunk_sz,
                     uint8_t* accum, size_t accum_cap, size_t* accum_len,
                     frame_guess_fn guess,
                     frame_handle_fn handle)
{
    for(;;) {
        const int n = recv_fn(chunk, chunk_sz);
        if(n <= 0) {
            break;
        }
        if(*accum_len + (size_t)n > accum_cap) {
            const size_t keep = accum_cap / 2;
            if(*accum_len > keep) {
                std::memmove(accum, accum + *accum_len - keep, keep);
                *accum_len = keep;
            }
        }
        std::memcpy(accum + *accum_len, chunk, (size_t)n);
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
            std::memmove(accum, accum + pos, *accum_len - pos);
            *accum_len -= pos;
        }
    }
}

} // namespace

TcpSender& TcpSender::instance()
{
    static TcpSender inst;
    return inst;
}

TcpSender::TcpSender() = default;

TcpSender::~TcpSender()
{
    stop();
}

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

bool TcpSender::set_tcp_nodelay(int sockfd)
{
    int flag = 1;
    return setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

int TcpSender::connect_tcp(const char* ip, int port)
{
    if(!ip || port <= 0) {
        return -1;
    }
    if(tcp_fd_ >= 0) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if(inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        return -2;
    }

    if(connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -3;
    }

    if(!set_tcp_nodelay(fd)) {
#if SERVICES_DEBUG
        printf("[TcpSender] TCP_NODELAY set failed\n");
#endif
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::snprintf(tcp_ip_, sizeof(tcp_ip_), "%s", ip);
    tcp_port_ = port;

    tcp_fd_ = fd;
    PrintJobState::instance().set_tcp_connected(true);
    tcp_rx_accum_len_ = 0;

#if SERVICES_DEBUG
    printf("[TcpSender] TCP connected %s:%d (NODELAY=1)\n", ip, port);
#endif
    return 0;
}

int TcpSender::open_serial(const char* device, int baud)
{
    if(!device) {
        return -1;
    }
    if(serial_fd_ >= 0) {
        close(serial_fd_);
        serial_fd_ = -1;
    }

    const int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) {
        return -1;
    }

    struct termios tty{};
    if(tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    speed_t spd = B115200;
    switch(baud) {
        case 9600:   spd = B9600; break;
        case 115200: spd = B115200; break;
        default:     spd = B115200; break;
    }
    cfsetospeed(&tty, spd);
    cfsetispeed(&tty, spd);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= CS8;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);

    serial_fd_ = fd;
    serial_rx_accum_len_ = 0;
    PrintJobState::instance().set_serial_open(true);

#if SERVICES_DEBUG
    printf("[TcpSender] serial open %s @ %d\n", device, baud);
#endif
    return 0;
}

void TcpSender::start_link_thread()
{
    if(link_run_.load()) {
        return;
    }
    link_stop_.store(false);
    link_run_.store(true);
    if(link_th_.joinable()) {
        link_th_.join();
    }
    link_th_ = std::thread(&TcpSender::link_thread_fn, this);
}

void TcpSender::stop()
{
    link_stop_.store(true);
    link_run_.store(false);
    if(link_th_.joinable()) {
        link_th_.join();
    }
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

bool TcpSender::is_link_running() const { return link_run_.load(); }
bool TcpSender::is_tcp_connected() const { return tcp_fd_ >= 0; }
bool TcpSender::is_serial_open() const { return serial_fd_ >= 0; }
bool TcpSender::is_sending() const { return is_sending_.load(); }

void TcpSender::emit_event(TcpJobEvent ev)
{
    if(event_cb_) {
        event_cb_(ev);
    }
}

void TcpSender::notify_serial_event(serial_event_t evt)
{
    if(evt == SERIAL_EVT_PRINT_CONDITION_OK) {
        serial_evt_condition_ok_.store(true);
    } else if(evt == SERIAL_EVT_PRINT_READY) {
        serial_evt_print_ready_.store(true);
    }
}

int TcpSender::tcp_send_raw(const uint8_t* data, size_t len)
{
    if(tcp_fd_ < 0 || !data || len == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(tcp_send_mtx_);
    const ssize_t n = send(tcp_fd_, data, len, 0);
    if(n != (ssize_t)len) {
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
            return 0;
        }
        return -1;
    }
    if(n == 0) {
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

int TcpSender::serial_send_raw(const uint8_t* data, size_t len)
{
    if(serial_fd_ < 0 || !data || len == 0) {
        return -1;
    }
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
            return 0;
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
                break;
            }
        }

        rb_pkt_type_t ptype;
        if(!rb_->peek_type(ptype)) {
            break;
        }

        rb_packet_t pkt;
        if(!rb_->get(pkt, 5)) {
            break;
        }

        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        tcp_stage_.push_back(pkt);
        moved++;
    }
}

bool TcpSender::mcu_rates_drained() const
{
    const auto snap = PrintJobState::instance().snapshot();
    return snap.cmd_cache_rate == 0 && snap.data_cache_rate == 0;
}

bool TcpSender::motion_stream_active() const
{
    return is_sending_.load() && !waiting_layer_.load() && !terminate_pending_.load();
}

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
            return true;
        }
        return false;
    }
    if(GcodeWorker::instance().is_file_open()) {
        return total > 0 && count > 0;
    }
    return false;
}

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

    first_layer_height_um_  = GcodeWorker::instance().get_layer_height_um();
    active_layer_height_um_ = first_layer_height_um_;

    PrintJobState::instance().set_sending(true);
    PrintJobState::instance().set_job_completed(false);
    PrintJobState::instance().set_job_error(false);
    PrintJobState::instance().set_sent_packets(0);

#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ >= 0) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_request(sbuf, sizeof(sbuf));
        if(slen > 0) {
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
        return;
    }
    if(is_sending_.load()) {
        return;
    }
    if(!ring_meets_start_threshold()) {
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
        return;
    }
    start_pending_.store(false);
    begin_sending_internal();
}

void TcpSender::request_terminate()
{
    terminate_pending_.store(true);
    is_sending_.store(false);
    start_pending_.store(false);
    PrintJobState::instance().set_sending(false);
}

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
            serial_send_raw(sbuf, (size_t)slen);
        }
    }
#endif
}

void TcpSender::run_manual_terminate_cleanup()
{
    send_tcp_reset();
    send_serial_print_terminate();

    GcodeWorker::instance().stop_parse();
    if(rb_) {
        rb_->clear();
    }
    tcp_stage_clear();

    const bool was_error = PrintJobState::instance().snapshot().job_error;
    reset_motion_state(true);
    PrintJobState::instance().reset_job_counters();

    emit_event(was_error ? TcpJobEvent::PrintError : TcpJobEvent::TerminateDone);
}

void TcpSender::run_natural_complete_cleanup()
{
    send_serial_print_finished();
    send_tcp_reset();

    reset_motion_state(true);
    PrintJobState::instance().set_job_completed(true);
    PrintJobState::instance().set_sending(false);

    emit_event(TcpJobEvent::PrintCompleted);
}

void TcpSender::link_process_serial_handshake()
{
#if !GLAVO_ENABLE_LAYER_CHANGE
    return;
#endif
    if(!waiting_layer_.load() || serial_fd_ < 0) {
        return;
    }

    const int phase = serial_handshake_phase_.load();

    if(phase == SERIAL_HS_WAIT_CONDITION_OK &&
       serial_evt_condition_ok_.exchange(false)) {
        uint8_t sbuf[32];
        const int slen = serial_build_print_start(first_layer_height_um_, sbuf, sizeof(sbuf));
        if(slen > 0) {
            serial_send_raw(sbuf, (size_t)slen);
        }
        serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
        serial_evt_print_ready_.store(false);
        return;
    }

    if(phase == SERIAL_HS_WAIT_PRINT_READY &&
       serial_evt_print_ready_.exchange(false)) {
        waiting_layer_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
    }
}

bool TcpSender::link_handle_layer_mark()
{
    rb_pkt_type_t ptype;
    if(!tcp_stage_peek_type(&ptype) || ptype != RB_PKT_LAYER_MARK) {
        return false;
    }

    rb_packet_t mark;
    if(!tcp_stage_pop_front(&mark)) {
        return false;
    }

#if GLAVO_ENABLE_LAYER_CHANGE
    if(serial_fd_ < 0) {
        return false;
    }
    const uint16_t mark_um = rb_layer_mark_get_height_um(mark);
    if(mark_um > 0) {
        active_layer_height_um_ = mark_um;
    }
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
        return;
    }
    if(serial_fd_ < 0) {
        waiting_layer_.store(false);
        layer_drain_pending_.store(false);
        serial_handshake_phase_.store(SERIAL_HS_NONE);
        return;
    }
    if(!mcu_rates_drained()) {
        return;
    }

    uint8_t sbuf[32];
    const int slen = serial_build_layer_done(active_layer_height_um_, sbuf, sizeof(sbuf));
    if(slen > 0) {
        serial_send_raw(sbuf, (size_t)slen);
    }
    layer_drain_pending_.store(false);
    serial_handshake_phase_.store(SERIAL_HS_WAIT_PRINT_READY);
    serial_evt_print_ready_.store(false);
}

int TcpSender::link_send_one_motion_batch()
{
    if(tcp_stage_count() == 0) {
        return 0;
    }

    rb_packet_t staged[kMaxMotionBatch];
    int n = 0;

    for(int i = 0; i < kMaxMotionBatch; i++) {
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

    uint8_t payload[kMaxMotionBatch * 11];
    for(int i = 0; i < n; i++) {
        std::memcpy(&payload[i * 11], staged[i].data, 11);
    }

    uint8_t frame[3 + kMaxMotionBatch * 11];
    const int flen = build_motion_frame(payload, n, frame, sizeof(frame));
    if(flen <= 0 || tcp_send_raw(frame, (size_t)flen) != 0) {
        std::lock_guard<std::mutex> lock(tcp_stage_mtx_);
        for(int i = n - 1; i >= 0; i--) {
            tcp_stage_.push_front(staged[i]);
        }
        return 0;
    }

    const int sent = PrintJobState::instance().snapshot().sent_packets + n;
    PrintJobState::instance().set_sent_packets(sent);

    if(progress_cb_) {
        progress_cb_();
    }
    return n;
}

void TcpSender::link_drain_motion_queue()
{
    if(!is_sending_.load()) {
        return;
    }
    if(waiting_layer_.load()) {
        return;
    }

    refill_tcp_stage_from_ring();
    link_try_layer_serial_dispatch();

    const auto snap = PrintJobState::instance().snapshot();
    const uint32_t threshold =
        (65535u * (uint32_t)MCU_CMD_BACKPRESSURE_PCT) / 100u;
    if(snap.cmd_cache_rate >= threshold) {
        return;
    }

    if(link_handle_layer_mark()) {
        link_try_layer_serial_dispatch();
        return;
    }

    if(tcp_stage_count() == 0) {
        return;
    }

    link_send_one_motion_batch();
}

void TcpSender::link_try_finish_sending()
{
    if(!is_sending_.load() || waiting_layer_.load()) {
        return;
    }
    if(tcp_stage_count() > 0) {
        return;
    }
    if(rb_ && !rb_->empty()) {
        return;
    }
    if(GcodeWorker::instance().is_parsing()) {
        return;
    }
    if(!GcodeWorker::instance().is_file_read_exhausted()) {
        return;
    }
    if(!mcu_rates_drained()) {
        natural_complete_pending_.store(true);
        return;
    }

    natural_complete_pending_.store(false);
    is_sending_.store(false);
    run_natural_complete_cleanup();
}

int TcpSender::link_thread_sleep_ms() const
{
    if(terminate_pending_.load()) {
        return 5;
    }
    if(is_sending_.load() && !waiting_layer_.load()) {
        return TCP_SEND_INTERVAL_MS;
    }
    if(waiting_layer_.load() || natural_complete_pending_.load()) {
        return 10;
    }
    if(tcp_fd_ >= 0) {
        return TCP_HEARTBEAT_INTERVAL_MS;
    }
    return 50;
}

void TcpSender::link_thread_fn()
{
    using clock = std::chrono::steady_clock;
    auto next_hb = clock::now();

    while(link_run_.load() && !link_stop_.load()) {
        const auto now = clock::now();

        if(tcp_fd_ >= 0) {
            tcp_drain_rx();
        } else if(is_sending_.load()) {
            PrintJobState::instance().set_job_error(true);
            is_sending_.store(false);
            terminate_pending_.store(true);
        } else if(!terminate_pending_.load()) {
            connect_tcp(tcp_ip_, tcp_port_);
            if(tcp_fd_ < 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
                continue;
            }
        }

        if(terminate_pending_.load()) {
            run_manual_terminate_cleanup();
            terminate_pending_.store(false);
        } else {
            const bool stream = motion_stream_active();
            if(!stream && tcp_fd_ >= 0 && now >= next_hb) {
                uint8_t hb[64];
                const int hblen = build_heartbeat_frame(hb, sizeof(hb));
                if(hblen > 0) {
                    tcp_send_raw(hb, (size_t)hblen);
                }
                next_hb = now + std::chrono::milliseconds(TCP_HEARTBEAT_INTERVAL_MS);
                tcp_drain_rx();
            }

            serial_drain_rx();
            link_process_serial_handshake();
            try_complete_pending_start();
            link_drain_motion_queue();
            link_try_finish_sending();
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(link_thread_sleep_ms()));
    }
}

} // namespace services
