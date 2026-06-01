#include "mcu_service.hpp"
#include "msg_types.hpp"
#include "msg_bus.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <thread>

namespace services {

/* ---------- 生命周期 ---------- */

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
    running_.store(true);

    subscribe(middleware::MsgType::UI_PM25_Req,
        [this](const middleware::Message& msg) { on_pm25_request(msg); });
    subscribe(middleware::MsgType::UI_CLOSE_DOOR_Req,
        [this](const middleware::Message& msg) { on_close_door_request(msg); });
    subscribe(middleware::MsgType::UI_OpenDoor_Req,
        [this](const middleware::Message& msg) { on_open_door_request(msg); });
    subscribe(middleware::MsgType::UI_Purify_Req,
        [this](const middleware::Message& msg) { on_purify_request(msg); });

    env_thread_ = std::thread(&MCUService::env_notify_thread_fn, this);
    return true;
}

bool MCUService::do_stop() {
    running_.store(false);
    purify_running_.store(false);

    if (env_thread_.joinable()) {
        env_thread_.join();
    }
    if (purify_thread_.joinable()) {
        purify_thread_.join();
    }
    return true;
}

/* ---------- 消息分发 ---------- */

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

/* ---------- 请求处理 ---------- */

void MCUService::on_pm25_request(const middleware::Message& msg) {
    middleware::Pm25ResultPayload p{};
    p.is_high = 0;        // 模拟：PM2.5 正常
    p.pm25_cabin = 12.5f;

    middleware::Message rsp(middleware::MsgType::APP_PM25_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if (!publish_async(rsp)) {
#if SERVICES_DEBUG
        printf("[MCUService] PM2.5 rsp publish failed\n");
#endif
    }

#if SERVICES_DEBUG
    printf("[MCUService] PM2.5 check -> is_high=%u val=%.1f\n",
           p.is_high, p.pm25_cabin);
#endif
}

void MCUService::on_close_door_request(const middleware::Message& msg) {
    middleware::DoorResultPayload p{};
    p.ok = 1;  // 模拟：门已关好

    middleware::Message rsp(middleware::MsgType::APP_CLOSE_DOOR_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if (!publish_async(rsp)) {
#if SERVICES_DEBUG
        printf("[MCUService] PM2.5 rsp publish failed\n");
#endif
    }

#if SERVICES_DEBUG
    printf("[MCUService] close door check -> ok=%u\n", p.ok);
#endif
}

void MCUService::on_open_door_request(const middleware::Message& msg) {
    middleware::DoorResultPayload p{};
    p.ok = 1;  // 模拟：开门成功

    middleware::Message rsp(middleware::MsgType::APP_OpenDoor_Rsp,
        static_cast<middleware::ModuleId>(module_id()), msg.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    if (!publish_async(rsp)) {
#if SERVICES_DEBUG
        printf("[MCUService] PM2.5 rsp publish failed\n");
#endif
    }

#if SERVICES_DEBUG
    printf("[MCUService] open door -> ok=%u\n", p.ok);
#endif
}

void MCUService::on_purify_request(const middleware::Message& msg) {
    if (purify_running_.exchange(true)) {
        return;
    }

    if (purify_thread_.joinable()) {
        purify_thread_.join();
    }
    purify_thread_ = std::thread(&MCUService::purify_thread_fn, this, msg.sender_id);

#if SERVICES_DEBUG
    printf("[MCUService] purify started\n");
#endif
}

/* ---------- 后台线程 ---------- */

void MCUService::env_notify_thread_fn() {
    float temp      = 24.0f;
    float pm_indoor = 35.0f;
    float pm_cabin  = 8.0f;
    float oxygen    = 21.0f;
    int   tick      = 0;

    while (running_.load()) {
        ++tick;

        // 微小随机波动
        temp += (static_cast<float>(std::rand() % 100) - 50.0f) * 0.002f;
        temp = std::fmax(20.0f, std::fmin(30.0f, temp));

        pm_indoor += (static_cast<float>(std::rand() % 100) - 48.0f) * 0.05f;
        pm_indoor = std::fmax(5.0f, std::fmin(80.0f, pm_indoor));

        pm_cabin += (static_cast<float>(std::rand() % 100) - 50.0f) * 0.02f;
        pm_cabin = std::fmax(2.0f, std::fmin(25.0f, pm_cabin));

        oxygen += (static_cast<float>(std::rand() % 100) - 50.0f) * 0.001f;
        oxygen = std::fmax(19.0f, std::fmin(22.0f, oxygen));

        // WiFi 信号偶尔变化
        int signal = 3;
        if ((tick % 30) == 0) {
            signal = 1 + (std::rand() % 4);
        }

        middleware::EnvSnapshotPayload p{};
        std::snprintf(p.device_name, sizeof(p.device_name), "%s", "Rongsu-3D");
        p.status         = static_cast<uint32_t>(middleware::UserHomeStatus::Idle);
        p.wifi_connected = 1;
        p.signal_bars    = signal;
        p.pm25_indoor    = pm_indoor;
        p.pm25_cabin     = pm_cabin;
        p.temperature_c  = temp;
        p.oxygen_percent = oxygen;

        middleware::Message ntf(middleware::MsgType::APP_Update_Ntf,
            static_cast<middleware::ModuleId>(module_id()),
            middleware::ModuleId::UI);
        ntf.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&p), sizeof(p));
        if (!publish_async(ntf)) {
#if SERVICES_DEBUG
            printf("[MCUService] env notify publish failed\n");
#endif
        }

#if SERVICES_DEBUG
        printf("[MCUService] env notify: T=%.1f PMi=%.1f PMc=%.1f O2=%.1f sig=%d\n",
               temp, pm_indoor, pm_cabin, oxygen, signal);
#endif

        // 每 2 秒推送一次环境快照
        for (int i = 0; i < 20 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void MCUService::purify_thread_fn(middleware::ModuleId sender) {
    for (int progress = 0; progress <= 100; progress += 5) {
        if (!purify_running_.load()) {
            break;
        }

        middleware::Message prog(middleware::MsgType::APP_Purify_Progress,
            static_cast<middleware::ModuleId>(module_id()), sender);
        prog.payload = middleware::MsgPayload::from_value(static_cast<uint32_t>(progress));
        if (!publish_async(prog)) {
#if SERVICES_DEBUG
            printf("[MCUService] purify progress publish failed at %d\n", progress);
#endif
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    middleware::Message done(middleware::MsgType::APP_Purify_Rsp,
        static_cast<middleware::ModuleId>(module_id()), sender);
    if (!publish_async(done)) {
#if SERVICES_DEBUG
        printf("[MCUService] purify done publish failed\n");
#endif
    }

    purify_running_.store(false);

#if SERVICES_DEBUG
    printf("[MCUService] purify done\n");
#endif
}

} // namespace services
