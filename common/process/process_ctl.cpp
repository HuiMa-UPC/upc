#include "process_ctl.hpp"
#include "log_rotating.hpp"

#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>

volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int signum) {
    std::fprintf(stderr, "\nReceived signal %d, shutting down...\n", signum);
    g_shutdown_requested = 1;
}

void process_set_beijing_timezone()
{
    /*
     * POSIX TZ：std offset，offset 为「本地时间加多少得到 UTC」。
     * CST-8 ⇒ 本地 = UTC+8（北京时间），不依赖 /usr/share/zoneinfo。
     * 系统时钟仍由 NTP 写 UTC；localtime_r / 日志时间戳走本地时区。
     */
    setenv("TZ", "CST-8", 1);
    tzset();
}

void process_install_signals() {
    process_set_beijing_timezone();
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
}

int process_check_running(const char* pid_file) {
    if (access(pid_file, F_OK) != 0) {
        return 0;
    }

    FILE* fp = std::fopen(pid_file, "r");
    if (!fp) {
        return 0;
    }

    pid_t pid = 0;
    if (std::fscanf(fp, "%d", &pid) != 1) {
        std::fclose(fp);
        return 0;
    }
    std::fclose(fp);

    if (kill(pid, 0) == 0) {
        return static_cast<int>(pid);
    }
    return 0;
}

int process_write_pid(const char* pid_file) {
    FILE* fp = std::fopen(pid_file, "w");
    if (!fp) {
        return -1;
    }
    std::fprintf(fp, "%d\n", static_cast<int>(getpid()));
    std::fclose(fp);
    return 0;
}

void process_remove_pid(const char* pid_file) {
    unlink(pid_file);
}

int process_daemonize(const char* log_base) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    if (setsid() < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    log_rotating_init(log_base);

    chdir("/");
    umask(0);
    return 0;
}
