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

#endif
