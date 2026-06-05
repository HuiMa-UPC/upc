# UPC 跨进程消息契约（方案 A）

> **唯一源码契约**：[`../middleware/message_bus/msg_types.hpp`](../middleware/message_bus/msg_types.hpp)  
> **业务入口**：[`../ipc/msg_port.hpp`](../ipc/msg_port.hpp)  
> 改动 `msg_types.hpp` 须 **UI 组 + Glavo 组** 评审。

---

## 1. 通信模型

不是 MQTT 式纯 topic，而是 **带 ModuleId 路由的发布/订阅总线**：

| 概念 | 说明 |
|------|------|
| **订阅** | `MsgPort::subscribe(MsgType, ModuleId, handler)` — 按消息类型 + 本进程身份过滤 |
| **发布** | `MsgPort::publish_async(Message)` — `sender_id` / `target_id` 决定本地投递或跨进程发送 |
| **泵** | 各进程主循环调用 `MsgPort::process()` 收包、投递、消费本地队列 |
| **传输** | 当前为 **1 对 1 Unix socket**（Glavo listen，UI connect） |

```
┌─────────────┐     subscribe/register_*      ┌──────────────┐
│  rongsu_ui  │ ◄────────────────────────────►│ UiMsgProxy   │
└──────┬──────┘                               └──────┬───────┘
       │ MsgPort (client)                              │
       │         ═══════ /tmp/upc_glavo.sock ═══════  │
       │ MsgPort (server)                              │
┌──────▼──────┐                               ┌──────▼───────┐
│rongsu_glavo │ ◄────────────────────────────►│ GlavoService │
└─────────────┘     subscribe/handle_*        └──────────────┘
```

**业务代码禁止**直接 include `ipc_msg_bus.hpp` / 操作 socket；只走 `msg_port.hpp` 或各进程 API 封装（`UiMsgProxy`、`GlavoService`）。

---

## 2. 命名规范

### 2.1 `ModuleId`（进程身份）

| 值 | 含义 | 进程 |
|----|------|------|
| `UI` | 界面 | `rongsu_ui` |
| `APP_GLAVO` | 振镜/串口后端 | `rongsu_glavo` |
| `0` | 广播（`target_id` 专用） | — |

新增后端进程时在此枚举追加（见 [§5 多后端拓扑](#5-多后端拓扑)）。

### 2.2 `MsgType`（消息类型）

```
<域>_<动作>_<后缀>
```

| 域前缀 | 含义 | 示例 |
|--------|------|------|
| `UI_` | 由 UI 发起 | `UI_PM25_Req` |
| `GLAVO_` | 由 Glavo 发起（**推荐新消息使用**） | `GLAVO_Env_Update_Ntf` |
| `APP_` | 同 `GLAVO_`（**历史命名，保留兼容**） | `APP_Update_Ntf` |
| `FS_` | 文件系统相关（**推荐**；现 `Fs_` 保留） | `FS_Dir_Open_Req` |

| 后缀 | 含义 |
|------|------|
| `_Req` | 请求，通常有对应 `_Rsp` |
| `_Rsp` | 响应 |
| `_Ntf` | 单向推送，无响应 |
| `_Done` | 流程结束通知 |
| `_Progress` | 进度推送 |

### 2.3 `Payload` 结构体

```
<动作>Payload
<动作>ReqPayload / <动作>RspPayload   // 请求/响应成对时
```

均为 **POD**，可 `memcpy`；跨进程时由 `ipc_wire` 序列化为 Inline 字节（`BufferRef` 发送前展开）。

### 2.4 各进程业务 API 命名

| 侧 | 发请求 | 收响应/推送 |
|----|--------|-------------|
| **UI** (`UiMsgProxy`) | `send_<action>()` | `register_<event>(callback)` |
| **Glavo** (`GlavoService`) | 内部 `publish_*` | `handle_<action>()` / `subscribe` 分派 |

---

## 3. 消息总表

| MsgType（当前枚举） | 规范名（新消息参照） | 方向 | Payload | UI API | Glavo 处理 |
|---------------------|----------------------|------|---------|--------|------------|
| `APP_Update_Ntf` | `GLAVO_Env_Update_Ntf` | Glavo→UI | `EnvSnapshotPayload` | `register_env_update` | `TcpSender` 推送 |
| `UI_PM25_Req` | — | UI→Glavo | Empty | `send_pm25_check` | `handle_pm25_req` |
| `APP_PM25_Rsp` | `GLAVO_PM25_Check_Rsp` | Glavo→UI | `Pm25ResultPayload` | `register_pm25_result` | PM2.5 流程 |
| `UI_CLOSE_DOOR_Req` | — | UI→Glavo | Empty | `send_close_door_check` | 关门检测 |
| `APP_CLOSE_DOOR_Rsp` | `GLAVO_Door_Close_Rsp` | Glavo→UI | `DoorResultPayload` | `register_close_door_result` | — |
| `Fs_DirOpen_Req` | `FS_Dir_Open_Req` | UI→Glavo | Empty（路径默认 `/home/Gcode`） | `send_fs_dir_open` | `handle_dir_open` |
| `Fs_DirOpen_Rsp` | `FS_Dir_Open_Rsp` | Glavo→UI | `FsDirHeader` + `FileEntryPayload[]` | `register_fs_dir` | 目录扫描 |
| `UI_OpenDoor_Req` | — | UI→Glavo | Empty | `send_open_door` | 开门 |
| `APP_OpenDoor_Rsp` | `GLAVO_Door_Open_Rsp` | Glavo→UI | `DoorResultPayload` | `register_open_door_result` | — |
| `UI_Purify_Req` | — | UI→Glavo | Empty | `send_purify` | 净化（待实现） |
| `APP_Purify_Progress` | `GLAVO_Purify_Progress_Ntf` | Glavo→UI | `Value`（百分比） | `register_purify_progress` | — |
| `APP_Purify_Rsp` | `GLAVO_Purify_Done` | Glavo→UI | Empty | `register_purify_done` | — |
| `UI_FileInfo_Req` | — | UI→Glavo | `PrintStartReqPayload` | `send_file_info_req` | `handle_file_info_req` |
| `APP_FileInfo_Rsp` | `GLAVO_FileInfo_Rsp` | Glavo→UI | `FileInfoPayload` | `register_file_info` | — |
| `UI_Print_Start_Req` | — | UI→Glavo | `PrintStartReqPayload` | `send_print_start` | 启动打印 |
| `APP_Print_Progress_Ntf` | `GLAVO_Print_Progress_Ntf` | Glavo→UI | `PrintProgressPayload` | `register_print_progress` | 进度推送 |
| `UI_Print_Terminate_Req` | — | UI→Glavo | Empty | `send_print_terminate` | 终止作业 |
| `APP_Print_Terminate_Done` | `GLAVO_Print_Terminate_Done` | Glavo→UI | Empty | `register_print_terminate_done` | — |

---

## 4. 新增消息（固定四步）

1. **`msg_types.hpp`** — 增加 `MsgType` + POD + 注释（本表同步一行）
2. **`glavo/.../glavo_service.cpp`** — `subscribe` + `handle_*` + 发 `_Rsp`/`_Ntf`
3. **`ui/lib/core/ui_msg_proxy.*`** — `send_*` + `register_*`
4. **无需改** `ipc_wire`（Inline / BufferRef 自动编解码）

Wire 协议版本：`WIRE_VERSION`（[`ipc_wire.hpp`](../ipc/ipc_wire.hpp)）。仅当帧格式变更时递增。

---

## 5. 多后端拓扑

### 5.1 现状（两进程）

- `IpcMsgBus` 只维护 **一个** `peer_id_` 与 **一条** socket
- Glavo：`init_server` → `peer_id = UI`
- UI：`init_client` → `peer_id = APP_GLAVO`
- **后端之间无法直接通信**；所有 UI↔Glavo 消息必经这条链路

### 5.2 多后端时的问题

若增加 `rongsu_camera`、`rongsu_logger` 等进程：

| 需求 | 现状能否满足 |
|------|--------------|
| UI ↔ 多个后端 | ❌ UI 只能连一个 server |
| Glavo ↔ Camera | ❌ 无 socket、无路由 |
| 任意进程广播 | ⚠️ 仅契约支持 `target_id=0`，传输层未做多跳 |

### 5.3 推荐演进路线

#### 方案 1：中心 Broker（**推荐，3 个及以上进程**）

新增 `rongsu_broker`（或 `upc_msgd`）：

```
        ┌──────────┐
        │  Broker  │  维护 ModuleId → connection 路由表
        └──┬───┬───┘
     ┌─────┘   └─────┐
  rongsu_ui    rongsu_glavo    rongsu_camera ...
  (client)      (client)         (client)
```

- 所有进程 **统一作为 client** 连接 broker（Unix socket 或 abstract namespace）
- 帧格式 **不变**（仍用 `ipc_wire`）；broker 只根据 `target_id` 转发，不解析业务
- `MsgPort` 演进：`init_broker_client(local_id)` + 路由表替代固定 `peer_id_`
- 后端互访：Camera `publish_async` 且 `target_id = APP_GLAVO` 即可，与 UI 发消息相同

优点：拓扑清晰、易加进程、易做日志旁路订阅。  
缺点：broker 成为单点（可 supervise 重启、连接断线重连）。

#### 方案 2：UI 多连接（仅「UI 访问多后端」）

- UI 内 `MsgPort` 持有 `map<ModuleId, IpcEndpoint>`
- 每个后端独立 socket（如 `/tmp/upc_glavo.sock`、`/tmp/upc_camera.sock`）
- Glavo 与 Camera **仍不能直连**

适合：后端互不通信，只被 UI 调度。

#### 方案 3：后端 Mesh（不推荐首发）

每对进程一条 socket，连接数 O(n²)，运维与重连复杂，除非强实时点对点且无 broker。

#### 方案 4：大流量旁路（视频等）

控制消息仍走 socket + `MsgPort`；大块数据用 **独立 SHM**（`ipc_shm_*`），元数据用现有 `MsgType` 通知。与 broker 正交。

### 5.4 契约层预留

新增进程时在 `ModuleId` 追加，例如：

```cpp
enum class ModuleId : uint32_t {
    UI         = 0x01,
    APP_GLAVO  = 0x02,
    APP_CAMERA = 0x03,  // 示例：视觉/监控
    APP_LOG    = 0x04,  // 示例：日志汇聚
};
```

`MsgType` 使用新域前缀（`CAMERA_`、`LOG_` 等），遵循 §2 命名规范。

### 5.5 实施顺序建议

| 阶段 | 内容 |
|------|------|
| **现在** | 两进程 + 本文档 + `msg_types.hpp` 分组注释 |
| **+1 后端** | 可先方案 2（UI 双连接），改动面小 |
| **≥3 进程或后端互访** | 上 Broker（方案 1），一次性替换 `IpcMsgBus` 路由 |

---

## 6. 文件职责（方案 A 目录）

| 路径 | 职责 | 维护方 |
|------|------|--------|
| `common/contract/README.md` | 本文档、消息总表 | 架构 / 双组评审 |
| `common/middleware/message_bus/msg_types.hpp` | **唯一源码契约** | 双组评审 |
| `common/ipc/msg_port.hpp` | 业务唯一 IPC 入口 | 平台组 |
| `common/ipc/ipc_wire.*` | 线协议 | 平台组 |
| `common/ipc/ipc_endpoint.*` | Socket 传输 | 平台组 |
| `common/ipc/ipc_msg_bus.*` | 单 peer 路由（待扩展 broker） | 平台组 |
| `ui/lib/core/ui_msg_proxy.*` | UI 侧 API | UI 组 |
| `glavo/lib/glavo/glavo_service.*` | Glavo 侧 API | Glavo 组 |
