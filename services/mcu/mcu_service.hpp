#ifndef MCU_SERVICE_HPP
#define MCU_SERVICE_HPP

#include "service_base.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

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

    void timer_thread_fn();
    void notify_timer();

    void send_env_snapshot();
    void send_purify_progress(middleware::ModuleId sender, int progress);
    void send_purify_done(middleware::ModuleId sender);

    // 定时器线程
    std::thread timer_thread_;
    std::mutex  timer_mtx_;
    std::condition_variable timer_cv_;
    std::atomic<bool> timer_running_{false};

    // 环境状态（定时器线程维护）
    float env_temp_       = 24.0f;
    float env_pm_indoor_  = 35.0f;
    float env_pm_cabin_   = 8.0f;
    float env_oxygen_     = 21.0f;
    int   env_tick_       = 0;

    // 净化状态
    std::atomic<bool> purify_active_{false};
    int purify_progress_ = 0;
    middleware::ModuleId purify_sender_id_{middleware::ModuleId::UI};
};

} // namespace services

#endif // MCU_SERVICE_HPP
