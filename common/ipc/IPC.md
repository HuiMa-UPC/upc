# common/ipc — 跨进程通信

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md) · CMake：`upc::ipc`  
> 门面：`middleware::MsgPort`

## 职责

| 组件 | 职责 |
|------|------|
| `ipc_wire.*` | 线协议 `WIRE_VERSION` |
| `ipc_endpoint.*` | Unix socket listen/connect |
| `ipc_msg_bus.*` | Hub 路由、订阅表 |
| `msg_port.*` | **业务唯一入口**（UI hub / 后端 server） |
| `ipc_shm_*` | 大流量 SHM 帧环（camera 预览等） |

**勿随意改**：`msg_types.hpp`、`contract/README.md`

## 拓扑（方案 2）

```
rongsu_glavo ──listen──► /tmp/upc_glavo.sock ◄──┐
rongsu_camera ──listen──► /tmp/upc_camera.sock ◄──┼── rongsu_ui (Hub)
test_camera  ──listen──► /tmp/upc_camera.sock ◄──┼── rongsu_ui (Hub)
                                                 ◄──┘
```

- 后端之间无 socket；UI 多连接。
- 分发靠 **`MsgType`**，不靠 socket 编号。

## MsgPort 速查

| API | 角色 |
|-----|------|
| `set_mode_ipc_hub` + `add_ipc_peer` | UI |
| `set_mode_ipc_server` | 后端 |
| `subscribe` / `publish_async` / `process` | 共用 |
| `peer_connected(ModuleId)` | 单 peer 状态 |

UI 主循环：`process(wait_ms)` + `process(0)`；Glavo：`process(2)`。

## 默认 socket

| 进程 | 路径 |
|------|------|
| Glavo | `/tmp/upc_glavo.sock`（`UPC_IPC_SOCKET` 可覆盖） |
| rongsu_camera | `/tmp/upc_camera.sock` |
| test_camera | `/tmp/upc_camera.sock` |

## 新增后端（平台层）

1. `msg_types.hpp` — `ModuleId` / `MsgType` / Payload（见 contract 四步）
2. `ipc_config.hpp` — 默认 socket
3. 新进程 `main` — server + `process` 循环
4. UI `main.cpp` — `add_ipc_peer`
5. `scripts/rongsu.sh` — 启动顺序 + `wait_for_socket`
6. UI 业务 — 独立 `XxxMsgProxy`（见 [ui/UI.md](../../ui/UI.md)）

大流量：控制走 MsgPort；像素走 `ipc_shm_*`。

## 相关文档

- [ui/UI.md](../../ui/UI.md) · [glavo/GLAVO.md](../../glavo/GLAVO.md) · [camera/CAMERA.md](../../camera/CAMERA.md) · [test/TEST.md](../../test/TEST.md)
