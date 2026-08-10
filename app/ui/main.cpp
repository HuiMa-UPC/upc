#include "platform.hpp"
#include "process_ctl.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"
#include "ui_app.hpp"
#include "upc_sys_monitor.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

const ProcessConfig kProc = process_config_ui();

const char* broker_socket_path() {
    const char* env = std::getenv("UPC_BROKER_SOCKET");
    return (env && env[0]) ? env : UPC_BROKER_SOCKET_DEFAULT;
}

bool connect_ipc_with_retry() {
    auto& port = middleware::MsgPort::instance();
    for (int i = 0; i < 100; ++i) {
        port.shutdown();
        if (!port.set_mode_ipc_client(middleware::ModuleId::UI,
                                      broker_socket_path())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!port.init()) {
            continue;
        }
        for (int j = 0; j < 20; ++j) {
            port.process(50);
            if (port.peer_connected()) {
                return true;
            }
        }
        if (port.peer_connected()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_shutdown_requested) {
            return false;
        }
    }
    return false;
}

int start_ui(char daemon) {
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

    process_log_boot_banner(kProc.name);

    if (!connect_ipc_with_retry()) {
        std::fprintf(stderr,
            "%s IPC connect failed (%s) — start rongsu_broker + rongsu_glavo first\n",
            kProc.name, broker_socket_path());
        if (daemon) {
            process_remove_pid(kProc.pid_file);
        }
        return 4;
    }

    if (platform_init() != 0) {
        if (daemon) {
            process_remove_pid(kProc.pid_file);
        }
        return 5;
    }

    ui::app_init(lv_scr_act());

    auto& port = middleware::MsgPort::instance();
    while (!g_shutdown_requested) {
        port.process(8);
        const uint32_t delay = lv_timer_handler();
        (void)delay;
        port.process(0);
        upc_sys_monitor_tick();
    }

    port.shutdown();
    if (daemon) {
        process_remove_pid(kProc.pid_file);
    }
    return 0;
}

int stop_ui() {
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

int status_ui() {
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
        return start_ui(1);
    }
    if (std::strcmp(cmd, "stop") == 0) {
        return stop_ui();
    }
    if (std::strcmp(cmd, "status") == 0) {
        return status_ui();
    }
    if (std::strcmp(cmd, "run") == 0) {
        return start_ui(0);
    }
    std::fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
