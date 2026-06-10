# common/ — 平台与契约层

跨 `ui/`、`glavo/` 共用；改动需双组评审。

| 子目录 | 职责 |
|--------|------|
| **`contract/`** | **[消息命名规范与总表](contract/README.md)**（方案 A，无独立 .hpp） |
| `middleware/` | `msg_types.hpp` 消息契约源码、MsgBus、内存池 |
| `ipc/` | Unix socket、`MsgPort` 门面（UI **Hub 多连接** / 后端 Server） |
| `debug/` | `UPC_LOG_*`、`upc_sys_monitor` |
| `process/` | PID、守护进程、`process_config`（日志路径） |

## 日志（每进程一个文件）

| 进程 | 日志文件 | PID |
|------|----------|-----|
| UI | `/var/log/rongsu_ui.log` | `/var/run/rongsu_ui.pid` |
| Glavo | `/var/log/rongsu_glavo.log` | `/var/run/rongsu_glavo.pid` |
| test_camera | `/var/log/test_camera.log` | `/var/run/test_camera.pid` |

`start` 时 `process_daemonize` 将 stdout/stderr 重定向到对应 `log_file`（含 `UPC_LOG_*`）。

`UPC_LOG_*` 行格式：`时间 level=… proc=rongsu_ui|glavo src=… module=… 消息`（`run` 双进程同终端时靠 `proc=` 区分）。

`HostMonitor`（`upc_sys_monitor_tick`）：UI 与 Glavo 主循环各调用一次，约每 60s 写入各自 `log_file`；`cpu_proc`/`rss` 为本进程，其余为整机指标。

## 启动

```bash
scripts/rongsu.sh start   # 先 glavo（wait socket），后 ui
```

UI 与 Glavo 为**独立进程**；UI 经 `MsgPort` Hub 连接各后端 socket（当前仅 Glavo）。一对多策略见 [`ipc/AGENTS.md`](ipc/AGENTS.md)、[`ui/AGENTS.md`](../ui/AGENTS.md) §11。
