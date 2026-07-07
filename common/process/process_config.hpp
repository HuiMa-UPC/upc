#ifndef PROCESS_CONFIG_HPP
#define PROCESS_CONFIG_HPP

#include "process_ctl.hpp"

/** 每进程独立 PID 与三文件循环日志（守护进程 start 时 stderr/stdout 重定向到 log_baseN.log） */
inline ProcessConfig process_config_ui() {
    return ProcessConfig{
        "/var/run/rongsu_ui.pid",
        "/var/log/rongsu_ui",
        "rongsu_ui",
    };
}

inline ProcessConfig process_config_glavo() {
    return ProcessConfig{
        "/var/run/rongsu_glavo.pid",
        "/var/log/rongsu_glavo",
        "rongsu_glavo",
    };
}

inline ProcessConfig process_config_camera() {
    return ProcessConfig{
        "/var/run/rongsu_camera.pid",
        "/var/log/rongsu_camera",
        "rongsu_camera",
    };
}

#endif
