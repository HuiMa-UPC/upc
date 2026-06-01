#include "platform.hpp"

#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"

#include "glavo_service.hpp"
#include "mcu_service.hpp"

#include "ui_app.hpp"

#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    middleware::MemPool::instance().init();
    middleware::BufferPool::instance().init();
    middleware::MsgBus::instance().init();

    services::GlavoService glavo_service;
    services::MCUService mcu_service;
    glavo_service.init();
    mcu_service.init();

    if (platform_init() != 0) {
        return 1;
    }

    /* UI 订阅就绪后再启动后端，避免 env 推送等消息在订阅前入队丢失 */
    ui::app_init(lv_scr_act());

    glavo_service.start();
    mcu_service.start();

    while (1) {
        const uint32_t delay = lv_timer_handler();
        middleware::MsgBus::instance().process();
        middleware::MsgBus::instance().process();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(delay > 0 ? delay : 1));
    }

    return 0;
}
