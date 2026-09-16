#include "process_ctl.hpp"
#include "rs_log.hpp"

#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

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

/* 日志根目录取自 rs_log.hpp，boot log 在其下拼出来：两处若各写一份字面量，
 * 改路径时 rs_log 与这里的 stdout/stderr 重定向会指向不同目录。 */
constexpr const char* kTopicLogDir = UPC_TOPIC_LOG_DIR;
constexpr const char* kBootLogPath = UPC_TOPIC_LOG_DIR "/topic_boot_lifecycle.log";
constexpr const char* kBootStateDir = "/home/.upc_boot_state";

char g_boot_proc_name[64] = {};
bool g_boot_atexit_registered = false;

/* mkdir -p：逐级建目录，已存在的 EEXIST 忽略 */
void mkdir_p(const char* dir)
{
    char path[256];
    std::snprintf(path, sizeof(path), "%s", dir);
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

void build_boot_state_path(const char* proc_name, char* out, size_t out_size)
{
    if(!out || out_size == 0) {
        return;
    }
    const char* name = (proc_name && proc_name[0]) ? proc_name : "unknown";
    std::snprintf(out, out_size, "%s/%s.state", kBootStateDir, name);
}

void write_boot_state(const char* proc_name, int clean, int pid, long long ts_unix)
{
    mkdir_p(kBootStateDir);
    char path[320];
    build_boot_state_path(proc_name, path, sizeof(path));

    char tmp_path[352];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path,
                  static_cast<int>(getpid()));

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd < 0) {
        return;
    }
    char line[160];
    const int n = std::snprintf(
        line, sizeof(line), "clean=%d pid=%d ts=%lld\n", clean, pid, ts_unix);
    if(n > 0) {
        (void)write(fd, line, static_cast<size_t>(n));
        (void)fsync(fd);
    }
    close(fd);
    (void)rename(tmp_path, path);
}

bool read_boot_state(const char* proc_name, int* clean, int* pid, long long* ts_unix)
{
    if(clean) {
        *clean = -1;
    }
    if(pid) {
        *pid = -1;
    }
    if(ts_unix) {
        *ts_unix = 0;
    }

    char path[320];
    build_boot_state_path(proc_name, path, sizeof(path));
    FILE* fp = std::fopen(path, "r");
    if(!fp) {
        return false;
    }

    int prev_clean = -1;
    int prev_pid = -1;
    long long prev_ts = 0;
    const int matched = std::fscanf(fp, "clean=%d pid=%d ts=%lld",
                                    &prev_clean, &prev_pid, &prev_ts);
    std::fclose(fp);
    if(matched != 3) {
        return false;
    }
    if(clean) {
        *clean = prev_clean;
    }
    if(pid) {
        *pid = prev_pid;
    }
    if(ts_unix) {
        *ts_unix = prev_ts;
    }
    return true;
}

long long read_uptime_s()
{
    FILE* fp = std::fopen("/proc/uptime", "r");
    if(!fp) {
        return -1;
    }
    double up = 0.0;
    const int ok = std::fscanf(fp, "%lf", &up);
    std::fclose(fp);
    if(ok != 1 || up < 0.0) {
        return -1;
    }
    return static_cast<long long>(up);
}

void read_boot_id(char* out, size_t out_size)
{
    if(!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    FILE* fp = std::fopen("/proc/sys/kernel/random/boot_id", "r");
    if(!fp) {
        return;
    }
    if(!std::fgets(out, static_cast<int>(out_size), fp)) {
        out[0] = '\0';
    }
    std::fclose(fp);
    const size_t n = std::strlen(out);
    if(n > 0 && out[n - 1] == '\n') {
        out[n - 1] = '\0';
    }
}

void process_mark_clean_shutdown()
{
    if(!g_boot_proc_name[0]) {
        return;
    }
    write_boot_state(g_boot_proc_name, 1, static_cast<int>(getpid()),
                     static_cast<long long>(std::time(nullptr)));
}

void redirect_stdio_to_boot_log()
{
    mkdir_p(kTopicLogDir);
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
    const long long now_unix = static_cast<long long>(std::time(nullptr));
    const long long uptime_s = read_uptime_s();
    char boot_id[96];
    read_boot_id(boot_id, sizeof(boot_id));

    int prev_clean = -1;
    int prev_pid = -1;
    long long prev_ts = 0;
    (void)read_boot_state(name, &prev_clean, &prev_pid, &prev_ts);

    upc::log_write(upc::Level::Info, "SYS", "BOOT", upc::LogTopic::Boot,
                   "PROCESS_BOOT proc=%s pid=%d boot_id=%s uptime_s=%lld "
                   "prev_clean=%d prev_pid=%d prev_ts=%lld",
                   name, pid, boot_id[0] ? boot_id : "?",
                   uptime_s, prev_clean, prev_pid, prev_ts);

    std::snprintf(g_boot_proc_name, sizeof(g_boot_proc_name), "%s", name);
    if(!g_boot_atexit_registered) {
        g_boot_atexit_registered = (std::atexit(process_mark_clean_shutdown) == 0);
    }
    // 启动后先标记为“未干净退出”，正常退出时由 atexit 写回 clean=1。
    write_boot_state(name, 0, pid, now_unix);
}
