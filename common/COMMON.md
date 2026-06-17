# common/ — 平台与契约

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)

跨 `ui/`、`glavo/`、`camera/`、`test/` 共用；契约变更需相关组知晓。

## 子目录

| 子目录 | 文档 | 职责 |
|--------|------|------|
| **`contract/`** | [contract/README.md](contract/README.md) | 消息命名与总表（改 MsgType 从这里走） |
| **`middleware/`** | [middleware/MIDDLEWARE.md](middleware/MIDDLEWARE.md) | MsgBus、内存池、线程池 |
| **`ipc/`** | [ipc/IPC.md](ipc/IPC.md) | Unix socket、`MsgPort`、SHM 帧环 |
| **`debug/`** | [debug/DEBUG.md](debug/DEBUG.md) | `UPC_LOG_*`、`upc_sys_monitor` |
| **`process/`** | [process/PROCESS.md](process/PROCESS.md) | PID、daemon、`process_config` |

## 进程与日志

| 进程 | 日志 | PID |
|------|------|-----|
| UI | `/var/log/rongsu_ui.log` | `/var/run/rongsu_ui.pid` |
| Glavo | `/var/log/rongsu_glavo.log` | `/var/run/rongsu_glavo.pid` |
| test_camera | `/var/log/test_camera.log` | `/var/run/test_camera.pid` |

## 启动

```bash
scripts/rongsu.sh start   # glavo [+ test_camera] → ui
```

UI 为 IPC **Hub**；后端各 **Server**。详见 [ipc/IPC.md](ipc/IPC.md)。

## 链接边界

- **UI**：`upc::ipc` + `ui::port`，**禁止** `glavo/lib`
- **Glavo**：`upc_glavo` + `upc::ipc` + `parser`，**禁止** lvgl
- **test_camera**：`middleware` + `upc::ipc`，**禁止** lvgl / glavo
