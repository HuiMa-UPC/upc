# common/process — 进程守护与配置

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md) · CMake：`upc::process`

## 职责

| 文件 | 职责 |
|------|------|
| `process_ctl.cpp` | daemonize、PID 文件、信号处理 |
| `process_config.hpp` | 各进程 `ProcessConfig`（pid/log/name） |

## 进程配置速查

| 函数 | 可执行文件 | PID 文件 | 日志 |
|------|------------|----------|------|
| `process_config_ui()` | `rongsu_ui` | `/var/run/rongsu_ui.pid` | `/var/log/rongsu_ui.log` |
| `process_config_glavo()` | `rongsu_glavo` | `/var/run/rongsu_glavo.pid` | `/var/log/rongsu_glavo.log` |
| `process_config_camera()` | `rongsu_camera` | `/var/run/rongsu_camera.pid` | `/var/log/rongsu_camera.log` |

各进程 `main.cpp` 支持 `{start|stop|status|run}`；`start` 时 stderr 重定向到 `log_file`。

## 相关文档

- [common/COMMON.md](../COMMON.md) · [scripts/rongsu.sh](../../scripts/rongsu.sh)
