#ifndef MCU_OTA_SESSION_HPP
#define MCU_OTA_SESSION_HPP

#include "tcp_rx.hpp"
#include "msg_runtime.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace upc::glavo {

using McuOtaStatusCb = std::function<void(const middleware::McuOtaStatusPayload&)>;

/**
 * @brief MCU 以太网 OTA 会话（一问一答，测试用）
 *
 * /userdata/image/galvo 只刷最高版本 Galvo AB 对（见 OTA_IMAGE.md）；MCU 0x03 就绪后发 seq=0 首包，
 * 等 0x0A（正确）或 0x0B（换 SlotB 并重发 0x03 BEGIN）；seq>=1 每包等 0x23。
 * WaitReady/WaitHeaderAck/Streaming 期间抑制空闲心跳 TX（FRAME CMD=0x0B）。
 * 全部 DATA 发完后：发心跳 → 收应答 → 发 0x03 Mode=0x02 → 等 MCU 0x13 完成。
 */
class McuOtaSession {
public:
    McuOtaSession() = default;

    void set_status_callback(McuOtaStatusCb cb);

    bool start(const char* image_dir);
    void cancel();
    void tick(int (*tcp_send)(const uint8_t*, size_t),
              void (*tcp_drain_rx)(),
              bool tcp_connected);

    void on_status_response(const tcp_response_t& resp);

    [[nodiscard]] bool is_active() const noexcept;
    [[nodiscard]] bool suppress_idle_heartbeat() const noexcept;

    /** OTA Success 后由 McuLink 取走一次，触发链路线程 TCP 重连 */
    [[nodiscard]] bool take_post_success_reconnect() noexcept;

private:
    enum class Phase : uint8_t {
        None,
        WaitReady,
        WaitHeaderAck,
        Streaming,
        WaitHbAfterLast,
        WaitComplete,
        Done,
        Failed,
        Cancelled,
    };

    enum class WaitEvent : uint8_t {
        None,
        Ready,
        HeaderAck,
        ChunkAck,
        Complete,
    };

    void reset_file_state();
    void reset_candidates();
    bool scan_bin_candidates(const char* dir);
    bool open_candidate(size_t index);
    void on_header_rejected(uint8_t progress);
    void notify(middleware::McuOtaState state, uint8_t progress);
    void finish(middleware::McuOtaState state, uint8_t progress);
    bool send_ota_ctrl(uint8_t mode, int (*tcp_send)(const uint8_t*, size_t));
    bool send_heartbeat(int (*tcp_send)(const uint8_t*, size_t));
    bool send_chunk(int (*tcp_send)(const uint8_t*, size_t));
    void handle_event(uint8_t flag, uint8_t progress);

    McuOtaStatusCb status_cb_;
    Phase phase_{Phase::None};
    WaitEvent waiting_{WaitEvent::None};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> post_success_reconnect_{false};

    std::vector<std::string> candidate_paths_;
    size_t candidate_index_{0};

    int file_fd_{-1};
    std::string bin_path_;
    uint32_t file_size_{0};
    uint32_t total_chunks_{0};
    uint32_t next_seq_{0};
    bool hb_sent_after_last_{false};
    bool end_sent_{false};
    uint8_t last_progress_{0};
};

} // namespace upc::glavo

#endif
