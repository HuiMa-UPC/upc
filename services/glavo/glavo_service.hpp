#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

#include "service_base.hpp"
#include "tcp_sender.hpp"
#include "msg_types.hpp"
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace services {

class GlavoService : public ServiceBase {
public:
    GlavoService();
    ~GlavoService() override;

protected:
    bool do_init() override;
    bool do_start() override;
    bool do_stop() override;
    void handle_message(const middleware::Message& msg) override;

private:
    void handle_dir_open(const middleware::Message& msg);
    void handle_file_info_req(const middleware::Message& msg);
    void handle_print_start(const middleware::Message& msg);
    void handle_print_terminate(const middleware::Message& msg);
    void handle_pm25_request(const middleware::Message& msg);
    void handle_close_door_request(const middleware::Message& msg);
    void handle_open_door_request(const middleware::Message& msg);
    void handle_purify_request(const middleware::Message& msg);

    void send_dir_open_empty(const middleware::Message& req);
    void stop_print_job();
    void on_tcp_job_event(TcpJobEvent ev);
    void on_motion_batch_sent();
    void publish_terminate_done();
    void reset_print_progress_state();
    void send_print_progress(bool force_complete);

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

    std::string current_dir_path_;
    std::vector<std::string> current_entries_;
    std::string current_file_path_;

    std::atomic<bool> print_running_{false};
    int last_print_pct_     = -1;
    bool last_parse_done_   = false;
};

} // namespace services

#endif
