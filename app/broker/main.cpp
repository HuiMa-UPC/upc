#include "process_ctl.hpp"
#include "upc_contract.hpp"
#include "ipc_msg_bus.hpp"
#include "rs_log.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

const ProcessConfig kProc = process_config_broker();

const char* broker_socket_path()
{
    const char* env = std::getenv("UPC_BROKER_SOCKET");
    return (env && env[0]) ? env : UPC_BROKER_SOCKET_DEFAULT;
}

int start_broker(char daemon)
{
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

    auto& bus = upc_ipc::IpcMsgBus::instance();
    if (!bus.init_broker(broker_socket_path())) {
        std::fprintf(stderr, "%s listen failed on %s\n",
                     kProc.name, broker_socket_path());
        if (daemon) {
            process_remove_pid(kProc.pid_file);
        }
        return 4;
    }

    std::printf("%s ready (IPC %s)\n", kProc.name, broker_socket_path());

    while (!g_shutdown_requested) {
        bus.process(50);
    }

    bus.shutdown();
    if (daemon) {
        process_remove_pid(kProc.pid_file);
    }
    return 0;
}

int stop_broker()
{
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

int status_broker()
{
    const int pid = process_check_running(kProc.pid_file);
    if (pid > 0) {
        std::printf("%s running (PID: %d)\n", kProc.name, pid);
        return 0;
    }
    std::printf("%s not running\n", kProc.name);
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("Usage: %s {start|stop|status|run}\n", argv[0]);
        return 1;
    }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "start") == 0) {
        return start_broker(1);
    }
    if (std::strcmp(cmd, "stop") == 0) {
        return stop_broker();
    }
    if (std::strcmp(cmd, "status") == 0) {
        return status_broker();
    }
    if (std::strcmp(cmd, "run") == 0) {
        return start_broker(0);
    }
    std::fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
