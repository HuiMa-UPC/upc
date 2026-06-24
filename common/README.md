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
| **传输** | UI **Hub** 多连接 + 后端 **Server**（见 [ipc/IPC.md](ipc/IPC.md)） |

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
| `UI` | 界面 / IPC Hub | `rongsu_ui` |
| `APP_GLAVO` | 振镜/串口后端 | `rongsu_glavo` |
| `APP_CAMERA` | 相机后端 | `rongsu_camera` / `test_camera` |
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
| `APP_Update_Ntf` | `GLAVO_Env_Update_Ntf` | Glavo→UI | `EnvSnapshotPayload` | `register_env_update` | `McuLink` 推送 |
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
| `UI_Camera_Stream_Start_Req` | — | UI→Camera | `CameraStreamStartReqPayload` | `CameraMsgProxy::send_stream_start` | 开预览 |
| `APP_Camera_Stream_Start_Rsp` | — | Camera→UI | `CameraStreamInfoPayload` | `register_stream_start_rsp` | SHM 参数 / dma fd 旁路信息 |
| `UI_Camera_Stream_Stop_Req` | — | UI→Camera | Empty | `send_stream_stop` | 关预览 |
| `APP_Camera_Frame_Ntf` | — | Camera→UI | `CameraFrameNtfPayload` | （可选） | 帧序号；默认关 |
| `APP_Print_Video_Session_Start_Ntf` | — | Glavo→Camera（Hub FWD） | `PrintVideoSessionPayload` | — | `PrintVideoNotify` / `CameraService` |
| `APP_Print_Layer_Snap_Ntf` | — | Glavo→Camera（Hub FWD） | `PrintVideoLayerSnapPayload` | — | 换层 Snap |
| `APP_Print_Video_Session_Finish_Ntf` | — | Glavo→Camera（Hub FWD） | `PrintVideoSessionPayload` | — | ffmpeg 合成 MP4 |
| `APP_Print_Video_Session_Abort_Ntf` | — | Glavo→Camera（Hub FWD） | `PrintVideoSessionPayload` | — | 删临时目录 |

`CameraStreamInfoPayload.dma_mode` 当前固定为 `1`：
- 预览统一走 **dma fd 旁路**：后端经主 IPC（`/tmp/upc_camera.sock`）`SCM_RIGHTS` 附带 dma_buf fd；UI 侧 `MsgPort::take_peer_dma_fds(APP_CAMERA)` 接收。SHM `/upc_camera_preview` 仅保留 slot 状态元数据，像素不再走 SHM payload。
- `dma_socket_path` 字段为 ABI 保留，当前未使用独立 socket。
- `test_camera` 与 `rongsu_camera` 使用同一套契约。

---

## 4. 新增消息（固定四步）

1. **`msg_types.hpp`** — 增加 `MsgType` + POD + 注释（本表同步一行）
2. **发送侧进程** — `subscribe` + `handle_*` 或 `publish_*`（Glavo / Camera / UI 各自实现）
3. **UI 侧** — 若需界面交互：`ui_msg_proxy.*` 或 `camera_msg_proxy.*` 的 `send_*` + `register_*`（纯后端互访可跳过）
4. **Hub 转发** — `target_id` 指向其它后端时，UI `IpcMsgBus::deliver_frames` 自动 `IPC_FWD`；一般无需改 `ipc_wire`

Wire 协议版本：`WIRE_VERSION`（[`ipc_wire.hpp`](../ipc/ipc_wire.hpp)）。仅当帧格式变更时递增。

---

## 5. 多后端拓扑

### 5.1 现状（UI Hub，方案 2 已落地）

```
rongsu_glavo ──listen──► /tmp/upc_glavo.sock ◄──┐
rongsu_camera ──listen──► /tmp/upc_camera.sock ◄──┼── rongsu_ui (Hub)
test_camera  ──listen──► /tmp/upc_camera.sock ◄──┘
```

- UI：`set_mode_ipc_hub` + `add_ipc_peer(APP_GLAVO, …)` + `add_ipc_peer(APP_CAMERA, …, required=false)`
- 各后端：`set_mode_ipc_server`，单连接 UI
- **后端互访**：Glavo `publish_async` 且 `target_id = APP_CAMERA` → UI Hub **`IPC_FWD`** → Camera（UI 不跑业务 handler）
- **大流量**：预览 NV12 走 dma fd 旁路 + `/upc_camera_preview` 元数据环；控制仍走 MsgPort

详见 [ipc/IPC.md](ipc/IPC.md)。

### 5.2 后续演进（可选）

| 需求 | 现状 | 可选方案 |
|------|------|----------|
| UI ↔ 多后端 | ✅ Hub 多 peer | — |
| Glavo ↔ Camera | ✅ Hub 转发 | 独立 Broker 减轻 UI 负担 |
| IPC 与 LVGL 同线程 | 主循环 `process(0)` 前置 + poll 上限 8ms | 独立 I/O 线程（仅 poll + FWD） |

**中心 Broker**（进程继续增多、或 UI 不想兼 Hub）：各进程 client 连 broker，按 `target_id` 转发，帧格式不变（未实施）。

**大流量旁路**：控制走 socket；像素走 dma fd + `ipc_dma_frame_ring` 元数据环（camera 预览已用）。

新增进程时在 `ModuleId` 追加（如 `APP_LOG`），`MsgType` 用域前缀（`LOG_` 等），遵循 §2 命名规范。

---

## 6. 文件职责（方案 A 目录）

| 路径 | 职责 | 维护方 |
|------|------|--------|
| `common/README.md` | 本文档、消息总表 | 架构 / 双组评审 |
| `common/middleware/message_bus/msg_types.hpp` | **唯一源码契约** | 双组评审 |
| `common/ipc/msg_port.hpp` | 业务唯一 IPC 入口 | 平台组 |
| `common/ipc/ipc_wire.*` | 线协议 | 平台组 |
| `common/ipc/ipc_endpoint.*` | Socket 传输 | 平台组 |
| `common/ipc/ipc_msg_bus.*` | Hub 多 peer 路由 + FWD | 平台组 |
| `app/ui/lib/core/ui_msg_proxy.*` | UI ↔ Glavo API | UI 组 |
| `app/ui/lib/core/camera_msg_proxy.*` | UI ↔ Camera 预览 IPC | UI 组 |
| `app/ui/lib/core/camera_preview.*` | dma fd 预览解码 + LVGL 显示 | UI 组 |
| `app/ui/lib/core/rga_dma_buf.*` | RGA dma32 heap 缓冲（封装 `common/ipc/dma_heap_buffer`） | UI 组 |
| `app/glavo/lib/glavo_service.*` | Glavo 侧 API | Glavo 组 |
| `app/glavo/lib/print_video_notify.*` | Glavo → Camera 录像 IPC | Glavo 组 |
| `app/camera/lib/camera_service.*` | Camera IPC + 录像 | Camera 组 |
