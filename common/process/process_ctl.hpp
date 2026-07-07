#ifndef PROCESS_CTL_HPP
#define PROCESS_CTL_HPP

#include <csignal>
#include <cstdio>

struct ProcessConfig {
    const char* pid_file;
    /** 日志路径前缀，实际文件为 {log_base}1.log ~ {log_base}3.log（5MB 轮转） */
    const char* log_base;
    const char* name;
};

extern volatile sig_atomic_t g_shutdown_requested;

void process_install_signals();
int  process_check_running(const char* pid_file);
int  process_write_pid(const char* pid_file);
void process_remove_pid(const char* pid_file);
int  process_daemonize(const char* log_base);

#endif
