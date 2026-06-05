#include "process_ctl.hpp"

#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int signum) {
    std::fprintf(stderr, "\nReceived signal %d, shutting down...\n", signum);
    g_shutdown_requested = 1;
}

void process_install_signals() {
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

int process_daemonize(const char* log_file) {
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

    const int fd = open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    chdir("/");
    umask(0);
    return 0;
}
