#ifndef PROCESS_CONFIG_HPP
#define PROCESS_CONFIG_HPP

#include "process_ctl.hpp"

/** 每进程独立 PID 与日志文件（守护进程 start 时 stderr/stdout 重定向到 log_file） */
inline ProcessConfig process_config_ui() {
    return ProcessConfig{
        "/var/run/rongsu_ui.pid",
        "/var/log/rongsu_ui.log",
        "rongsu_ui",
    };
}

inline ProcessConfig process_config_glavo() {
    return ProcessConfig{
        "/var/run/rongsu_glavo.pid",
        "/var/log/rongsu_glavo.log",
        "rongsu_glavo",
    };
}

#endif
