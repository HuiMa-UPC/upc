#ifndef GLAVO_SERVICE_HPP
#define GLAVO_SERVICE_HPP

#include "service_base.hpp"
#include "msg_types.hpp"
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
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
    void handle_print_start(const middleware::Message& msg);
    void handle_print_terminate(const middleware::Message& msg);

    void send_dir_open_empty(const middleware::Message& req);
    void send_print_progress(int progress_percent);
    void stop_print_job();
    void print_progress_thread_fn();

    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out) {
        if (payload.type != middleware::MsgPayload::Type::Inline ||
            payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, payload.data.bytes.data(), sizeof(T));
        return true;
    }

    /** 构建完整路径 */
    std::string make_full_path(const std::string& dir, const std::string& name);

    /** 当前打开的目录路径 */
    std::string current_dir_path_;
    /** 当前目录下的文件/目录列表（按索引顺序） */
    std::vector<std::string> current_entries_;

    /** 从 Inline payload 中提取字符串 */
    static std::string payload_to_string(const middleware::MsgPayload& payload);

    std::atomic<bool> print_running_{false};
    std::thread print_progress_thread_;
};

} // namespace services

#endif
