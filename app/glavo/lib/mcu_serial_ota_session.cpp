#include "mcu_serial_ota_session.hpp"
#include "device_config.hpp"
#include "protocol.hpp"
#include "rs_log.hpp"
#include "ota_image_pair.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

namespace upc::glavo {

namespace {

const char* basename_only(const std::string& path)
{
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path.c_str() : path.c_str() + pos + 1;
}

constexpr int kReqTimeoutMs        = 1000;
constexpr int kReqMaxRetry         = 3;      /* 首发后再重发次数 */
constexpr int kFirstAckTimeoutMs   = 90000;  /* 擦除窗：不重发 */
constexpr int kChunkTimeoutMs      = 500;
constexpr int kChunkMaxRetry       = 5;
constexpr int kFinishedTimeoutMs   = 2000;
constexpr int kFinishedMaxRetry    = 3;
constexpr int kRebootWaitMs        = 15000;
constexpr int kVersionPollMs       = 2500;
constexpr int kVersionPollTotalMs  = 90000;
constexpr int kFailCooldownMs      = 6000;

constexpr uint8_t kOtaStateTesting   = 3;
constexpr uint8_t kOtaStateConfirmed = 4;
constexpr uint8_t kOtaStateFailed    = 5;

uint32_t read_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/** 文件名 SlotA→1、SlotB→2，与版本上报 value[5] 一致。 */
bool slot_id_from_path(const std::string& path, uint8_t* out)
{
    if(!out) {
        return false;
    }
    upc::OtaImageSlot slot{};
    upc::OtaImageVersion ver{};
    if(!upc::parse_ota_image_basename(basename_only(path), upc::OtaImageKind::Mcu,
                                      slot, ver)) {
        return false;
    }
    *out = (slot == upc::OtaImageSlot::A) ? 1u : 2u;
    return true;
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

void McuSerialOtaSession::reset_attempt()
{
    send_attempts_ = 0;
    deadline_ = {};
}

void McuSerialOtaSession::arm_deadline(int timeout_ms)
{
    deadline_ = Clock::now() + std::chrono::milliseconds(timeout_ms);
}

bool McuSerialOtaSession::deadline_expired() const
{
    return Clock::now() >= deadline_;
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
    target_run_slot_ = 0;
    chunk_inflight_ = false;
    finished_sent_ = false;
    inflight_seq_ = 0;
    reset_attempt();
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

    upc::OtaImagePair pair{};
    const upc::OtaImagePairError err =
        upc::scan_highest_ota_image_pair(dir, upc::OtaImageKind::Mcu, pair);
    if(err != upc::OtaImagePairError::Ok) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA mcu pair select failed dir=%s err=%s",
                        dir, upc::ota_image_pair_error_cstr(err));
        return false;
    }

    candidate_paths_.push_back(pair.slot_a_path);
    candidate_paths_.push_back(pair.slot_b_path);

    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA mcu pair V%u.%u.%u SlotA→SlotB in %s",
                    pair.version.major, pair.version.minor, pair.version.patch,
                    dir);
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

    const uint32_t chunk = static_cast<uint32_t>(MCU_OTA_CHUNK_SIZE);
    const auto file_size = static_cast<uint32_t>(st.st_size);
    /* 打包工具输出 = 128 头 + N×128 固件；裸 Keil .bin 会落到这里。 */
    if(chunk == 0 || (file_size % chunk) != 0) {
        ::close(fd);
        UPC_LOG_MCU_OTA(Error,
                        "SERIAL_OTA reject unpackaged bin %s size=%u (need N×%u)",
                        basename_only(path), file_size, chunk);
        return false;
    }

    uint8_t hdr[16]{};
    if(::pread(fd, hdr, sizeof(hdr), 0) != static_cast<ssize_t>(sizeof(hdr))) {
        ::close(fd);
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA read header fail %s", path.c_str());
        return false;
    }
    uint8_t name_slot = 0;
    if(!slot_id_from_path(path, &name_slot)) {
        ::close(fd);
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA slot parse fail %s", path.c_str());
        return false;
    }
    const uint32_t write_slot = read_le32(hdr);
    if(write_slot != static_cast<uint32_t>(name_slot)) {
        ::close(fd);
        UPC_LOG_MCU_OTA(Error,
                        "SERIAL_OTA slot mismatch %s name=%u header_write_slot=%u",
                        basename_only(path), name_slot, write_slot);
        return false;
    }

    candidate_index_ = index;
    file_fd_ = fd;
    bin_path_ = path;
    file_size_ = file_size;
    total_chunks_ = file_size_ / chunk;
    next_chunk_index_ = 0;
    target_run_slot_ = name_slot;
    chunk_inflight_ = false;
    finished_sent_ = false;
    inflight_seq_ = 0;
    reset_attempt();

    UPC_LOG_MCU_OTA(Info,
                    "SERIAL_OTA open candidate[%zu/%zu] %s size=%u chunks=%u slot=%u",
                    index + 1,
                    candidate_paths_.size(),
                    basename_only(bin_path_),
                    file_size_,
                    total_chunks_,
                    target_run_slot_);
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
    reset_attempt();
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
    request_sent_ = false;
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
    inflight_seq_ = 0;
    request_sent_ = true;
    ++send_attempts_;
    arm_deadline(kReqTimeoutMs);
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA TX request (CMD=0x30) attempt=%u", send_attempts_);
    return true;
}

bool McuSerialOtaSession::send_next_chunk(int (*serial_send)(const uint8_t*, size_t))
{
    if(file_fd_ < 0 || next_chunk_index_ >= total_chunks_) {
        return false;
    }
    const uint32_t chunk = static_cast<uint32_t>(MCU_OTA_CHUNK_SIZE);
    const uint32_t offset = next_chunk_index_ * chunk;
    if(offset + chunk > file_size_) {
        return false;
    }

    uint8_t payload[MCU_OTA_CHUNK_SIZE];
    const ssize_t rd = ::pread(file_fd_, payload, chunk, static_cast<off_t>(offset));
    if(rd != static_cast<ssize_t>(chunk)) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA read fail idx=%u", next_chunk_index_);
        return false;
    }

    const uint8_t seq = static_cast<uint8_t>(next_chunk_index_ & 0xFFu);
    uint8_t frame[192];
    const int flen = serial_ota_build_data(seq, payload, static_cast<uint8_t>(chunk),
                                           frame, sizeof(frame));
    if(flen <= 0 || !serial_send || serial_send(frame, static_cast<size_t>(flen)) != 0) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA send chunk fail idx=%u", next_chunk_index_);
        return false;
    }

    inflight_seq_ = seq;
    chunk_inflight_ = true;
    ++send_attempts_;
    if(next_chunk_index_ == 0) {
        notify(middleware::McuOtaState::Preparing, 0);
        arm_deadline(kFirstAckTimeoutMs);
    } else {
        notify(middleware::McuOtaState::Transferring, progress_u8());
        arm_deadline(kChunkTimeoutMs);
    }
    UPC_LOG_MCU_OTA(Debug, "SERIAL_OTA TX chunk idx=%u seq=%u len=%u attempt=%u file=%s",
                    next_chunk_index_, seq, chunk, send_attempts_,
                    basename_only(bin_path_));
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
    inflight_seq_ = 0;
    finished_sent_ = true;
    ++send_attempts_;
    arm_deadline(kFinishedTimeoutMs);
    notify(middleware::McuOtaState::Verifying, 255);
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA TX finished (TLV=0x04) attempt=%u", send_attempts_);
    return true;
}

bool McuSerialOtaSession::send_version_query(int (*serial_send)(const uint8_t*, size_t))
{
    uint8_t frame[32];
    const int len = serial_build_project_info_request(frame, sizeof(frame));
    if(len <= 0 || !serial_send || serial_send(frame, static_cast<size_t>(len)) != 0) {
        UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA version query TX fail");
        return false;
    }
    arm_deadline(kVersionPollMs);
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA TX GET_VERSION");
    return true;
}

void McuSerialOtaSession::enter_phase4_confirm()
{
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA enter phase4: wait %d ms then poll version slot=%u",
                    kRebootWaitMs, target_run_slot_);
    phase_ = Phase::WaitReboot;
    chunk_inflight_ = false;
    finished_sent_ = true;
    notify(middleware::McuOtaState::Verifying, 255);
    arm_deadline(kRebootWaitMs);
}

void McuSerialOtaSession::enter_cooldown_then_fail()
{
    UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA cooldown %d ms before fail (MCU session idle)",
                    kFailCooldownMs);
    phase_ = Phase::Cooldown;
    arm_deadline(kFailCooldownMs);
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

void McuSerialOtaSession::apply_version_report(uint8_t run_slot, uint8_t ota_state)
{
    UPC_LOG_MCU_OTA(Info, "SERIAL_OTA version slot=%u ota_state=%u target=%u",
                    run_slot, ota_state, target_run_slot_);
    if(ota_state == kOtaStateFailed) {
        UPC_LOG_MCU_OTA(Error, "SERIAL_OTA phase4 FAILED (MCU rolled back)");
        finish(middleware::McuOtaState::Failed, 255);
        return;
    }
    if(ota_state == kOtaStateConfirmed && run_slot == target_run_slot_) {
        UPC_LOG_MCU_OTA(Info, "SERIAL_OTA phase4 CONFIRMED slot=%u", run_slot);
        finish(middleware::McuOtaState::Success, 255);
        return;
    }
    if(ota_state == kOtaStateTesting) {
        UPC_LOG_MCU_OTA(Info, "SERIAL_OTA phase4 TESTING, keep polling");
    }
}

bool McuSerialOtaSession::on_serial_rx_frame(const uint8_t* frame, size_t len)
{
    if(!is_active()) {
        return false;
    }

    if(phase_ == Phase::PollVersion) {
        serial_project_info_t info{};
        if(serial_parse_project_info_report(frame, len, &info) != 0) {
            return false;
        }
        apply_version_report(info.run_slot, info.ota_state);
        /* 终态后交给 McuLink 再解析一遍，About 能拿到新版本。 */
        return is_active();
    }

    serial_ota_reply_t rep{};
    if(serial_ota_parse_reply(frame, len, &rep) != 0) {
        return false;
    }

    if(phase_ == Phase::WaitReqResult &&
       rep.port == SERIAL_OTA_PORT_GENERAL &&
       rep.cmd == SERIAL_OTA_CMD_REQUEST &&
       rep.tlv_type == SERIAL_OTA_TLV_REQ_RESULT &&
       rep.seq == inflight_seq_ &&
       rep.tlv_len >= 1) {
        if(rep.value0 == 1) {
            UPC_LOG_MCU_OTA(Info, "SERIAL_OTA request accepted");
            phase_ = Phase::WaitFirstAck;
            chunk_inflight_ = false;
            reset_attempt();
            notify(middleware::McuOtaState::Preparing, 0);
        } else {
            UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA request rejected by MCU (not packet loss)");
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
            reset_attempt();
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
            if(rep.value0 != 1) {
                UPC_LOG_MCU_OTA(Warn, "SERIAL_OTA chunk ack=0 seq=%u attempt=%u",
                                rep.seq, send_attempts_);
                if(send_attempts_ > static_cast<uint32_t>(kChunkMaxRetry)) {
                    enter_cooldown_then_fail();
                } else {
                    chunk_inflight_ = false;
                }
                return true;
            }
            chunk_inflight_ = false;
            reset_attempt();
            ++next_chunk_index_;
            notify(middleware::McuOtaState::Transferring, progress_u8());
            return true;
        }
    }

    if(phase_ == Phase::WaitFinishedAck &&
       rep.port == SERIAL_OTA_PORT_TASK &&
       rep.cmd == SERIAL_OTA_CMD_PROCESSING &&
       rep.tlv_type == SERIAL_OTA_TLV_FINISHED_ACK &&
       rep.seq == inflight_seq_ &&
       rep.tlv_len >= 1) {
        if(rep.value0 == 1) {
            UPC_LOG_MCU_OTA(Info, "SERIAL_OTA finish ack=1 (download only, not success)");
            enter_phase4_confirm();
        } else {
            UPC_LOG_MCU_OTA(Error, "SERIAL_OTA finish ack failed (len/CRC)");
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
        if(!request_sent_) {
            if(!send_request(serial_send)) {
                finish(middleware::McuOtaState::Failed, 0);
            }
            break;
        }
        if(deadline_expired()) {
            if(send_attempts_ > static_cast<uint32_t>(kReqMaxRetry)) {
                UPC_LOG_MCU_OTA(Error, "SERIAL_OTA request timeout, MCU no response");
                finish(middleware::McuOtaState::Failed, 0);
            } else if(!send_request(serial_send)) {
                finish(middleware::McuOtaState::Failed, 0);
            }
        }
        break;
    case Phase::WaitFirstAck:
        if(!chunk_inflight_) {
            if(!send_next_chunk(serial_send)) {
                finish(middleware::McuOtaState::Failed, progress_u8());
            }
            break;
        }
        if(deadline_expired()) {
            UPC_LOG_MCU_OTA(Error, "SERIAL_OTA first packet timeout, do not resend");
            finish(middleware::McuOtaState::Failed, progress_u8());
        }
        break;
    case Phase::Streaming:
        if(next_chunk_index_ >= total_chunks_) {
            phase_ = Phase::WaitFinishedAck;
            finished_sent_ = false;
            reset_attempt();
            break;
        }
        if(!chunk_inflight_) {
            if(!send_next_chunk(serial_send)) {
                finish(middleware::McuOtaState::Failed, progress_u8());
            }
            break;
        }
        if(deadline_expired()) {
            if(send_attempts_ > static_cast<uint32_t>(kChunkMaxRetry)) {
                UPC_LOG_MCU_OTA(Error, "SERIAL_OTA chunk retry exhausted idx=%u",
                                next_chunk_index_);
                enter_cooldown_then_fail();
            } else if(!send_next_chunk(serial_send)) {
                finish(middleware::McuOtaState::Failed, progress_u8());
            }
        }
        break;
    case Phase::WaitFinishedAck:
        if(!finished_sent_) {
            if(!send_finished(serial_send)) {
                finish(middleware::McuOtaState::Failed, 255);
            }
            break;
        }
        if(deadline_expired()) {
            if(send_attempts_ > static_cast<uint32_t>(kFinishedMaxRetry)) {
                UPC_LOG_MCU_OTA(Warn,
                    "SERIAL_OTA finished no ack after retries, poll version");
                enter_phase4_confirm();
            } else if(!send_finished(serial_send)) {
                finish(middleware::McuOtaState::Failed, 255);
            }
        }
        break;
    case Phase::WaitReboot:
        if(deadline_expired()) {
            UPC_LOG_MCU_OTA(Info, "SERIAL_OTA reboot wait done, start version poll");
            phase_ = Phase::PollVersion;
            poll_until_ = Clock::now() + std::chrono::milliseconds(kVersionPollTotalMs);
            deadline_ = Clock::now();
        }
        break;
    case Phase::PollVersion:
        if(Clock::now() >= poll_until_) {
            UPC_LOG_MCU_OTA(Error, "SERIAL_OTA phase4 poll timeout");
            finish(middleware::McuOtaState::Failed, 255);
            break;
        }
        if(deadline_expired()) {
            (void)send_version_query(serial_send);
        }
        break;
    case Phase::Cooldown:
        if(deadline_expired()) {
            finish(middleware::McuOtaState::Failed, progress_u8());
        }
        break;
    default:
        break;
    }
}

} // namespace upc::glavo
