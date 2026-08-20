#include "process_ctl.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"
#include "upc_sys_monitor.hpp"
#include "rs_log.hpp"
#include "media_service.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

const ProcessConfig kProc = process_config_media();

const char* broker_socket_path()
{
    const char* env = std::getenv("UPC_BROKER_SOCKET");
    return (env && env[0]) ? env : UPC_BROKER_SOCKET_DEFAULT;
}

int start_media(char daemon)
{
    upc::log_set_process_name(kProc.name);
    process_install_signals();

    if(daemon) {
        const int running = process_check_running(kProc.pid_file);
        if(running > 0) {
            std::fprintf(stderr, "%s already running (PID: %d)\n",
                         kProc.name, running);
            return 1;
        }
        if(process_daemonize() < 0) {
            std::fprintf(stderr, "%s daemonize failed\n", kProc.name);
            return 2;
        }
        if(process_write_pid(kProc.pid_file) < 0) {
            std::fprintf(stderr, "%s cannot write PID file\n", kProc.name);
            return 3;
        }
        std::printf("Starting %s (PID: %d)...\n", kProc.name, getpid());
    }

    process_log_boot_banner(kProc.name);

    auto& port = middleware::MsgPort::instance();
    if(!port.set_mode_ipc_client(middleware::ModuleId::APP_MEDIA,
                                 broker_socket_path())) {
        UPC_LOG_MEDIA_SERVICE(Error, "IPC connect failed on %s",
                              broker_socket_path());
        return 4;
    }
    if(!port.init()) {
        UPC_LOG_MEDIA_SERVICE(Error, "MsgPort init failed");
        return 5;
    }

    upc::media::MediaService service;
    if(!service.init()) {
        UPC_LOG_MEDIA_SERVICE(Error, "service init failed");
        return 6;
    }

    UPC_LOG_MEDIA_SERVICE(Info, "ready IPC broker %s", broker_socket_path());
    while(!g_shutdown_requested) {
        port.process(2);
        service.tick();
        upc_sys_monitor_tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    service.shutdown();
    port.shutdown();
    if(daemon) {
        process_remove_pid(kProc.pid_file);
    }
    return 0;
}

int stop_media()
{
    const int pid = process_check_running(kProc.pid_file);
    if(pid == 0) {
        std::printf("%s not running\n", kProc.name);
        return 1;
    }
    std::printf("Stopping %s (PID: %d)...\n", kProc.name, pid);
    if(kill(static_cast<pid_t>(pid), SIGTERM) < 0) {
        perror("kill failed");
        return 2;
    }
    int timeout = 50;
    while(timeout-- > 0) {
        if(kill(static_cast<pid_t>(pid), 0) != 0) {
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

int status_media()
{
    const int pid = process_check_running(kProc.pid_file);
    if(pid > 0) {
        std::printf("%s running (PID: %d)\n", kProc.name, pid);
        return 0;
    }
    std::printf("%s not running\n", kProc.name);
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 2) {
        std::printf("Usage: %s {start|stop|status|run}\n", argv[0]);
        return 1;
    }
    const char* cmd = argv[1];
    if(std::strcmp(cmd, "start") == 0) {
        return start_media(1);
    }
    if(std::strcmp(cmd, "stop") == 0) {
        return stop_media();
    }
    if(std::strcmp(cmd, "status") == 0) {
        return status_media();
    }
    if(std::strcmp(cmd, "run") == 0) {
        return start_media(0);
    }
    std::fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
