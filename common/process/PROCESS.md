# common/process — 进程守护与配置

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md) · CMake：`upc::process`

## 职责

| 文件 | 职责 |
|------|------|
| `process_ctl.cpp` | daemonize、PID 文件、信号处理 |
| `log_rotating.cpp` | 三文件 5MB 循环日志（1→2→3→1，断电 append 恢复） |
| `process_config.hpp` | 各进程 `ProcessConfig`（pid/log_base/name） |

## 进程配置速查

| 函数 | 可执行文件 | PID 文件 | 日志（前缀，实际为 `N.log`，N=1..3） |
|------|------------|----------|------|
| `process_config_ui()` | `rongsu_ui` | `/var/run/rongsu_ui.pid` | `/var/log/rongsu_ui` |
| `process_config_glavo()` | `rongsu_glavo` | `/var/run/rongsu_glavo.pid` | `/var/log/rongsu_glavo` |
| `process_config_camera()` | `rongsu_camera` | `/var/run/rongsu_camera.pid` | `/var/log/rongsu_camera` |

各进程 `main.cpp` 支持 `{start|stop|status|run}`；`start` 时 stderr/stdout 重定向到 `{log_base}N.log`（单文件上限 5MB，三文件循环；重启不清空已有文件）。

## 相关文档

- [common/COMMON.md](../COMMON.md) · [scripts/rongsu.sh](../../scripts/rongsu.sh)
