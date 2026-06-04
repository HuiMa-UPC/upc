#include "platform.hpp"

#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"

#include "glavo_service.hpp"
#include "tcp_sender.hpp"
#include "device_config.hpp"

#include "ui_app.hpp"

#include <chrono>
#include <thread>
#include <cstdio>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    middleware::MemPool::instance().init();
    middleware::BufferPool::instance().init();
    middleware::MsgBus::instance().init();

    services::GlavoService glavo_service;
    glavo_service.init();

    auto& tcp = services::TcpSender::instance();

    while(tcp.connect_tcp(GLAVO_TCP_DEFAULT_IP, GLAVO_TCP_DEFAULT_PORT) != 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
    }

    while(tcp.open_serial(GLAVO_SERIAL_DEFAULT_DEVICE, GLAVO_SERIAL_DEFAULT_BAUD) != 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
    }

    tcp.start_link_thread();

    if (platform_init() != 0) {
        return 1;
    }

    ui::app_init(lv_scr_act());

    glavo_service.start();

    while (1) {
        const uint32_t delay = lv_timer_handler();
        middleware::MsgBus::instance().process();
        middleware::MsgBus::instance().process();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(delay > 0 ? delay : 1));
    }

    return 0;
}
