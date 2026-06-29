#ifndef MCU_OTA_SESSION_HPP
#define MCU_OTA_SESSION_HPP

#include "tcp_rx.hpp"
#include "msg_types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace upc::glavo {

using McuOtaStatusCb = std::function<void(const middleware::McuOtaStatusPayload&)>;

/**
 * @brief MCU 以太网 OTA 会话（一问一答，测试用）
 *
 * WaitReady/Streaming 期间抑制空闲 0x0B；全部 DATA 发完后恢复心跳等 0x13。
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

private:
    enum class Phase : uint8_t {
        None,
        WaitReady,
        Streaming,
        WaitComplete,
        Done,
        Failed,
        Cancelled,
    };

    enum class WaitEvent : uint8_t {
        None,
        Ready,
        ChunkAck,
        Complete,
    };

    void reset_file();
    bool open_newest_bin(const char* dir);
    void notify(middleware::McuOtaState state, uint8_t progress);
    void finish(middleware::McuOtaState state, uint8_t progress);
    bool send_begin(int (*tcp_send)(const uint8_t*, size_t));
    bool send_chunk(int (*tcp_send)(const uint8_t*, size_t));
    void handle_event(uint8_t flag, uint8_t progress);

    McuOtaStatusCb status_cb_;
    Phase phase_{Phase::None};
    WaitEvent waiting_{WaitEvent::None};
    std::atomic<bool> cancel_requested_{false};

    int file_fd_{-1};
    std::string bin_path_;
    uint32_t file_size_{0};
    uint32_t total_chunks_{0};
    uint32_t next_seq_{0};
    bool begin_sent_{false};
    uint8_t last_progress_{0};
};

} // namespace upc::glavo

#endif
