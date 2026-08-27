#include "mcu_ota_session.hpp"
#include "device_config.hpp"
#include "tcp_tx.hpp"
#include "rs_log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

const char* basename_only(const std::string& path)
{
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path.c_str() : path.c_str() + pos + 1;
}

const char* ota_evt_name(uint8_t flag)
{
    switch(flag) {
    case TCP_EVT_OTA_READY:      return "OTA_READY(0x03)";
    case TCP_EVT_OTA_HEADER_OK:  return "HEADER_OK(0x0A)";
    case TCP_EVT_OTA_HEADER_BAD: return "HEADER_REJECT(0x0B)";
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
    return phase_ == Phase::WaitReady || phase_ == Phase::WaitHeaderAck ||
           phase_ == Phase::Streaming;
}

bool McuOtaSession::take_post_success_reconnect() noexcept
{
    return post_success_reconnect_.exchange(false);
}

void McuOtaSession::reset_file_state()
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

void McuOtaSession::reset_candidates()
{
    candidate_paths_.clear();
    candidate_index_ = 0;
}

bool McuOtaSession::scan_bin_candidates(const char* dir)
{
    reset_candidates();
    if(!dir || !dir[0]) {
        return false;
    }

    DIR* dp = ::opendir(dir);
    if(!dp) {
        UPC_LOG_MCU_OTA(Error, "OTA opendir fail %s errno=%d", dir, errno);
        return false;
    }

    const std::string prefix = [&dir]() {
        std::string p = dir;
        if(!p.empty() && p.back() != '/') {
            p += '/';
        }
        return p;
    }();

    for(dirent* ent = ::readdir(dp); ent; ent = ::readdir(dp)) {
        if(!ends_with_bin(ent->d_name)) {
            continue;
        }
        const std::string path = prefix + ent->d_name;
        struct stat file_stat{};
        if(::stat(path.c_str(), &file_stat) != 0 ||
           !S_ISREG(file_stat.st_mode) || file_stat.st_size <= 0) {
            continue;
        }
        candidate_paths_.push_back(path);
    }
    ::closedir(dp);

    std::sort(candidate_paths_.begin(), candidate_paths_.end(),
              [](const std::string& a, const std::string& b) {
                  return std::strcmp(basename_only(a), basename_only(b)) < 0;
              });

    if(candidate_paths_.empty()) {
        UPC_LOG_MCU_OTA(Error, "OTA no .bin in %s", dir);
        return false;
    }

    UPC_LOG_MCU_OTA(Info, "OTA found %zu .bin candidate(s) in %s",
                     candidate_paths_.size(), dir);
    for(size_t i = 0; i < candidate_paths_.size(); ++i) {
        UPC_LOG_MCU_OTA(Info, "OTA candidate[%zu] %s", i,
                         basename_only(candidate_paths_[i]));
    }
    return true;
}

bool McuOtaSession::open_candidate(size_t index)
{
    if(index >= candidate_paths_.size()) {
        return false;
    }

    if(file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }

    const std::string& path = candidate_paths_[index];
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) {
        UPC_LOG_MCU_OTA(Error, "OTA open fail %s errno=%d", path.c_str(), errno);
        return false;
    }

    struct stat file_stat{};
    if(::fstat(fd, &file_stat) != 0 || file_stat.st_size <= 0) {
        ::close(fd);
        UPC_LOG_MCU_OTA(Error, "OTA stat fail %s", path.c_str());
        return false;
    }

    candidate_index_ = index;
    file_fd_         = fd;
    bin_path_        = path;
    file_size_       = static_cast<uint32_t>(file_stat.st_size);
    const uint32_t chunk = static_cast<uint32_t>(GLAVO_OTA_CHUNK_SIZE);
    total_chunks_ = (file_size_ + chunk - 1u) / chunk;
    next_seq_     = 0;
    hb_sent_after_last_ = false;
    end_sent_           = false;
    waiting_            = WaitEvent::None;

    UPC_LOG_MCU_OTA(Info, "OTA open candidate[%zu/%zu] %s size=%u chunks=%u",
                     index + 1, candidate_paths_.size(),
                     basename_only(bin_path_), file_size_, total_chunks_);
    return true;
}

bool McuOtaSession::start(const char* image_dir)
{
    if(is_active()) {
        UPC_LOG_MCU_OTA(Warn, "OTA already active");
        return false;
    }

    const char* dir = (image_dir && image_dir[0]) ? image_dir : GALVO_OTA_IMAGE_DIR;
    if(!scan_bin_candidates(dir)) {
        return false;
    }
    if(!open_candidate(0)) {
        reset_candidates();
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
    reset_file_state();
    reset_candidates();
    if(state == middleware::McuOtaState::Success) {
        phase_ = Phase::Done;
        post_success_reconnect_.store(true);
    } else if(state == middleware::McuOtaState::Cancelled) {
        phase_ = Phase::Cancelled;
    } else {
        phase_ = Phase::Failed;
    }
}

void McuOtaSession::on_header_rejected(uint8_t progress)
{
    UPC_LOG_MCU_OTA(Warn,
                    "OTA header reject 0x0B file=%s attempt %zu/%zu progress=%u",
                    basename_only(bin_path_), candidate_index_ + 1,
                    candidate_paths_.size(), progress);

    reset_file_state();

    const size_t next_index = candidate_index_ + 1;
    if(next_index >= candidate_paths_.size()) {
        if(candidate_paths_.size() == 1) {
            UPC_LOG_MCU_OTA(Error,
                            "OTA failed: sole candidate header address mismatch");
        } else {
            UPC_LOG_MCU_OTA(Error,
                            "OTA failed: all %zu candidate(s) header address mismatch",
                            candidate_paths_.size());
        }
        finish(middleware::McuOtaState::Failed, progress);
        return;
    }

    UPC_LOG_MCU_OTA(Info, "OTA switch to candidate[%zu/%zu]",
                     next_index + 1, candidate_paths_.size());
    if(!open_candidate(next_index)) {
        UPC_LOG_MCU_OTA(Error, "OTA open next candidate fail");
        finish(middleware::McuOtaState::Failed, progress);
        return;
    }

    phase_ = Phase::WaitReady;
    notify(middleware::McuOtaState::Preparing, progress);
}

bool McuOtaSession::send_ota_ctrl(uint8_t mode, int (*tcp_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = tcp_build_ota_ctrl_frame(mode, frame, sizeof(frame));
    if(len <= 0 || !tcp_send || tcp_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Error, "OTA send 0x03 mode=0x%02X fail", mode);
        return false;
    }
    UPC_LOG_MCU_OTA(Info, "OTA TX ctrl 0x03 mode=0x%02X file=%s", mode,
                     bin_path_.empty() ? "?" : basename_only(bin_path_));
    return true;
}

bool McuOtaSession::send_heartbeat(int (*tcp_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = tcp_build_heartbeat_frame(frame, sizeof(frame));
    if(len <= 0 || !tcp_send || tcp_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Error, "OTA send heartbeat CMD=0x0B fail");
        return false;
    }
    UPC_LOG_MCU_OTA(Debug, "OTA TX heartbeat CMD=0x0B");
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

    if(next_seq_ == 0) {
        waiting_ = WaitEvent::HeaderAck;
    } else {
        waiting_ = WaitEvent::ChunkAck;
    }
    notify(middleware::McuOtaState::Transferring, last_progress_);
    UPC_LOG_MCU_OTA(Debug, "OTA TX data seq=%u len=%u file=%s",
                     next_seq_, len, basename_only(bin_path_));
    return true;
}

void McuOtaSession::handle_event(uint8_t flag, uint8_t progress)
{
    if(progress > 0) {
        last_progress_ = progress;
    }

    if(flag != TCP_EVT_NONE) {
        UPC_LOG_MCU_OTA(Debug,
                        "OTA RX evt=0x%02X %s progress=%u phase=%u waiting=%u seq=%u/%u file=%s",
                        flag, ota_evt_name(flag), progress,
                        static_cast<unsigned>(phase_),
                        static_cast<unsigned>(waiting_),
                        next_seq_, total_chunks_,
                        bin_path_.empty() ? "?" : basename_only(bin_path_));
    }

    if(flag == TCP_EVT_OTA_FAILED) {
        UPC_LOG_MCU_OTA(Error, "OTA MCU fail evt=0xE3");
        finish(middleware::McuOtaState::Failed, progress);
        return;
    }

    if(phase_ == Phase::WaitReady && flag == TCP_EVT_OTA_READY) {
        UPC_LOG_MCU_OTA(Info, "OTA MCU ready file=%s",
                         basename_only(bin_path_));
        phase_   = Phase::WaitHeaderAck;
        waiting_ = WaitEvent::None;
        notify(middleware::McuOtaState::Transferring, progress);
        return;
    }

    if(phase_ == Phase::WaitHeaderAck && waiting_ == WaitEvent::HeaderAck &&
       flag == TCP_EVT_OTA_HEADER_OK) {
        UPC_LOG_MCU_OTA(Info, "OTA header OK 0x0A file=%s",
                         basename_only(bin_path_));
        next_seq_ = 1;
        waiting_  = WaitEvent::None;
        phase_    = Phase::Streaming;
        notify(middleware::McuOtaState::Transferring, progress);
        return;
    }

    if(phase_ == Phase::WaitHeaderAck && waiting_ == WaitEvent::HeaderAck &&
       flag == TCP_EVT_OTA_HEADER_BAD) {
        on_header_rejected(progress);
        return;
    }

    if(phase_ == Phase::Streaming && waiting_ == WaitEvent::ChunkAck &&
       flag == TCP_EVT_OTA_CHUNK_DONE) {
        ++next_seq_;
        waiting_ = WaitEvent::None;
        notify(middleware::McuOtaState::Transferring, progress);
        if(next_seq_ >= total_chunks_) {
            UPC_LOG_MCU_OTA(Info, "OTA all chunks sent file=%s, wait heartbeat rsp",
                             basename_only(bin_path_));
            phase_              = Phase::WaitHbAfterLast;
            hb_sent_after_last_ = false;
            notify(middleware::McuOtaState::Verifying, progress);
        }
        return;
    }

    if(phase_ == Phase::WaitComplete && waiting_ == WaitEvent::Complete &&
       flag == TCP_EVT_OTA_ALL_DONE) {
        UPC_LOG_MCU_OTA(Info, "OTA MCU complete evt=0x13 file=%s",
                         basename_only(bin_path_));
        finish(middleware::McuOtaState::Success, progress);
        return;
    }

    if(flag == TCP_EVT_OTA_ALL_DONE || flag == TCP_EVT_OTA_CHUNK_DONE ||
       flag == TCP_EVT_OTA_READY || flag == TCP_EVT_OTA_HEADER_OK ||
       flag == TCP_EVT_OTA_HEADER_BAD) {
        UPC_LOG_MCU_OTA(Warn,
                        "OTA RX ignored evt=0x%02X %s phase=%u waiting=%u seq=%u",
                        flag, ota_evt_name(flag),
                        static_cast<unsigned>(phase_),
                        static_cast<unsigned>(waiting_),
                        next_seq_);
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

    case Phase::WaitHeaderAck:
        if(waiting_ == WaitEvent::None && next_seq_ == 0) {
            (void)send_chunk(tcp_send);
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
