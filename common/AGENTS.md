# common/ — 平台与契约层

跨 `ui/`、`glavo/` 共用；改动需双组评审。

| 子目录 | 职责 |
|--------|------|
| **`contract/`** | **[消息命名规范与总表](contract/README.md)**（方案 A，无独立 .hpp） |
| `middleware/` | `msg_types.hpp` 消息契约源码、MsgBus、内存池 |
| `ipc/` | Unix socket、`MsgPort` 门面 |
| `debug/` | `UPC_LOG_*`、`upc_sys_monitor` |
| `process/` | PID、守护进程、`process_config`（日志路径） |

## 日志（每进程一个文件）

| 进程 | 日志文件 | PID |
|------|----------|-----|
| UI | `/var/log/rongsu_ui.log` | `/var/run/rongsu_ui.pid` |
| Glavo | `/var/log/rongsu_glavo.log` | `/var/run/rongsu_glavo.pid` |

`start` 时 `process_daemonize` 将 stdout/stderr 重定向到对应 `log_file`（含 `UPC_LOG_*`）。

## 启动

```bash
scripts/rongsu.sh start   # 先 glavo 后 ui
```
