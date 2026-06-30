#include "mcu_ota_session.hpp"
#include "device_config.hpp"
#include "tcp_tx.hpp"
#include "rs_log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace upc::glavo {

namespace {

bool ends_with_bin(const char* name)
{
    if(!name) {
        return false;
    }
    const size_t n = std::strlen(name);
    return n >= 4 && std::strcmp(name + n - 4, ".bin") == 0;
}

const char* ota_evt_name(uint8_t flag)
{
    switch(flag) {
    case TCP_EVT_OTA_READY:      return "OTA_READY(0x03)";
    case TCP_EVT_OTA_CHUNK_DONE: return "CHUNK_DONE(0x23)";
    case TCP_EVT_OTA_ALL_DONE:   return "ALL_DONE(0x13)";
    case TCP_EVT_OTA_FAILED:     return "FAILED(0xE3)";
    default:                     return "UNKNOWN";
    }
}

} // namespace

void McuOtaSession::set_status_callback(McuOtaStatusCb cb)
{
    status_cb_ = std::move(cb);
}

bool McuOtaSession::is_active() const noexcept
{
    return phase_ != Phase::None && phase_ != Phase::Done &&
           phase_ != Phase::Failed && phase_ != Phase::Cancelled;
}

bool McuOtaSession::suppress_idle_heartbeat() const noexcept
{
    return phase_ == Phase::WaitReady || phase_ == Phase::Streaming;
}

bool McuOtaSession::take_post_success_reconnect() noexcept
{
    return post_success_reconnect_.exchange(false);
}

void McuOtaSession::reset_file()
{
    if(file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }
    bin_path_.clear();
    file_size_          = 0;
    total_chunks_       = 0;
    next_seq_           = 0;
    hb_sent_after_last_ = false;
    end_sent_           = false;
    waiting_            = WaitEvent::None;
}

bool McuOtaSession::open_newest_bin(const char* dir)
{
    reset_file();
    if(!dir || !dir[0]) {
        return false;
    }

    DIR* dp = ::opendir(dir);
    if(!dp) {
        UPC_LOG_MCU_OTA(Error, "OTA opendir fail %s errno=%d", dir, errno);
        return false;
    }

    std::string best_path;
    time_t best_mtime = 0;

    for(dirent* ent = ::readdir(dp); ent; ent = ::readdir(dp)) {
        if(!ends_with_bin(ent->d_name)) {
            continue;
        }
        std::string path = dir;
        if(!path.empty() && path.back() != '/') {
            path += '/';
        }
        path += ent->d_name;

        struct stat file_stat{};
        if(::stat(path.c_str(), &file_stat) != 0 ||
           !S_ISREG(file_stat.st_mode)) {
            continue;
        }
        if(best_path.empty() || file_stat.st_mtime >= best_mtime) {
            best_mtime = file_stat.st_mtime;
            best_path  = std::move(path);
        }
    }
    ::closedir(dp);

    if(best_path.empty()) {
        UPC_LOG_MCU_OTA(Error, "OTA no .bin in %s", dir);
        return false;
    }

    const int fd = ::open(best_path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) {
        UPC_LOG_MCU_OTA(Error, "OTA open fail %s errno=%d",
                         best_path.c_str(), errno);
        return false;
    }

    struct stat file_stat{};
    if(::fstat(fd, &file_stat) != 0 || file_stat.st_size <= 0) {
        ::close(fd);
        return false;
    }

    file_fd_   = fd;
    bin_path_  = best_path;
    file_size_ = static_cast<uint32_t>(file_stat.st_size);
    const uint32_t chunk = static_cast<uint32_t>(GLAVO_OTA_CHUNK_SIZE);
    total_chunks_ = (file_size_ + chunk - 1u) / chunk;

    UPC_LOG_MCU_OTA(Info, "OTA bin %s size=%u chunks=%u",
                     bin_path_.c_str(), file_size_, total_chunks_);
    return true;
}

bool McuOtaSession::start(const char* image_dir)
{
    if(is_active()) {
        UPC_LOG_MCU_OTA(Warn, "OTA already active");
        return false;
    }

    const char* dir = (image_dir && image_dir[0]) ? image_dir : GLAVO_OTA_IMAGE_DIR;
    if(!open_newest_bin(dir)) {
        return false;
    }

    cancel_requested_.store(false);
    phase_ = Phase::WaitReady;
    notify(middleware::McuOtaState::Preparing, 0);
    return true;
}

void McuOtaSession::cancel()
{
    if(phase_ == Phase::None) {
        return;
    }
    cancel_requested_.store(true);
}

void McuOtaSession::notify(middleware::McuOtaState state, uint8_t progress)
{
    if(!status_cb_) {
        return;
    }
    middleware::McuOtaStatusPayload p{};
    p.state    = state;
    p.progress = progress;
    status_cb_(p);
}

void McuOtaSession::finish(middleware::McuOtaState state, uint8_t progress)
{
    notify(state, progress);
    reset_file();
    if(state == middleware::McuOtaState::Success) {
        phase_ = Phase::Done;
        post_success_reconnect_.store(true);
    } else if(state == middleware::McuOtaState::Cancelled) {
        phase_ = Phase::Cancelled;
    } else {
        phase_ = Phase::Failed;
    }
}

bool McuOtaSession::send_ota_ctrl(uint8_t mode, int (*tcp_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = tcp_build_ota_ctrl_frame(mode, frame, sizeof(frame));
    if(len <= 0 || !tcp_send || tcp_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Error, "OTA send 0x03 mode=0x%02X fail", mode);
        return false;
    }
    UPC_LOG_MCU_OTA(Info, "OTA TX ctrl 0x03 mode=0x%02X", mode);
    return true;
}

bool McuOtaSession::send_heartbeat(int (*tcp_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = tcp_build_heartbeat_frame(frame, sizeof(frame));
    if(len <= 0 || !tcp_send || tcp_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Error, "OTA send heartbeat 0x0B fail");
        return false;
    }
    UPC_LOG_MCU_OTA(Debug, "OTA TX heartbeat 0x0B");
    return true;
}

bool McuOtaSession::send_chunk(int (*tcp_send)(const uint8_t*, size_t))
{
    if(file_fd_ < 0 || next_seq_ >= total_chunks_) {
        return false;
    }

    const uint32_t chunk = static_cast<uint32_t>(GLAVO_OTA_CHUNK_SIZE);
    const uint32_t off   = next_seq_ * chunk;
    uint16_t len         = chunk;
    if(off + len > file_size_) {
        len = static_cast<uint16_t>(file_size_ - off);
    }

    std::vector<uint8_t> buf(len);
    const ssize_t rd = ::pread(file_fd_, buf.data(), len, static_cast<off_t>(off));
    if(rd != static_cast<ssize_t>(len)) {
        UPC_LOG_MCU_OTA(Error, "OTA read fail seq=%u", next_seq_);
        return false;
    }

    uint8_t frame[16u + GLAVO_OTA_CHUNK_SIZE];
    const int flen = tcp_build_ota_data_frame(
        static_cast<uint16_t>(next_seq_), buf.data(), len,
        frame, sizeof(frame));
    if(flen <= 0 || !tcp_send || tcp_send(frame, static_cast<size_t>(flen)) != 0) {
        UPC_LOG_MCU_OTA(Error, "OTA send data seq=%u fail", next_seq_);
        return false;
    }

    waiting_ = WaitEvent::ChunkAck;
    notify(middleware::McuOtaState::Transferring, last_progress_);
    UPC_LOG_MCU_OTA(Debug, "OTA TX data seq=%u len=%u", next_seq_, len);
    return true;
}

void McuOtaSession::handle_event(uint8_t flag, uint8_t progress)
{
    if(progress > 0) {
        last_progress_ = progress;
    }

    if(flag != TCP_EVT_NONE) {
        UPC_LOG_MCU_OTA(Debug,
                        "OTA RX evt=0x%02X %s progress=%u phase=%u waiting=%u seq=%u/%u",
                        flag, ota_evt_name(flag), progress,
                        static_cast<unsigned>(phase_),
                        static_cast<unsigned>(waiting_),
                        next_seq_, total_chunks_);
    }

    if(flag == TCP_EVT_OTA_FAILED) {
        UPC_LOG_MCU_OTA(Error, "OTA MCU fail evt=0xE3");
        finish(middleware::McuOtaState::Failed, progress);
        return;
    }

    if(phase_ == Phase::WaitReady && flag == TCP_EVT_OTA_READY) {
        UPC_LOG_MCU_OTA(Info, "OTA MCU ready");
        phase_   = Phase::Streaming;
        waiting_ = WaitEvent::None;
        notify(middleware::McuOtaState::Transferring, progress);
        return;
    }

    if(phase_ == Phase::Streaming && waiting_ == WaitEvent::ChunkAck &&
       flag == TCP_EVT_OTA_CHUNK_DONE) {
        ++next_seq_;
        waiting_ = WaitEvent::None;
        notify(middleware::McuOtaState::Transferring, progress);
        if(next_seq_ >= total_chunks_) {
            UPC_LOG_MCU_OTA(Info, "OTA all chunks sent, wait heartbeat rsp");
            phase_              = Phase::WaitHbAfterLast;
            hb_sent_after_last_ = false;
            notify(middleware::McuOtaState::Verifying, progress);
        }
        return;
    }

    if(phase_ == Phase::WaitComplete && waiting_ == WaitEvent::Complete &&
       flag == TCP_EVT_OTA_ALL_DONE) {
        UPC_LOG_MCU_OTA(Info, "OTA MCU complete evt=0x13");
        finish(middleware::McuOtaState::Success, progress);
        return;
    }

    if(flag == TCP_EVT_OTA_ALL_DONE || flag == TCP_EVT_OTA_CHUNK_DONE ||
       flag == TCP_EVT_OTA_READY) {
        UPC_LOG_MCU_OTA(Warn,
                        "OTA RX ignored evt=0x%02X phase=%u waiting=%u",
                        flag,
                        static_cast<unsigned>(phase_),
                        static_cast<unsigned>(waiting_));
    }
}

void McuOtaSession::on_status_response(const tcp_response_t& resp)
{
    if(!is_active()) {
        return;
    }

    if(phase_ == Phase::WaitHbAfterLast && hb_sent_after_last_) {
        UPC_LOG_MCU_OTA(Info, "OTA RX heartbeat rsp evt=0x%02X progress=%u",
                        resp.event_flag, resp.ota_progress);
        phase_ = Phase::WaitComplete;
        if(resp.ota_progress > 0) {
            last_progress_ = resp.ota_progress;
        }
        return;
    }

    handle_event(resp.event_flag, resp.ota_progress);
}

void McuOtaSession::tick(int (*tcp_send)(const uint8_t*, size_t),
                         void (*tcp_drain_rx)(),
                         bool tcp_connected)
{
    if(cancel_requested_.load()) {
        UPC_LOG_MCU_OTA(Info, "OTA cancelled (test)");
        finish(middleware::McuOtaState::Cancelled, last_progress_);
        cancel_requested_.store(false);
        return;
    }

    if(phase_ == Phase::None || phase_ == Phase::Done ||
       phase_ == Phase::Failed || phase_ == Phase::Cancelled) {
        return;
    }

    if(!tcp_connected) {
        if(phase_ != Phase::WaitComplete) {
            UPC_LOG_MCU_OTA(Error, "OTA TCP disconnected");
            finish(middleware::McuOtaState::Failed, last_progress_);
        }
        return;
    }

    if(tcp_drain_rx) {
        tcp_drain_rx();
    }

    switch(phase_) {
    case Phase::WaitReady:
        if(waiting_ == WaitEvent::None &&
           send_ota_ctrl(TCP_OTA_MODE_BEGIN, tcp_send)) {
            waiting_ = WaitEvent::Ready;
        }
        break;

    case Phase::Streaming:
        if(waiting_ == WaitEvent::None) {
            (void)send_chunk(tcp_send);
        }
        break;

    case Phase::WaitHbAfterLast:
        if(!hb_sent_after_last_ && send_heartbeat(tcp_send)) {
            hb_sent_after_last_ = true;
        }
        break;

    case Phase::WaitComplete:
        if(!end_sent_ && send_ota_ctrl(TCP_OTA_MODE_END, tcp_send)) {
            end_sent_ = true;
            waiting_  = WaitEvent::Complete;
        }
        break;

    default:
        break;
    }
}

} // namespace upc::glavo
