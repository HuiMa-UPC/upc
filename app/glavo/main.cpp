#include "process_ctl.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"
#include "upc_sys_monitor.hpp"
#include "rs_log.hpp"
#include "glavo_service.hpp"
#include "mcu_link.hpp"
#include "device_config.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

const ProcessConfig kProc = process_config_glavo();

const char* broker_socket_path() {
    const char* env = std::getenv("UPC_BROKER_SOCKET");
    return (env && env[0]) ? env : UPC_BROKER_SOCKET_DEFAULT;
}

int start_glavo(char daemon) {
    upc::log_set_process_name(kProc.name);
    process_install_signals();

    if (daemon) {
        const int running = process_check_running(kProc.pid_file);
        if (running > 0) {
            std::fprintf(stderr, "%s already running (PID: %d)\n",
                kProc.name, running);
            return 1;
        }
        if (process_daemonize(kProc.log_base) < 0) {
            std::fprintf(stderr, "%s daemonize failed\n", kProc.name);
            return 2;
        }
        if (process_write_pid(kProc.pid_file) < 0) {
            std::fprintf(stderr, "%s cannot write PID file\n", kProc.name);
            return 3;
        }
        std::printf("Starting %s (PID: %d)...\n", kProc.name, getpid());
    }

    auto& port = middleware::MsgPort::instance();
    if (!port.set_mode_ipc_client(middleware::ModuleId::APP_GLAVO,
                                  broker_socket_path())) {
        std::fprintf(stderr, "%s IPC connect failed on %s\n",
            kProc.name, broker_socket_path());
        return 4;
    }
    if (!port.init()) {
        return 5;
    }

    upc::glavo::GlavoService glavo_service;
    if (!glavo_service.init()) {
        return 6;
    }

    auto& mcu = upc::glavo::McuLink::instance();
    /* TCP 振镜为可选链路：首次尝试一次即可，连不上由 link 线程后台自动重连，
     * 不阻塞串口/调试链路的启动（否则振镜未接时串口与 UI_Debug_* 都无法工作）。 */
    mcu.connect_tcp();

    while (mcu.open_serial(GLAVO_SERIAL_DEFAULT_DEVICE, GLAVO_SERIAL_DEFAULT_BAUD) != 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
        if (g_shutdown_requested) {
            if (daemon) {
                process_remove_pid(kProc.pid_file);
            }
            return 0;
        }
    }

    mcu.start_link_thread();
    glavo_service.start();

    std::printf("%s ready (IPC broker %s)\n",
        kProc.name, broker_socket_path());

    while (!g_shutdown_requested) {
        port.process(2);
        upc_sys_monitor_tick();
    }

    glavo_service.stop();
    port.shutdown();
    if (daemon) {
        process_remove_pid(kProc.pid_file);
    }
    return 0;
}

int stop_glavo() {
    const int pid = process_check_running(kProc.pid_file);
    if (pid == 0) {
        std::printf("%s not running\n", kProc.name);
        return 1;
    }
    std::printf("Stopping %s (PID: %d)...\n", kProc.name, pid);
    if (kill(static_cast<pid_t>(pid), SIGTERM) < 0) {
        perror("kill failed");
        return 2;
    }
    int timeout = 50;
    while (timeout-- > 0) {
        if (kill(static_cast<pid_t>(pid), 0) != 0) {
            std::printf("%s stopped\n", kProc.name);
            process_remove_pid(kProc.pid_file);
            return 0;
        }
        usleep(100000);
    }
    std::printf("Force killing %s...\n", kProc.name);
    kill(static_cast<pid_t>(pid), SIGKILL);
    process_remove_pid(kProc.pid_file);
    return 0;
}

int status_glavo() {
    const int pid = process_check_running(kProc.pid_file);
    if (pid > 0) {
        std::printf("%s running (PID: %d)\n", kProc.name, pid);
        return 0;
    }
    std::printf("%s not running\n", kProc.name);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Usage: %s {start|stop|status|run}\n", argv[0]);
        return 1;
    }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "start") == 0) {
        return start_glavo(1);
    }
    if (std::strcmp(cmd, "stop") == 0) {
        return stop_glavo();
    }
    if (std::strcmp(cmd, "status") == 0) {
        return status_glavo();
    }
    if (std::strcmp(cmd, "run") == 0) {
        return start_glavo(0);
    }
    std::fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
