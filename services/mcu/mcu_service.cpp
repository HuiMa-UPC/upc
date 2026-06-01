#include "mcu_service.hpp"
#include "print_job_state.hpp"
#include "tcp_sender.hpp"
#include "msg_types.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>

namespace services {

MCUService::MCUService()
    : ServiceBase(ServiceConfig{
          "mcu",
          static_cast<uint32_t>(middleware::ModuleId::APP_MCU),
          false
      }) {}

MCUService::~MCUService() {
    do_stop();
}

bool MCUService::do_init() {
    return true;
}

bool MCUService::do_start() {
    timer_running_.store(true);

    subscribe(middleware::MsgType::UI_PM25_Req,
        [this](const middleware::Message& msg) { on_pm25_request(msg); });
    subscribe(middleware::MsgType::UI_CLOSE_DOOR_Req,
        [this](const middleware::Message& msg) { on_close_door_request(msg); });
    subscribe(middleware::MsgType::UI_OpenDoor_Req,
        [this](const middleware::Message& msg) { on_open_door_request(msg); });
    subscribe(middleware::MsgType::UI_Purify_Req,
        [this](const middleware::Message& msg) { on_purify_request(msg); });

    timer_thread_ = std::thread(&MCUService::timer_thread_fn, this);
    return true;
}

bool MCUService::do_stop() {
    timer_running_.store(false);
    purify_active_.store(false);
    notify_timer();

    if(timer_thread_.joinable()) {
        timer_thread_.join();
    }
    return true;
}

void MCUService::handle_message(const middleware::Message& msg) {
    switch (msg.type) {
        case middleware::MsgType::UI_PM25_Req:
            on_pm25_request(msg);
            break;
        case middleware::MsgType::UI_CLOSE_DOOR_Req:
            on_close_door_request(msg);
            break;
        case middleware::MsgType::UI_OpenDoor_Req:
            on_open_door_request(msg);
            break;
        case middleware::MsgType::UI_Purify_Req:
            on_purify_request(msg);
            break;
        default:
            break;
    }
}

void MCUService::on_pm25_request(const middleware::Message& msg) {
    middleware::Pm25ResultPayload p{};
    p.is_high = 0;
    p.pm25_cabin = 12.5f;

    middleware::Message rsp(middleware::MsgType::APP_PM25_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(rsp);
}

void MCUService::on_close_door_request(const middleware::Message& msg) {
    middleware::DoorResultPayload p{};
    p.ok = 1;

    middleware::Message rsp(middleware::MsgType::APP_CLOSE_DOOR_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(rsp);
}

void MCUService::on_open_door_request(const middleware::Message& msg) {
    middleware::DoorResultPayload p{};
    p.ok = 1;

    middleware::Message rsp(middleware::MsgType::APP_OpenDoor_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(rsp);
}

void MCUService::on_purify_request(const middleware::Message& msg) {
    if (purify_active_.exchange(true)) {
        return;
    }
    purify_progress_ = 0;
    purify_sender_id_ = msg.sender_id;
    notify_timer();
}

void MCUService::timer_thread_fn() {
    using namespace std::chrono;
    const auto env_interval    = seconds(2);
    const auto purify_interval = milliseconds(200);

    auto next_env    = steady_clock::now() + env_interval;
    auto next_purify = steady_clock::now();

    while (timer_running_.load()) {
        std::unique_lock<std::mutex> lock(timer_mtx_);
        auto next_wake = next_env;
        if(purify_active_.load()) {
            next_wake = std::min(next_wake, next_purify);
        }

        timer_cv_.wait_until(lock, next_wake, [this]() {
            return !timer_running_.load() || purify_active_.load();
        });

        if (!timer_running_.load()) {
            break;
        }

        auto now = steady_clock::now();

        if(now >= next_env) {
            send_env_snapshot();
            next_env = now + env_interval;
        }

        if (purify_active_.load() && now >= next_purify) {
            send_purify_progress(purify_sender_id_, purify_progress_);
            purify_progress_ += 5;

            if (purify_progress_ > 100) {
                purify_active_.store(false);
                send_purify_done(purify_sender_id_);
            }
            next_purify = now + purify_interval;
        }
    }
}

void MCUService::notify_timer() {
    std::lock_guard<std::mutex> lock(timer_mtx_);
    timer_cv_.notify_one();
}

void MCUService::send_env_snapshot() {
    ++env_tick_;

    env_temp_ += (static_cast<float>(std::rand() % 100) - 50.0f) * 0.002f;
    env_temp_ = std::fmax(20.0f, std::fmin(30.0f, env_temp_));

    env_pm_indoor_ += (static_cast<float>(std::rand() % 100) - 48.0f) * 0.05f;
    env_pm_indoor_ = std::fmax(5.0f, std::fmin(80.0f, env_pm_indoor_));

    env_pm_cabin_ += (static_cast<float>(std::rand() % 100) - 50.0f) * 0.02f;
    env_pm_cabin_ = std::fmax(2.0f, std::fmin(25.0f, env_pm_cabin_));

    env_oxygen_ += (static_cast<float>(std::rand() % 100) - 50.0f) * 0.001f;
    env_oxygen_ = std::fmax(19.0f, std::fmin(22.0f, env_oxygen_));

    int signal = 3;
    if ((env_tick_ % 30) == 0) {
        signal = 1 + (std::rand() % 4);
    }

    const auto job = PrintJobState::instance().snapshot();
    middleware::UserHomeStatus status = middleware::UserHomeStatus::Idle;
    if(job.is_sending) {
        status = middleware::UserHomeStatus::Printing;
    } else if(job.job_error) {
        status = middleware::UserHomeStatus::Error;
    } else if(job.job_completed) {
        status = middleware::UserHomeStatus::Ready;
    } else if(TcpSender::instance().is_tcp_connected()) {
        status = middleware::UserHomeStatus::Ready;
    }

    middleware::EnvSnapshotPayload p{};
    std::snprintf(p.device_name, sizeof(p.device_name), "%s", "Rongsu-3D");
    p.status         = static_cast<uint32_t>(status);
    p.wifi_connected = TcpSender::instance().is_tcp_connected() ? 1 : 0;
    p.signal_bars    = signal;
    p.pm25_indoor    = env_pm_indoor_;
    p.pm25_cabin     = env_pm_cabin_;
    p.temperature_c  = env_temp_;
    p.oxygen_percent = env_oxygen_;

    middleware::Message ntf(middleware::MsgType::APP_Update_Ntf,
        static_cast<middleware::ModuleId>(module_id()),
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)publish_async(ntf);
}

void MCUService::send_purify_progress(middleware::ModuleId sender, int progress) {
    middleware::Message prog(middleware::MsgType::APP_Purify_Progress,
        static_cast<middleware::ModuleId>(module_id()), sender);
    prog.payload = middleware::MsgPayload::from_value(static_cast<uint32_t>(progress));
    (void)publish_async(prog);
}

void MCUService::send_purify_done(middleware::ModuleId sender) {
    middleware::Message done(middleware::MsgType::APP_Purify_Rsp,
        static_cast<middleware::ModuleId>(module_id()), sender);
    (void)publish_async(done);
}

} // namespace services
