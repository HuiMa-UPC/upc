# common/debug — 统一日志与系统监控

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md) · CMake：`upc::debug`  
> 详阅：[doc/LOG_GUIDE.md](../../doc/LOG_GUIDE.md)

## 职责

| 文件 | 职责 |
|------|------|
| `rs_log.hpp` / `rs_log.cpp` | `UPC_LOG_*` 宏、`proc=` 进程名 |
| `upc_log_modules.hpp` | 模块子开关 `UPC_MOD_*` |
| `rs_msg_format.cpp` | IPC 消息摘要格式化 |
| `upc_sys_monitor.*` | HostMonitor（CPU/内存/温度） |

## 模块开关（`UPC_DEBUG=ON` 时）

| 宏 | module 标签 | 典型进程 |
|----|-------------|----------|
| `UPC_MOD_GLAVO_SERVICE` | GlavoService | rongsu_glavo |
| `UPC_MOD_MCU_LINK` | McuLink | rongsu_glavo |
| `UPC_MOD_CAMERA_SERVICE` | CameraService | rongsu_camera |
| `UPC_MOD_CAMERA_PREVIEW` | CameraPreview | rongsu_ui |
| `UPC_MOD_MSG_BUS` / `UPC_MOD_IPC` | MSGBUS / IPC | 全部 |

编译降噪示例：

```bash
cmake -B build -DUPC_DEBUG=ON \
  -DCMAKE_CXX_FLAGS="-DUPC_MOD_MCU_LINK=0 -DUPC_MOD_CAMERA_PREVIEW=0"
```

## 相关文档

- [doc/LOG_GUIDE.md](../../doc/LOG_GUIDE.md) · [common/COMMON.md](../COMMON.md)
