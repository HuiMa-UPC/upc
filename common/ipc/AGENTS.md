# ipc 模块 — 多人协作指南

> 命名空间：`upc_ipc`（传输）+ `middleware::MsgPort`（业务门面）  
> CMake 目标：`upc::ipc`

## 职责边界

| 目录/文件 | 负责人建议 | 职责 |
|-----------|------------|------|
| `ipc_wire.*` | 平台/架构 | **线协议**（`MsgType` + POD 字节），版本号 `WIRE_VERSION` |
| `ipc_endpoint.*` | 平台/架构 | Unix socket listen/connect/帧读写 |
| `ipc_msg_bus.*` | 平台/架构 | 进程内订阅表 + 跨进程收发 |
| `msg_port.*` | 平台/架构 | **唯一业务入口**，UI/Glavo 只 include 此文件 |
| `ipc_config.hpp` | 运维/平台 | 套接字路径默认值 |

**不要改**（除非全员评审）：

- `middleware/message_bus/msg_types.hpp` — 消息契约源码
- 契约文档与总表：`common/contract/README.md`

## 依赖关系

```
ui / services  →  msg_port.hpp  →  upc::ipc  →  middleware::port
```

- **UI 组**：只链接 `upc::ipc` + `ui::port`，**禁止**链接 `services`
- **Glavo 组**：链接 `services` + `upc::ipc`，**禁止**链接 `lvgl`
- **平台组**：维护 `ipc/` 与 `scripts/rongsu.sh`

## 进程与套接字

| 进程 | 可执行文件 | IPC 角色 | PID 文件 |
|------|------------|----------|----------|
| Glavo | `rongsu_glavo` | Server `listen` + `accept` | `/var/run/rongsu_glavo.pid` |
| UI | `rongsu_ui` | Client `connect` | `/var/run/rongsu_ui.pid` |

默认套接字：`/tmp/upc_glavo.sock`（`UPC_IPC_SOCKET` 可覆盖）

## 扩展消息类型

见 [`common/contract/README.md`](../contract/README.md) §4（四步流程 + 命名规范）。

## 后续：日志进程 / 共享内存

- 日志旁路：broker 或第三 socket 订阅 `MsgType` 子集（当前未实现）  
- 视频流：新建 `ipc_shm_*`，不改现有 socket 帧格式
