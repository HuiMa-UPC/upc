#include "process_ctl.hpp"
#include "rs_log.hpp"

#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <cstring>

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

namespace {

constexpr const char* kTopicLogDir = "/var/log/topic";
constexpr const char* kBootLogPath = "/var/log/topic/topic_boot_lifecycle.log";

void ensure_topic_log_dir()
{
    char path[256];
    std::snprintf(path, sizeof(path), "%s", kTopicLogDir);
    const size_t len = std::strlen(path);
    for(size_t i = 1; i < len; ++i) {
        if(path[i] != '/') {
            continue;
        }
        path[i] = '\0';
        (void)mkdir(path, 0755);
        path[i] = '/';
    }
    (void)mkdir(path, 0755);
}

void redirect_stdio_to_boot_log()
{
    ensure_topic_log_dir();
    int fd = open(kBootLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if(fd < 0) {
        fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    if(fd >= 0) {
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
        close(fd);
    }
}

} // namespace

int process_daemonize() {
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

    redirect_stdio_to_boot_log();

    chdir("/");
    umask(0);
    return 0;
}

void process_log_boot_banner(const char* proc_name)
{
    const char* name = (proc_name && proc_name[0]) ? proc_name : "?";
    const int pid = static_cast<int>(getpid());

    upc::log_write(upc::Level::Info, "SYS", "BOOT",
                   "PROCESS_BOOT proc=%s pid=%d", name, pid);
}
