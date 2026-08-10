#ifndef PROCESS_CTL_HPP
#define PROCESS_CTL_HPP

#include "upc_contract.hpp"

#include <csignal>
#include <cstdio>

extern volatile sig_atomic_t g_shutdown_requested;

void process_install_signals();
/** 本地墙钟按北京时间（UTC+8）：WiFi/NTP 同步的仍是 UTC，localtime 显示 +8 */
void process_set_beijing_timezone();
int  process_check_running(const char* pid_file);
int  process_write_pid(const char* pid_file);
void process_remove_pid(const char* pid_file);
int  process_daemonize(const char* log_base);
/**
 * 进程启动后调用一次（须在 daemonize 重定向 stderr 之后）：
 * 在日志中打印醒目 PROCESS_BOOT 标记，便于 grep 判断上电/进程拉起。
 */
void process_log_boot_banner(const char* proc_name);

#endif
