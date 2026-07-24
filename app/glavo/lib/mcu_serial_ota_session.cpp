#include "mcu_serial_ota_session.hpp"
#include "device_config.hpp"
#include "protocol.hpp"
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

bool file_newer(const std::string& a, const std::string& b)
{
    struct stat sa{};
    struct stat sb{};
    if(::stat(a.c_str(), &sa) != 0 || ::stat(b.c_str(), &sb) != 0) {
        return a < b;
    }
    if(sa.st_mtime != sb.st_mtime) {
        return sa.st_mtime > sb.st_mtime;
    }
    return a < b;
}

} // namespace

void McuSerialOtaSession::set_status_callback(McuOtaStatusCb cb)
{
    status_cb_ = std::move(cb);
}

bool McuSerialOtaSession::is_active() const noexcept
{
    return phase_ != Phase::None && phase_ != Phase::Done &&
           phase_ != Phase::Failed && phase_ != Phase::Cancelled;
}

void McuSerialOtaSession::reset_file_state()
{
    if(file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }
    bin_path_.clear();
    file_size_ = 0;
    total_chunks_ = 0;
    next_chunk_index_ = 0;
    chunk_inflight_ = false;
    finished_sent_ = false;
    inflight_seq_ = 0;
}

void McuSerialOtaSession::reset_candidates()
{
    candidate_paths_.clear();
    candidate_index_ = 0;
}

bool McuSerialOtaSession::scan_bin_candidates(const char* dir)
{
    reset_candidates();
    if(!dir || !dir[0]) {
        return false;
    }

    DIR* dp = ::opendir(dir);
    if(!dp) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA opendir fail %s errno=%d", dir, errno);
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
        struct stat st{};
        if(::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            continue;
        }
        candidate_paths_.push_back(path);
    }
    ::closedir(dp);

    std::sort(candidate_paths_.begin(), candidate_paths_.end(), file_newer);

    if(candidate_paths_.empty()) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA no .bin in %s", dir);
        return false;
    }

    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA found %zu candidate(s) in %s",
                    candidate_paths_.size(), dir);
    for(size_t i = 0; i < candidate_paths_.size(); ++i) {
        UPC_LOG_MCU_OTA(Info, "SERIAL_OTA candidate[%zu] %s",
                        i, basename_only(candidate_paths_[i]));
    }
    return true;
}

bool McuSerialOtaSession::open_candidate(size_t index)
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
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA open fail %s errno=%d", path.c_str(), errno);
        return false;
    }
    struct stat st{};
    if(::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA stat fail %s", path.c_str());
        return false;
    }

    candidate_index_ = index;
    file_fd_ = fd;
    bin_path_ = path;
    file_size_ = static_cast<uint32_t>(st.st_size);
    const uint32_t chunk = static_cast<uint32_t>(MCU_OTA_CHUNK_SIZE);
    total_chunks_ = (file_size_ + chunk - 1u) / chunk;
    next_chunk_index_ = 0;
    chunk_inflight_ = false;
    finished_sent_ = false;
    inflight_seq_ = 0;

    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA open candidate[%zu/%zu] %s size=%u chunks=%u",
                    index + 1,
                    candidate_paths_.size(),
                    basename_only(bin_path_),
                    file_size_,
                    total_chunks_);
    return true;
}

bool McuSerialOtaSession::start(const char* image_dir)
{
    if(is_active()) {
        UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA already active");
        return false;
    }
    const char* dir = (image_dir && image_dir[0]) ? image_dir : MCU_OTA_IMAGE_DIR;
    if(!scan_bin_candidates(dir)) {
        return false;
    }
    if(!open_candidate(0)) {
        reset_candidates();
        return false;
    }

    cancel_requested_.store(false);
    request_sent_ = false;
    phase_ = Phase::WaitReqResult;
    notify(middleware::McuOtaState::Preparing, 0);
    return true;
}

void McuSerialOtaSession::cancel()
{
    if(phase_ == Phase::None) {
        return;
    }
    cancel_requested_.store(true);
}

void McuSerialOtaSession::notify(middleware::McuOtaState state, uint8_t progress)
{
    if(!status_cb_) {
        return;
    }
    middleware::McuOtaStatusPayload p{};
    p.state = state;
    p.progress = progress;
    status_cb_(p);
}

void McuSerialOtaSession::finish(middleware::McuOtaState state, uint8_t progress)
{
    notify(state, progress);
    reset_file_state();
    reset_candidates();
    if(state == middleware::McuOtaState::Cancelled) {
        phase_ = Phase::Cancelled;
    } else if(state == middleware::McuOtaState::Success) {
        phase_ = Phase::Done;
    } else {
        phase_ = Phase::Failed;
    }
}

uint8_t McuSerialOtaSession::progress_u8() const
{
    if(total_chunks_ == 0) {
        return 0;
    }
    if(next_chunk_index_ >= total_chunks_) {
        return 255;
    }
    return static_cast<uint8_t>((next_chunk_index_ * 255u) / total_chunks_);
}

bool McuSerialOtaSession::send_request(int (*serial_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = serial_ota_build_request(0, frame, sizeof(frame));
    if(len <= 0 || !serial_send || serial_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA send request fail");
        return false;
    }
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA TX request (CMD=0x30)");
    request_sent_ = true;
    return true;
}

bool McuSerialOtaSession::send_next_chunk(int (*serial_send)(const uint8_t*, size_t))
{
    if(file_fd_ < 0 || next_chunk_index_ >= total_chunks_) {
        return false;
    }
    const uint32_t chunk = static_cast<uint32_t>(MCU_OTA_CHUNK_SIZE);
    const uint32_t offset = next_chunk_index_ * chunk;
    uint8_t send_len = static_cast<uint8_t>(chunk);
    if(offset + send_len > file_size_) {
        send_len = static_cast<uint8_t>(file_size_ - offset);
    }

    uint8_t payload[MCU_OTA_CHUNK_SIZE];
    const ssize_t rd = ::pread(file_fd_, payload, send_len, static_cast<off_t>(offset));
    if(rd != static_cast<ssize_t>(send_len)) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA read fail idx=%u", next_chunk_index_);
        return false;
    }

    const uint8_t seq = static_cast<uint8_t>(next_chunk_index_ & 0xFFu);
    uint8_t frame[192];
    const int flen = serial_ota_build_data(seq, payload, send_len, frame, sizeof(frame));
    if(flen <= 0 || !serial_send || serial_send(frame, static_cast<size_t>(flen)) != 0) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA send chunk fail idx=%u", next_chunk_index_);
        return false;
    }

    inflight_seq_ = seq;
    chunk_inflight_ = true;
    notify(middleware::McuOtaState::Transferring, progress_u8());
    UPC_LOG_MCU_OTA(Debug, "SERIAL_OTA TX chunk idx=%u seq=%u len=%u file=%s",
                    next_chunk_index_, seq, send_len, basename_only(bin_path_));
    return true;
}

bool McuSerialOtaSession::send_finished(int (*serial_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = serial_ota_build_finished(0, frame, sizeof(frame));
    if(len <= 0 || !serial_send || serial_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA send finished fail");
        return false;
    }
    finished_sent_ = true;
    notify(middleware::McuOtaState::Verifying, 255);
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA TX finished (TLV=0x04)");
    return true;
}

void McuSerialOtaSession::on_first_packet_rejected()
{
    UPC_LOG_MCU_OTA(Warn,
                    "SERIAL_OTA first packet reject file=%s attempt %zu/%zu",
                    basename_only(bin_path_),
                    candidate_index_ + 1,
                    candidate_paths_.size());

    const size_t next_index = candidate_index_ + 1;
    if(next_index >= candidate_paths_.size()) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA failed: all candidates rejected");
        finish(middleware::McuOtaState::Failed, progress_u8());
        return;
    }
    if(!open_candidate(next_index)) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA open next candidate fail");
        finish(middleware::McuOtaState::Failed, progress_u8());
        return;
    }
    phase_ = Phase::WaitFirstAck;
    notify(middleware::McuOtaState::Preparing, 0);
}

bool McuSerialOtaSession::on_serial_rx_frame(const uint8_t* frame, size_t len)
{
    if(!is_active()) {
        return false;
    }
    serial_ota_reply_t rep{};
    if(serial_ota_parse_reply(frame, len, &rep) != 0) {
        return false;
    }

    if(phase_ == Phase::WaitReqResult &&
       rep.port == SERIAL_OTA_PORT_GENERAL &&
       rep.cmd == SERIAL_OTA_CMD_REQUEST &&
       rep.tlv_type == SERIAL_OTA_TLV_REQ_RESULT &&
       rep.tlv_len >= 1) {
        if(rep.value0 == 1) {
            UPC_LOG_MCU_OTA(Info, "SERIAL_OTA request accepted");
            phase_ = Phase::WaitFirstAck;
            chunk_inflight_ = false;
            notify(middleware::McuOtaState::Preparing, 0);
        } else {
            UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA request rejected by MCU");
            finish(middleware::McuOtaState::Failed, 0);
        }
        return true;
    }

    if((phase_ == Phase::WaitFirstAck || phase_ == Phase::Streaming) &&
       rep.port == SERIAL_OTA_PORT_TASK &&
       rep.cmd == SERIAL_OTA_CMD_PROCESSING &&
       rep.seq == inflight_seq_ && rep.tlv_len >= 1) {
        if(phase_ == Phase::WaitFirstAck && rep.tlv_type == SERIAL_OTA_TLV_JUDGMENT_SEQ) {
            chunk_inflight_ = false;
            if(rep.value0 == 1) {
                ++next_chunk_index_;
                phase_ = Phase::Streaming;
                notify(middleware::McuOtaState::Transferring, progress_u8());
            } else {
                on_first_packet_rejected();
            }
            return true;
        }

        if(phase_ == Phase::Streaming && rep.tlv_type == SERIAL_OTA_TLV_PACKS_ACK) {
            chunk_inflight_ = false;
            if(rep.value0 != 1) {
                UPC_LOG_MCU_OTA(Error, "SERIAL_OTA chunk ack fail seq=%u", rep.seq);
                finish(middleware::McuOtaState::Failed, progress_u8());
                return true;
            }
            ++next_chunk_index_;
            notify(middleware::McuOtaState::Transferring, progress_u8());
            return true;
        }
    }

    if(phase_ == Phase::WaitFinishedAck &&
       rep.port == SERIAL_OTA_PORT_TASK &&
       rep.cmd == SERIAL_OTA_CMD_PROCESSING &&
       rep.tlv_type == SERIAL_OTA_TLV_FINISHED_ACK &&
       rep.tlv_len >= 1) {
        if(rep.value0 == 1) {
            UPC_LOG_MCU_OTA(Info, "SERIAL_OTA finish ack success");
            finish(middleware::McuOtaState::Success, 255);
        } else {
            UPC_LOG_MCU_OTA(Error, "SERIAL_OTA finish ack failed");
            finish(middleware::McuOtaState::Failed, 255);
        }
        return true;
    }
    return false;
}

void McuSerialOtaSession::tick(int (*serial_send)(const uint8_t*, size_t), bool serial_connected)
{
    if(cancel_requested_.load()) {
        UPC_LOG_MCU_OTA(Info, "SERIAL_OTA cancelled");
        finish(middleware::McuOtaState::Cancelled, progress_u8());
        cancel_requested_.store(false);
        return;
    }

    if(!is_active()) {
        return;
    }
    if(!serial_connected) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA serial disconnected");
        finish(middleware::McuOtaState::Failed, progress_u8());
        return;
    }

    switch(phase_) {
    case Phase::WaitReqResult:
        if(!request_sent_ && !send_request(serial_send)) {
            finish(middleware::McuOtaState::Failed, 0);
        }
        break;
    case Phase::WaitFirstAck:
    case Phase::Streaming:
        if(next_chunk_index_ >= total_chunks_) {
            phase_ = Phase::WaitFinishedAck;
            break;
        }
        if(!chunk_inflight_ && !send_next_chunk(serial_send)) {
            finish(middleware::McuOtaState::Failed, progress_u8());
        }
        break;
    case Phase::WaitFinishedAck:
        if(!finished_sent_ && !send_finished(serial_send)) {
            finish(middleware::McuOtaState::Failed, 255);
        }
        break;
    default:
        break;
    }
}

} // namespace upc::glavo
