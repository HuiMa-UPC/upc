#ifndef PROCESS_CTL_HPP
#define PROCESS_CTL_HPP

#include "upc_contract.hpp"

#include <csignal>
#include <cstdio>

extern volatile sig_atomic_t g_shutdown_requested;

void process_install_signals();
int  process_check_running(const char* pid_file);
int  process_write_pid(const char* pid_file);
void process_remove_pid(const char* pid_file);
int  process_daemonize(const char* log_base);

#endif
