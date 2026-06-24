#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

/** Glavo 侧 IPC API：处理 UI 请求、推送 Ntf/Rsp。契约见 common/contract/README.md */

#include "mcu_link.hpp"
#include "msg_port.hpp"
#include "msg_types.hpp"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace upc::glavo {

class GlavoService {
public:
    GlavoService();
    ~GlavoService();

    GlavoService(const GlavoService&) = delete;
    GlavoService& operator=(const GlavoService&) = delete;

    // 生命周期
    bool init();
    bool start();
    bool stop();

private:
    enum class ServiceState {
        Idle,
        Initializing,
        Running,
        Stopping,
        Error
    };

    void handle_message(const middleware::Message& msg);
    void handle_dir_open(const middleware::Message& msg);
    void handle_file_info_req(const middleware::Message& msg);
    void handle_print_start(const middleware::Message& msg);
    void handle_print_terminate(const middleware::Message& msg);
    void handle_pm25_request(const middleware::Message& msg);
    void handle_close_door_request(const middleware::Message& msg);
    void handle_open_door_request(const middleware::Message& msg);
    void handle_purify_request(const middleware::Message& msg);

    void send_door_rsp(const middleware::Message& msg, middleware::MsgType rsp_type);
    void send_dir_open_empty(const middleware::Message& req);
    void stop_print_job();
    void on_mcu_link_event(McuLinkEvent ev);
    void on_motion_batch_sent();
    void publish_terminate_done();
    void reset_print_progress_state();
    void send_print_progress(bool force_complete);

    // 消息入口 (由 MsgBus 订阅回调，转发至工作线程)
    void on_message(const middleware::Message& msg);
    void worker_loop();
    [[nodiscard]] bool publish_async(const middleware::Message& msg);
    void subscribe(middleware::MsgType type, middleware::MsgHandler handler);
    [[nodiscard]] ServiceState state() const noexcept;
    [[nodiscard]] static uint32_t module_id() noexcept;

    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out) {
        if (payload.type != middleware::MsgPayload::Type::Inline ||
            payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, payload.data.bytes.data(), sizeof(T));
        return true;
    }

    std::string make_full_path(const std::string& dir, const std::string& name);
    static std::string payload_to_string(const middleware::MsgPayload& payload);

    std::atomic<ServiceState> state_{ServiceState::Idle};
    std::vector<middleware::Subscription> subscriptions_;

    // 工作线程：IPC 业务在工作线程中处理，回 UI 须 publish_async
    std::thread worker_thread_;
    std::queue<middleware::Message> worker_queue_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::atomic<bool> worker_running_{false};

    std::string current_dir_path_;
    std::vector<std::string> current_entries_;
    std::string current_file_path_;

    std::atomic<bool> print_running_{false};
    int last_print_pct_     = -1;
    bool last_parse_done_   = false;
};

} // namespace upc::glavo

#endif
