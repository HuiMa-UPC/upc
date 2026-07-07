# common/ — 平台与契约

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)

跨 `app/` 各进程共用；契约变更需相关组知晓。

## 子目录

| 子目录 | 文档 | 职责 |
|--------|------|------|
| **契约文档** | [README.md](README.md) | 消息命名与总表（改 MsgType 从这里走） |
| **`middleware/`** | [middleware/MIDDLEWARE.md](middleware/MIDDLEWARE.md) | MsgBus、内存池 |
| **`ipc/`** | [ipc/IPC.md](ipc/IPC.md) | Unix socket、`MsgPort`、SHM 帧环 |
| **`debug/`** | [debug/DEBUG.md](debug/DEBUG.md) | `UPC_LOG_*`、`upc_sys_monitor` |
| **`process/`** | [process/PROCESS.md](process/PROCESS.md) | PID、daemon、`process_config` |

## 进程与日志

| 进程 | 日志 | PID |
|------|------|-----|
| UI | `/var/log/rongsu_ui{1,2,3}.log` | `/var/run/rongsu_ui.pid` |
| Glavo | `/var/log/rongsu_glavo{1,2,3}.log` | `/var/run/rongsu_glavo.pid` |
| Camera | `/var/log/rongsu_camera{1,2,3}.log` | `/var/run/rongsu_camera.pid` |

## 构建与启动

```bash
./build.sh                # 全量编译，产物 output/
scripts/rongsu.sh start   # glavo + rongsu_camera → ui
```

UI 为 IPC **Hub**；后端各 **Server**。详见 [ipc/IPC.md](ipc/IPC.md)。

## 链接边界

- **UI**：`upc::ipc` + `ui::port`，**禁止** `app/glavo/lib`
- **Glavo**：`upc_glavo` + `upc::ipc` + `parser`，**禁止** lvgl
- **Camera**：`middleware` + `upc::ipc`，**禁止** lvgl / `app/glavo` / `app/ui`
