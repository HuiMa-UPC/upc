#ifndef MCU_SERVICE_HPP
#define MCU_SERVICE_HPP

#include "service_base.hpp"
#include <atomic>
#include <thread>

namespace services {

class MCUService : public ServiceBase {
public:
    MCUService();
    ~MCUService();

protected:
    bool do_init() override;
    bool do_start() override;
    bool do_stop() override;
    void handle_message(const middleware::Message& msg) override;

private:
    void on_pm25_request(const middleware::Message& msg);
    void on_close_door_request(const middleware::Message& msg);
    void on_open_door_request(const middleware::Message& msg);
    void on_purify_request(const middleware::Message& msg);

    void env_notify_thread_fn();
    void purify_thread_fn(middleware::ModuleId sender);

    std::atomic<bool> running_{false};
    std::thread env_thread_;

    std::atomic<bool> purify_running_{false};
    std::thread purify_thread_;
};

} // namespace services

#endif // MCU_SERVICE_HPP
