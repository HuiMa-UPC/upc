# common/ipc — 跨进程通信

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md) · CMake：`upc::ipc`  
> 业务入口：**仅** `middleware::MsgPort`（勿直接调 `IpcMsgBus`）

## 文件职责

| 文件 | 职责 |
|------|------|
| `msg_port.*` | 业务门面：InProcess / IPC 模式切换 |
| `ipc_msg_bus.*` | Hub 路由、订阅表、epoll I/O、`publish` 出站/入站 |
| `ipc_wire.*` | 线协议编解码（`WIRE_VERSION`） |
| `ipc_endpoint.*` | Unix stream socket listen/connect/帧收发 |
| `ipc_poller.*` | Linux epoll 封装（非 Linux 走 fallback 轮询） |
| `ipc_config.hpp` | 默认 socket 路径、重连/启动超时 |
| `ipc_dma_frame_ring.*` | 预览 dma fd 元数据环 |
| `dma_heap_buffer.*` | dma-heap 单个缓冲区分配 |

`dma_heap_buffer.*` 同时被 UI `RgaDmaBuf`、camera `CameraDmaFrameSource` 与 `test_camera_dma_source` 复用，避免各自维护 dma-heap 候选列表。

**契约勿随意改**：`msg_types.hpp`、`common/README.md`

## 拓扑（方案 2）

```
rongsu_glavo ──listen──► /tmp/upc_glavo.sock ◄──┐
rongsu_camera ──listen──► /tmp/upc_camera.sock ◄──┼── rongsu_ui (Hub)
test_camera  ──listen──► /tmp/upc_camera.sock ◄──┘
```

- 后端之间**无** socket；UI 多连接。
- 路由靠 **`MsgType` + `target_id`**，不靠 socket fd。

## MsgPort 速查

| API | 用途 |
|-----|------|
| `set_mode_ipc_hub` + `add_ipc_peer` | UI Hub |
| `set_mode_ipc_server` | Glavo / Camera / test |
| `subscribe` / `publish_async` / `process` | 共用 |
| `publish` | 同步投递（InProcess 或 IPC `publish_sync`；Glavo 工作线程慎用） |
| `publish_async_broadcast` | `target_id=0` 广播 |
| `peer_connected(ModuleId)` | 单 peer 连接状态 |

**兼容 API**（仓库内 UI 已用 hub+add_peer，未调用）：`set_mode_ipc_client` ≡ hub + 仅连 Glavo。

### 进程主循环

- **UI**：`process(0)` → `lv_timer_handler()` → `process(min(delay,8))` → `process(0)`
- **Glavo / Camera**：`process(2)`（I/O + 本地队列投递）

## 发布与投递

### 出站（本进程 `publish_async`）

`send_outbound_from_local` 规则：

| 模式 | `target` | 行为 |
|------|----------|------|
| Server | 本进程 | 仅本地队列/同步投递 |
| Server | 其它后端 | **只** `send_to_peer(UI)`，wire 内 `target_id` 不变 |
| Hub | 已注册 peer | `send_to_peer(target)` |
| Hub / Server | `0`（广播） | 所有已连接 peer |

### 入站（`deliver_frames`）

1. `wire_decode` → 可选 sender 校验（见下）
2. `IPC_RX` 日志
3. **Hub 且 `target` 为其它 peer**：**仅** `IPC_FWD`，不 `deliver_local`
4. 否则 `deliver_local` → 匹配 `MsgType` + `module_id==target_id` 的订阅回调
5. 无订阅 → `IPC_DELIVER miss`（仅对应在本地应处理的报文）

### `publish_async` vs `publish_sync`

| | async | sync |
|--|-------|------|
| 本地投递 | `enqueue_local` → 下次 `process()` | 立即 `deliver_local` |
| 发送失败 | 打 `IPC_TX` Warn | 静默返回 false |

跨进程回复 UI：**必须** `publish_async`，目标 `ModuleId::UI`。

## Hub 转发（后端互访）

```
Glavo publish_async(target=APP_CAMERA)
  → wire 发往 UI（sender=GLAVO, target=CAMERA）
  → UI IPC_RX
  → IPC_FWD → Camera
  → Camera IPC_RX + IPC_DELIVER
```

要点：

- 转发在 **`IpcMsgBus::deliver_frames`** 内同步完成，不经过 UiMsgProxy。
- **wire `sender_id`** 保留逻辑源（如 `APP_GLAVO`）；Camera/Glavo Server 的 transport 对端恒为 UI，**不对 Hub 转发报文打 sender mismatch**。
- 预览像素走 **dma fd 旁路 + `ipc_dma_frame_ring`** 元数据环，控制消息走 MsgPort。

## 日志阶段

| 阶段 | 含义 |
|------|------|
| `IPC_TX` | 本进程经 socket 发出 |
| `IPC_RX` | 本进程收到并已解码 |
| `IPC_FWD` | Hub 转发至其它 peer |
| `IPC_ENQUEUE` | 进入本地异步队列 |
| `IPC_DELIVER` | 投递订阅回调（`handlers=N`） |
| `IPC_DELIVER miss` | 本地应处理但无订阅者 |

详见 [doc/LOG_GUIDE.md](../../doc/LOG_GUIDE.md) §4。

## 默认 socket

| 进程 | 路径 |
|------|------|
| Glavo | `/tmp/upc_glavo.sock`（`UPC_IPC_SOCKET` 可覆盖） |
| rongsu_camera / test_camera | `/tmp/upc_camera.sock` |

`ipc_config.hpp`：`UPC_IPC_PEER_RECONNECT_INTERVAL_MS`（Hub 重连）、`UPC_IPC_PEER_STARTUP_WAIT_MS`（`rongsu.sh` 等待 listen）。

## 新增后端（平台层）

1. `msg_types.hpp` — `ModuleId` / `MsgType` / Payload（contract 四步）
2. `ipc_config.hpp` — 默认 socket（若需）
3. 新进程 `main` — `set_mode_ipc_server` + `process` 循环
4. UI `main.cpp` — `add_ipc_peer`
5. `scripts/rongsu.sh` — 启动顺序 + `wait_for_socket`
6. UI 业务 — 独立 `XxxMsgProxy`

大流量：控制走 MsgPort；像素走主 IPC SCM_RIGHTS 传 dma fd + `ipc_dma_frame_ring` 元数据环（camera 预览：NV12，UI 侧 RGA/SW 转 BGRX）。
详见 `app/camera/lib/camera_dma_frame_source.*`、`test/camera/test_camera_dma_source.*` 与 `app/ui/lib/core/camera_preview.*`。

## 相关文档

- [app/ui/UI.md](../../app/ui/UI.md) · [app/glavo/GLAVO.md](../../app/glavo/GLAVO.md) · [app/camera/CAMERA.md](../../app/camera/CAMERA.md) · [test/TEST.md](../../test/TEST.md)
