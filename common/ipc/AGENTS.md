# ipc 模块 — 多人协作指南

> 命名空间：`upc_ipc`（传输）+ `middleware::MsgPort`（业务门面）  
> CMake 目标：`upc::ipc`

## 职责边界

| 目录/文件 | 负责人建议 | 职责 |
|-----------|------------|------|
| `ipc_wire.*` | 平台/架构 | **线协议**（`MsgType` + POD 字节），版本号 `WIRE_VERSION` |
| `ipc_endpoint.*` | 平台/架构 | Unix 域 socket listen/connect/帧读写 |
| `ipc_msg_bus.*` | 平台/架构 | Hub 多 peer 路由、订阅表、跨进程收发 |
| `msg_port.*` | 平台/架构 | **唯一业务入口**；UI hub / 后端 server |
| `ipc_config.hpp` | 运维/平台 | 套接字路径、peer 重连/启动等待 |
| `ipc_shm_*` | 平台/架构 | 大流量旁路 SHM 帧环（预留，暂无业务接入） |

**不要改**（除非全员评审）：

- `middleware/message_bus/msg_types.hpp` — 消息契约源码
- 契约文档与总表：`common/contract/README.md`

## 依赖关系

```
ui / glavo  →  msg_port.hpp  →  upc::ipc  →  middleware::port
```

- **UI 组**：只链接 `upc::ipc` + `ui::port`，**禁止**链接 `glavo/lib`
- **Glavo 组**：链接 `glavo/lib` + `upc::ipc`，**禁止**链接 `lvgl`
- **平台组**：维护 `ipc/` 与 `scripts/rongsu.sh`

## 一对多策略（方案 2）

```
rongsu_glavo ──listen──► /tmp/upc_glavo.sock ◄──┐
rongsu_ota   ──listen──► /tmp/upc_ota.sock   ◄──┼── rongsu_ui (IpcMsgBus Hub)
…                                              ◄──┘
```

- 后端之间 **无 socket**；仅 UI 多连接。
- UI：**Hub 模式** — `peers_` 表，每条 `ModuleId` 对应一个 `IpcEndpoint`。
- 后端：**Server 模式** — `listen` + 等待 UI `connect`（单 UI 连接）。

### 收包（UI）

`process()` → `pump_inbound_hub()` 轮询 **所有** peer 的 socket → `wire_decode` → **同一** `deliver_local()` → 各 `subscribe` 回调。分发靠 **`MsgType`**，不靠 socket 编号。

### 发包（UI）

| 方式 | API / 字段 | 行为 |
|------|------------|------|
| 单播 | `Message(..., target_id = APP_XXX)` | `send_to_peer` 选对应 socket |
| 广播 | `target_id = 0` 或 `publish_async_broadcast()` | 发往所有 **已连接** peer |

### 连接与重连

- `peer_connected()`：所有 **required** peer 均连着。
- `peer_connected(ModuleId)`：单个 peer 状态。
- 断线：`try_reconnect_peers()`，间隔 `UPC_IPC_PEER_RECONNECT_INTERVAL_MS`（默认 3000ms）。

### I/O 模型（Linux epoll）

- Hub / Server 的 socket 由 `IpcMsgBus` 注册到 `IpcPoller`（`ipc_poller.cpp`）。
- `MsgPort::process(io_timeout_ms)` → `epoll_wait`：有数据立即收包；无数据阻塞至多 `timeout_ms`。
- 非 Linux 平台 fallback 为轮询 + `sleep`（`pump_inbound_fallback`）。
- UI 主循环：`port.process(lv_delay)` + `port.process(0)`（同帧 Req/Rsp）；Glavo：`port.process(2)`。

## MsgPort API 速查

| API | 角色 | 说明 |
|-----|------|------|
| `set_mode_ipc_hub(local_id)` | UI | 进入 Hub，初始化内存池 |
| `add_ipc_peer(id, path, required)` | UI | 注册并连接一个后端；可多次调用 |
| `set_mode_ipc_server(id, path)` | 后端 | `listen`，不阻塞 accept |
| `subscribe` / `publish_async` / `process` | 共用 | `process(timeout_ms)` 默认 0 |
| `publish_async_broadcast(msg)` | UI | 等价 `target_id=0` |
| `init_client(local, path)` | UI 兼容 | 等价 hub + `add_peer(APP_GLAVO, …, true)` |

## 进程与套接字（当前）

| 进程 | 可执行文件 | IPC 模式 | 默认 socket | PID |
|------|------------|----------|-------------|-----|
| Glavo | `rongsu_glavo` | Server | `/tmp/upc_glavo.sock` | `/var/run/rongsu_glavo.pid` |
| UI | `rongsu_ui` | Hub client | （连上述各后端） | `/var/run/rongsu_ui.pid` |

环境变量：`UPC_IPC_SOCKET` 覆盖 Glavo socket。新后端在 `ipc_config.hpp` 增加 `UPC_*_IPC_SOCKET_DEFAULT`。

启动顺序见 `scripts/rongsu.sh`（后端 `wait_for_socket` 后再启 UI）。

## 新增后端 — 平台层清单

1. `msg_types.hpp` — `ModuleId` + `MsgType` + Payload
2. `ipc_config.hpp` — 默认 socket 路径
3. 新进程 `main` — `set_mode_ipc_server(ModuleId::APP_XXX, path)` + 主循环 `port.process()`
4. UI `main.cpp` — `add_ipc_peer(APP_XXX, path, required)`
5. `rongsu.sh` — 先起新后端并等待 socket
6. UI 业务 — 独立 `XxxMsgProxy`（见 [ui/AGENTS.md §11](../../ui/AGENTS.md)）

大流量数据：socket 只传控制消息；像素等用 `ipc_shm_*`（与 MsgPort 正交）。

## 扩展消息类型

见 [`common/contract/README.md`](../contract/README.md) §4（四步流程 + 命名规范）。

## 相关文档

| 文档 | 内容 |
|------|------|
| [ui/AGENTS.md](../../ui/AGENTS.md) §11 | UI 接入多后端、Proxy 约定 |
| [common/README.md](../README.md) §5 | 架构演进（方案 1 Broker / 方案 2 UI 多连接） |
| [glavo/AGENTS.md](../../glavo/AGENTS.md) | Glavo server 与 Service |
