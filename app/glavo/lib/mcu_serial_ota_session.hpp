#ifndef MCU_SERIAL_OTA_SESSION_HPP
#define MCU_SERIAL_OTA_SESSION_HPP

#include "mcu_ota_session.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace upc::glavo {

/** 串口 MCU OTA：/userdata/image/mcu 最高版本 MainBoard AB 对（见 OTA_IMAGE.md）
 *
 * 停等协议与超时/重发见 doc/MCU_OTA丢包重传机制.md。成功以阶段四版本查询为准，
 * FINISHED_ACK=1 只表示镜像已落槽并即将复位，不等于升级成功。
 */
class McuSerialOtaSession {
public:
    McuSerialOtaSession() = default;

    void set_status_callback(McuOtaStatusCb cb);

    bool start(const char* image_dir);
    void cancel();
    void tick(int (*serial_send)(const uint8_t*, size_t), bool serial_connected);
    bool on_serial_rx_frame(const uint8_t* frame, size_t len);

    [[nodiscard]] bool is_active() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    enum class Phase : uint8_t {
        None,
        WaitReqResult,
        WaitFirstAck,
        Streaming,
        WaitFinishedAck,
        WaitReboot,
        PollVersion,
        Cooldown,
        Done,
        Failed,
        Cancelled,
    };

    void reset_file_state();
    void reset_candidates();
    void reset_attempt();
    void arm_deadline(int timeout_ms);
    [[nodiscard]] bool deadline_expired() const;
    bool scan_bin_candidates(const char* dir);
    bool open_candidate(size_t index);
    bool send_request(int (*serial_send)(const uint8_t*, size_t));
    bool send_next_chunk(int (*serial_send)(const uint8_t*, size_t));
    bool send_finished(int (*serial_send)(const uint8_t*, size_t));
    bool send_version_query(int (*serial_send)(const uint8_t*, size_t));
    void on_first_packet_rejected();
    void enter_phase4_confirm();
    void enter_cooldown_then_fail();
    void apply_version_report(uint8_t run_slot, uint8_t ota_state);
    void notify(middleware::McuOtaState state, uint8_t progress);
    void finish(middleware::McuOtaState state, uint8_t progress);
    [[nodiscard]] uint8_t progress_u8() const;

    McuOtaStatusCb status_cb_;
    Phase phase_{Phase::None};
    std::atomic<bool> cancel_requested_{false};

    std::vector<std::string> candidate_paths_;
    size_t candidate_index_{0};
    int file_fd_{-1};
    std::string bin_path_;
    uint32_t file_size_{0};
    uint32_t total_chunks_{0};
    uint32_t next_chunk_index_{0};
    uint8_t target_run_slot_{0};

    bool request_sent_{false};
    bool chunk_inflight_{false};
    bool finished_sent_{false};
    uint8_t inflight_seq_{0};
    uint32_t send_attempts_{0};
    Clock::time_point deadline_{};
    Clock::time_point poll_until_{};
};

} // namespace upc::glavo

#endif
