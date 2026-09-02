# common/ — 平台、契约与模块说明

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)  
> **唯一源码契约**：[`upc_contract.hpp`](upc_contract.hpp) · **业务 IPC 入口**：[`ipc/msg_port.hpp`](ipc/msg_port.hpp)  
> 改动契约须 **UI 组 + Glavo 组** 评审，并同步本文档消息总表。

跨 `app/` 各进程共用；契约变更需相关组知晓。

## 构建与链接

- **CMake 目标**：`upc::common`（统一静态库；兼容别名 `upc::ipc`、`middleware::port`、`upc::debug`、`upc::process`）
- **契约头文件**：[`upc_contract.hpp`](upc_contract.hpp) — 跨进程 ABI 唯一维护入口（§1 IPC / §2 日志 / §3 进程 / §4 Wire / §5 MsgType）
- **运行时消息**：[`middleware/message_bus/msg_runtime.hpp`](middleware/message_bus/msg_runtime.hpp) — `Message` / `MsgPayload`

```bash
./build.sh                # 全量编译，产物 output/
scripts/rongsu.sh start   # glavo + rongsu_camera → ui
```

## 源码目录

| 子目录 | 职责 |
|--------|------|
| `upc_contract.hpp` | 跨进程契约（唯一维护入口） |
| `middleware/` | MsgBus、内存池、`msg_runtime.hpp` |
| `ipc/` | Unix socket、`MsgPort`、SHM/dma 帧环 |
| `debug/` | `UPC_LOG_*`、`upc_sys_monitor` |
| `process/` | PID、daemon、专题日志重定向 |
| `fs_manifest/` | 本地清单 JSON（G-code / 打印历史 `config.json`；根路径见 `media_paths.hpp`） |

## 进程与日志

| 进程 | 日志目录 | PID |
|------|---------|-----|
| 全进程统一 | `/var/log/topic/*.log`（按专题拆分） | 各自 `/var/run/rongsu_*.pid` |

各业务进程为 IPC **Client**，连中央 **Broker**（`rongsu_broker`）。

**上电标记**：各进程在 `daemonize`（或 `run`）后调用 `process_log_boot_banner()`，日志写入 `topic_boot_lifecycle.log`，可用 `grep PROCESS_BOOT /var/log/topic/topic_boot_lifecycle.log` 确认是否重新拉起。

**时区**：`process_install_signals()` 内 `process_set_beijing_timezone()` 设置 `TZ=CST-8`（北京 / UTC+8）。WiFi 后 NTP 仍写 UTC 系统时钟；`localtime_r`、日志时间戳、历史页/录像文件名按北京时间显示。勿把系统时钟硬加 8 小时。

## 链接边界

- **UI**：`upc::common`（或 `upc::ipc`）+ `ui::port`，**禁止** `app/glavo/lib`
- **Glavo**：`upc_glavo` + `upc::common` + `parser`，**禁止** lvgl
- **Camera**：`upc::common`，**禁止** lvgl / `app/glavo` / `app/ui`

---

---

## 消息契约 · 通信模型

不是 MQTT 式纯 topic，而是 **带 ModuleId 路由的发布/订阅总线**：

| 概念 | 说明 |
|------|------|
| **订阅** | `MsgPort::subscribe(MsgType, ModuleId, handler)` — 按消息类型 + 本进程身份过滤 |
| **发布** | `MsgPort::publish_async(Message)` — `sender_id` / `target_id` 决定本地投递或跨进程发送 |
| **泵** | 各进程主循环调用 `MsgPort::process()` 收包、投递、消费本地队列 |
| **传输** | 各进程 **Client** 连 Broker；Broker 按 `target_id` 转发（见 [§ IPC](#ipc跨进程通信)） |

```
┌─────────────┐     subscribe/register_*      ┌──────────────┐
│  rongsu_ui  │ ◄────────────────────────────►│ UiMsgProxy   │
└──────┬──────┘                               └──────┬───────┘
       │ MsgPort (client)                              │
       │         ═══════ /tmp/upc_broker.sock ═══════  │
       │                    ▲                          │
       │         ┌──────────┴──────────┐              │
       │         │   rongsu_broker     │              │
       │         └──────────▲──────────┘              │
       │ MsgPort (client)   │                          │
┌──────▼──────┐      ┌──────▼──────┐           ┌──────▼───────┐
│rongsu_glavo │      │rongsu_camera│           │ GlavoService │
└─────────────┘      └─────────────┘           └──────────────┘
```

**业务代码禁止**直接 include `ipc_msg_bus.hpp` / 操作 socket；只走 `msg_port.hpp` 或各进程 API 封装（`UiMsgProxy`、`GlavoService`）。

---

## 消息契约 · 命名规范

### 2.1 `ModuleId`（进程身份）

| 值 | 含义 | 进程 |
|----|------|------|
| `UI` | 界面 / IPC Hub | `rongsu_ui` |
| `APP_GLAVO` | 振镜/串口后端 | `rongsu_glavo` |
| `APP_CAMERA` | 相机后端 | `rongsu_camera` |
| `APP_XFR` | WiFi 传输后端 | `rongsu_xfr` |
| `APP_VISUAL` | 视觉后端 | `rongsu_visual` |
| `APP_MQTT` | MQTT 网关后端 | `rongsu_mqtt` |
| `APP_MEDIA` | 媒体后端（拍照/录像/延时） | `rongsu_media` |
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

**强约束（必须遵守）**：
- `*_Req`：必须有且仅有对应 `*_Rsp` 作为请求应答；禁止使用 `*_Done` 或 `*_Ntf` 代替请求应答。
- `*_Ntf`：只能用于单向事件通知，不得承载“期待对端执行并回执”的命令语义。
- `*_Ntf`：用于单向通知（包括事件、阶段、进度、状态更新）；不得承载“期待对端执行并回执”的命令语义。

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

## 消息契约 · 消息总表

| MsgType（当前枚举） | 规范名（新消息参照） | 方向 | Payload | UI API | Glavo 处理 |
|---------------------|----------------------|------|---------|--------|------------|
| `APP_Update_Ntf` | `GLAVO_Env_Update_Ntf` | Glavo→UI/MQTT | `EnvSnapshotPayload`（含 `pm1_indoor`/`pm10_indoor`/`humidity_percent`/`pressure_kpa`/`baseplate_temp_c`/`powder_bits`；环境指标无效填 <0→UI 显示 `--`；`powder_bits` 用于缺粉弹窗继续按钮门控） | `register_env_update` | `McuLink` 推送（UI 与 MQTT 双发） |
| `UI_PM25_Req` | — | UI→Glavo | Empty | `send_pm25_check` | `handle_pm25_req` |
| `APP_PM25_Rsp` | `GLAVO_PM25_Check_Rsp` | Glavo→UI | `Pm25ResultPayload` | `register_pm25_result` | PM2.5 流程 |
| `UI_CLOSE_DOOR_Req` | — | UI→Glavo | Empty | `send_close_door_check` | 关门检测 |
| `APP_CLOSE_DOOR_Rsp` | `GLAVO_Door_Close_Rsp` | Glavo→UI | `DoorResultPayload` | `register_close_door_result` | — |
| `Fs_DirOpen_Req` | `FS_Dir_Open_Req` | UI→Glavo | `PrintFileSource`（仅 USB） | `send_fs_dir_open` | Local 不走 IPC，UI 直读 `/home/Gcode/config.json` |
| `Fs_DirOpen_Rsp` | `FS_Dir_Open_Rsp` | Glavo→UI | `FsDirHeader` + `FileEntryPayload[]` | `register_fs_dir` | **仅 USB** 扫盘应答 |
| `Fs_Delete_Ntf` | `FS_Delete_Ntf` | UI→Glavo | `FsDeleteNtfPayload` | `send_fs_delete_ntf` | 删 Local 文件 + 更新 config.json |
| `APP_Fs_Delete_Rsp` | — | Glavo→UI | `FsDeleteRspPayload` | `register_fs_delete_rsp` | Local 删除完成/失败回执；UI 收到 `ok=1` 后刷新本地目录 |
| `UI_OpenDoor_Req` | — | UI→Glavo | Empty | `send_open_door` | 开门 |
| `APP_OpenDoor_Rsp` | `GLAVO_Door_Open_Rsp` | Glavo→UI | `DoorResultPayload` | `register_open_door_result` | — |
| `UI_Purify_Req` | — | UI→Glavo | Empty | `send_purify` | 净化（待实现） |
| `APP_Purify_Ntf` | `GLAVO_Purify_Ntf` | Glavo→UI | `Value`（百分比） | `register_purify_progress` | — |
| `APP_Purify_Rsp` | `GLAVO_Purify_Rsp` | Glavo→UI | Empty | `register_purify_done` | — |
| `UI_Clean_Start_Req` | — | UI→Glavo | Empty | `send_clean_start` | 开门前清粉（PM2.5≥阈值触发：设风机占空比→开风机→达标后关风机） |
| `APP_Clean_Start_Rsp` | `GLAVO_Clean_Start_Rsp` | Glavo→UI | Empty | `register_clean_start_result` | 清粉达标完成→前端自动开门 |
| `UI_Clean_Stop_Req` | — | UI→Glavo | Empty | `send_clean_stop` | 清粉中断（按清粉页 X 触发），请求后端立即关风机 |
| `APP_Clean_Stop_Rsp` | `GLAVO_Clean_Stop_Rsp` | Glavo→UI | Empty | `register_clean_stop_result` | 清粉中断受理完成→前端切回开门流程 |
| `UI_Clean_Test_Start_Req` | — | UI→Glavo | `CleanTestStartReqPayload` | `send_clean_test_start` | 打印结果页 Purify：启动清粉测试并通知 Media 按延时帧率抽帧 |
| `APP_Clean_Test_Start_Rsp` | — | Glavo→UI | `CleanTestStartRspPayload` | `register_clean_test_start_rsp` | 请求受理状态（避免并发启动） |
| `APP_Clean_Test_Result_Ntf` | — | Media→UI（有录像）/ Glavo→UI（无录像） | `CleanTestDonePayload` | `register_clean_test_done` | MCU 清粉完成上报（A2/42 TLV 0x03）后：Media 停止清粉抽帧并回执；`result_code` 00/01/02 均表示结束；最终 MP4 在结果页退出后统一合成 |
| `UI_Clean_Test_Close_Req` | — | UI→Glavo | Empty | `send_clean_test_close` | 返回首页时强制发送 `clean_test_close` 收尾 |
| `UI_Print_Video_Finalize_Req` | — | UI→Media | `PrintVideoFinalizeReqPayload` | `send_print_video_finalize` | 开门取件后触发：promote 目录 + 清 pending；延时成片受 `UPC_TIMELAPSE_PLAYBACK_PNG` 控制（默认跳过 `timelapse.mp4`），打印录像仍可 remux `print_h264.mp4` |
| `APP_Clean_Test_Close_Rsp` | — | Glavo→UI | Empty | `register_clean_test_close_rsp` | close 指令已受理 |
| `UI_Transport_Req` | — | UI→Glavo | Empty | `send_transport_request` | 转运盒 Ready：发串口自动转运请求（A2/0x41 TLV 0x02） |
| `APP_Transport_Result_Ntf` | — | Glavo→UI | `TransportResultPayload` | `register_transport_result` | MCU TLV 0x04：`0`=完成→BoxShutter；`1/2` 失败/中止→停留转圈页（无超时） |
| `UI_Print_Prepare_Req` | — | UI→Glavo | `PrintPrepareReqPayload` | `send_print_prepare` | USB 复制+轻量扫描+config |
| `APP_Print_Prepare_Rsp` | — | Glavo→UI | `PrintPrepareRspPayload` | `register_print_prepare_done` | — |
| `UI_Parser_Exit_Ntf` | — | UI→Glavo | Empty | `send_parser_exit` | 取消解析 |
| `UI_Print_Start_Req` | — | UI→Glavo | `PrintStartReqPayload` | `send_print_start` | 正式解析+下发 |
| `APP_Print_Progress_Ntf` | `GLAVO_Print_Progress_Ntf` | Glavo→UI | `PrintProgressPayload` | `register_print_progress` | 进度推送 |
| `UI_Print_Terminate_Req` | — | UI→Glavo | Empty | `send_print_terminate` | 终止作业 |
| `UI_Print_Pause_Req` | — | UI→Glavo | Empty | `send_print_pause` | 暂停 TCP 运动下发（解析继续） |
| `UI_Print_Resume_Req` | — | UI→Glavo | Empty | `send_print_resume` | 恢复 TCP 运动下发 |
| `UI_Print_Powder_Low_Ignore_Req` | — | UI→Glavo | Empty | `send_print_powder_low_ignore` | 当前打印会话忽略后续缺粉检测：不再自动暂停、不再推缺粉通知 |
| `APP_Print_Pause_Rsp` | `GLAVO_Print_Pause_Rsp` | Glavo→UI | `PrintPauseStatePayload` | `register_print_pause_done` | 暂停/恢复响应（paused=1/0；Pause/Resume Req 共用） |
| `APP_Print_Powder_Low_Ntf` | — | Glavo→UI | Empty | `register_print_powder_low` | 打印中粉量位 bit0/bit1 同时为 0 时仅通知一次；UI 弹补粉弹窗 |
| `APP_Print_Terminate_Rsp` | `GLAVO_Print_Terminate_Rsp` | Glavo→UI | Empty | `register_print_terminate_done` | — |
| `GLAVO_Print_State_Ntf` | — | Glavo→MQTT | `PrintStateNtfPayload`（`mode` 复用 `UserHomeStatus`：`2`=开始打印 / `0`=打印结束；`since_ms`=进入该 mode 的 Unix ms） | — | 打印状态边沿：开始打印/打印结束空闲时推一次，触发 MQTT 上报 `state` 消息 |
| `UI_Supply_Status_Req` | — | UI→Glavo | Empty | `send_supply_status` | stub |
| `APP_Supply_Status_Rsp` | `GLAVO_Supply_Status_Rsp` | Glavo→UI | `SupplyStatusPayload` | `register_supply_status` | — |
| `UI_Camera_Stream_Start_Req` | — | UI→Camera | `CameraStreamStartReqPayload` | `CameraMsgProxy::send_stream_start` | 开预览 |
| `APP_Camera_Stream_Start_Rsp` | — | Camera→UI | `CameraStreamInfoPayload` | `register_stream_start_rsp` | SHM 参数 / dma fd 旁路信息 |
| `UI_Camera_Stream_Stop_Req` | — | UI→Camera | Empty | `send_stream_stop` | 关预览 |
| `APP_Camera_Stream_Stop_Rsp` | — | Camera→UI | Empty | `register_stream_stop_done` | — |
| `APP_Camera_Frame_Ntf` | — | Camera→UI | `CameraFrameNtfPayload` | （可选） | 帧序号；默认关 |
| `UI_Camera_Snapshot_Req` | — | UI→Media | Empty | `send_snapshot_req` | 存当前帧 PNG + 配对 `.rthm` 缩略图到 `/home/media/photo` |
| `APP_Camera_Snapshot_Rsp` | — | Media→UI | `CameraSnapshotRspPayload` | `register_snapshot_rsp` | — |
| `UI_Storage_Usage_Req` | — | UI→Media | Empty | （历史页直接发总线） | 历史页进入时请求一次容量统计（used/total） |
| `APP_Storage_Usage_Rsp` | — | Media→UI | `StorageUsagePayload` | （历史页直接订阅总线） | 返回 `/home/media` 所在分区已用/总容量，刷新右上角 Storage |
| `VISUAL_Camera_Stream_Start_Req` | — | Visual→Camera | `CameraStreamStartReqPayload`/Empty | — | 视觉进程申请并行取流 |
| `APP_Visual_Camera_Stream_Start_Rsp` | — | Camera→Visual | `CameraStreamInfoPayload` | — | 视觉侧 SHM 参数 / dma fd |
| `VISUAL_Camera_Stream_Stop_Req` | — | Visual→Camera | Empty | — | 视觉进程停止取流 |
| `APP_Visual_Camera_Stream_Stop_Rsp` | — | Camera→Visual | Empty | — | 视觉取流关闭确认 |
| `GLAVO_Visual_FirstLayer_Session_Start_Ntf` | `0x1425` | Glavo→Visual | Empty | — | 打印开始且首层视觉开启时，启动 Camera 取流并预热定位缓存 |
| `GLAVO_Visual_FirstLayer_Session_Stop_Ntf` | `0x1426` | Glavo→Visual | Empty | — | 首层通过、打印结束/终止/错误或功能关闭时停止取流，保留 RKNN 上下文 |
| `APP_Visual_FirstLayer_Reference_Ready_Rsp` | `0x1427` | Visual→Glavo | `VisualFirstLayerReferenceReadyPayload` | — | 同会话裸板 B0 采集回执；成功前禁止开始打印运动，失败则保守终止起印 |
| `GLAVO_Visual_FirstLayer_Check_Req` | `0x1423` | Glavo→Visual | `VisualFirstLayerCheckReqPayload` | — | 触发首层视觉检测（检测前允许从 Camera 最新帧做拷贝/处理） |
| `APP_Visual_FirstLayer_Check_Rsp` | `0x1424` | Visual→Glavo | `VisualFirstLayerCheckRspPayload` | — | 首层检测结果：`result=1` 继续打印，`result=0` 继续铺粉 |
| `APP_Print_Video_Session_Start_Ntf` | — | Glavo→Media（Hub FWD） | `PrintVideoSessionPayload`（含 `h264_bitrate`，0=Media 默认中等码率 / cal `PRINT_H264_BITRATE`） | — | `PrintVideoNotify` / `MediaService` |
| `APP_Print_Layer_Snap_Ntf` | — | Glavo→Media（Hub FWD） | `PrintVideoLayerSnapPayload` | — | 换层 Snap |
| `APP_Print_Video_Session_Finish_Ntf` | — | Glavo→Media（Hub FWD） | `PrintVideoSessionPayload` | — | 保存 `/home/media/video/{job}/1.png...N.png` + `thumb.rthm` + config 快路径（`video_pending`） |
| `APP_Print_Video_Session_Abort_Ntf` | — | Glavo→Media（Hub FWD） | `PrintVideoSessionPayload` | — | 有帧同样落盘目录 + manifest 快路径（`terminated` + `video_pending`）；0 帧不清整目录 |
| `APP_Print_H264_Record_Pause_Ntf` | — | Glavo→Media（Hub FWD） | `PrintPauseStatePayload` | — | 打印暂停/恢复时同步连续 H264 录制状态（paused=1 暂停采帧，0 恢复） |
| `APP_Clean_Video_Session_Start_Ntf` | — | Glavo→Media（Hub FWD） | `CleanVideoSessionStartPayload` | — | 清粉延时抽帧开始：接续打印 `N.png`，按 `fps`/`TIMELAPSE_VIDEO_FPS` 定时写 `(N+1).png…` |
| `APP_Clean_Video_Session_Stop_Ntf` | — | Glavo→Media（Hub FWD） | `CleanVideoSessionStopPayload` | — | 清粉抽帧收尾：成功时仅更新末帧 `thumb.rthm` 与清粉帧范围；最终 MP4 在 `UI_Print_Video_Finalize_Req` 时统一由全部 PNG 合成 |
| `APP_Print_Video_Manifest_Ntf` | — | Media→UI | `PrintVideoManifestPayload` | `CameraMsgProxy::register_print_video_manifest` | manifest `rename()` 成功后发；**0 帧也必发**（`has_entry=0`），否则 UI 停在打印中界面下不来 |
| `MQTT_Media_Rtsp_Start_Req` | — | MQTT→Media | `RtspPushStartReqPayload` | — | 云端 `start_rtsp_push`：`push_url` 原样推 RTSP（已含 token）；一期仅 `live` |
| `APP_Mqtt_Rtsp_Start_Rsp` | — | Media→MQTT | `RtspPushRspPayload` | — | 映射 `cmd/ack`（accepted/rejected/error + reason/detail） |
| `MQTT_Media_Rtsp_Stop_Req` | — | MQTT→Media | `RtspPushStopReqPayload` | — | 云端 `stop_rtsp_push`；不摘 Camera peer |
| `APP_Mqtt_Rtsp_Stop_Rsp` | — | Media→MQTT | `RtspPushRspPayload` | — | 停流结果 → `cmd/ack` |
| `UI_Debug_Local_Rtsp_Set_Ntf` | — | UI→Media | `DebugOnOffPayload` | `DebugMsgProxy::send_local_rtsp_set` | 局域网 live555 RTSP Server 开关（调试页 Other，仅本次运行，默认关）；**target 是 `APP_MEDIA` 不是 `APP_GLAVO`** |
| `UI_Debug_Local_Rtsp_Get_Ntf` | — | UI→Media | Empty | `DebugMsgProxy::request_local_rtsp_state` | 进页查询当前开关状态 |
| `APP_Media_Local_Rtsp_State_Ntf` | — | Media→UI | `DebugOnOffPayload` | `DebugMsgProxy::register_local_rtsp` | 回报**实际**状态：相机流未就绪/编码器失败时回 0，UI 据此把开关弹回 |
| `MQTT_Ota_Notify_Ntf` | — | MQTT→UI | `OtaNotifyPayload` | `register_ota_notify` | 收到云端 OTA 通知后弹确认框（用户确认后再请求 manifest） |
| `UI_Ota_User_Decision_Req` | — | UI→MQTT | `OtaDecisionReqPayload` | `send_ota_user_decision` | 用户确认/取消 OTA 通知（取消仅关弹窗，不上报云端） |
| `APP_Mqtt_Ota_Progress_Ntf` | — | MQTT→UI | `OtaProgressPayload` | `register_ota_progress` | 下载与校验进度；OTA 应用阶段可直接 100%（本期跳过真实刷写） |
| `XFR_Transfer_Progress_Ntf` | — | XFR→UI | `XfrTransferProgressPayload` | `register_xfr_transfer_progress` | WiFi 传输阶段进度（Uploading/Slicing/Finished/Error） |
| `UI_Mcu_Ota_Start_Req` / `UI_Mcu_Ota_Cancel_Ntf` / `APP_Mcu_Ota_Status_Ntf` | — | UI↔Glavo | `McuOtaStatusPayload` | `send_mcu_ota_start` / `send_mcu_ota_cancel` / `register_mcu_ota_status` | Galvo 以太网 OTA（`/home/image/galvo`） |
| `UI_Serial_Mcu_Ota_Start_Req` / `UI_Serial_Mcu_Ota_Cancel_Ntf` / `APP_Serial_Mcu_Ota_Status_Ntf` | — | UI↔Glavo | `McuOtaStatusPayload` | `send_serial_mcu_ota_start` / `send_serial_mcu_ota_cancel` / `register_serial_mcu_ota_status` | MCU 串口 OTA（`/home/image/mcu`） |
| `UI_Debug_Axis_Jog_Ntf` | — | UI→Glavo | `DebugAxisJogPayload` | `DebugMsgProxy::send_axis_jog` | **后端待实现** |
| `UI_Debug_Axis_Hold_Stop_Ntf` | — | UI→Glavo | `DebugAxisPayload` | `send_axis_hold_stop` | 长按松手停 |
| `UI_Debug_Axis_SetZero_Ntf` | — | UI→Glavo | `DebugAxisPayload` | `send_axis_set_zero` | Z 工作零点 |
| `UI_Debug_Axis_Home_Ntf` | — | UI→Glavo | `DebugAxisPayload` | `send_axis_home` | 轴回 Home（Z 0x11 / S 0x10） |
| `UI_Debug_Galvo_Move_Ntf` | — | UI→Glavo | `DebugGalvoMovePayload` | `send_galvo_move` | 振镜步进（预留 TCP） |
| `UI_Debug_Galvo_Home_Ntf` | — | UI→Glavo | Empty | `send_galvo_home` | 振镜回中 |
| `UI_Debug_Galvo_RedLight_Ntf` | — | UI→Glavo | `DebugOnOffPayload` | `send_galvo_red_light` | 红光/激光模式切换（`on=1` 红光，`on=0` 激光）。Glavo 侧每次都下发（TCP 0x0A + 串口激光使能），不去重、不回读状态 |
| `UI_Debug_Galvo_Param_Save_Ntf` | — | UI→Glavo | `DebugGalvoParamPayload` | `send_galvo_param_save` | Scale/步长 |
| `UI_Debug_Galvo_Calib_Ntf` | — | UI→Glavo | Empty | `send_galvo_calib` | 开始标定 |
| `UI_Debug_Device_Ntf` | — | UI→Glavo | `DebugDevicePayload` | `send_device` | 设备/门/电机/照明 |
| `UI_Debug_Powder_Bucket_Ntf` | — | UI→Glavo | `DebugBucketPayload` | `send_powder_bucket` | 粉桶 |
| `UI_Debug_Pusher_Ntf` | — | UI→Glavo | `DebugMotorPayload` | `send_sieve` | 筛粉电机（0=停止/1=筛粉运行/2=反转倒渣） |
| `UI_Debug_Recycle_Motor_Ntf` | — | UI→Glavo | `DebugMotorPayload` | `send_inner_door` | 内门（0=停止/1=开/2=关；原回粉电机块改为内门） |
| `UI_Debug_Fan_Ntf` | — | UI→Glavo | `DebugFanPayload` | `send_fan` | 风扇开/关（占空比走 Fan_Duty_Ntf） |
| `UI_Debug_Switch_Ntf` | — | UI→Glavo | `DebugSwitchPayload` | `send_switch` | 激光/洗气/底板加热 |
| `UI_Debug_CleanTest_Start_Ntf` | — | UI→Glavo | Empty | `send_clean_test_start` | 清粉测试开启（关闭暂不使用） |
| `UI_Debug_CleanTest_Close_Ntf` | — | UI→Glavo | Empty | `send_clean_test_close` | 清粉测试关闭/中止 |
| `UI_Debug_Laser_Power_Set_Ntf` | — | UI→Glavo | `DebugLaserPowerPayload` | `send_laser_power_set` | 激光手动功率 0–100 |
| `UI_Debug_Laser_Power_Reset_Ntf` | — | UI→Glavo | Empty | `send_laser_power_reset` | 取消手动功率、恢复外控 |
| `UI_Debug_Laser_AlarmReset_Ntf` | — | UI→Glavo | Empty | `send_laser_alarm_reset` | 激光器报警复位（1301） |
| `UI_Debug_Print_H264_Record_Set_Ntf` | — | UI→Glavo | `DebugOnOffPayload` | `send_print_h264_record_set` | 调试页全局开关：打印期连续 H264 录像（仅本次运行） |
| `UI_Debug_Print_H264_Record_Get_Ntf` | — | UI→Glavo | Empty | `request_print_h264_record_state` | 查询当前“打印期连续 H264 录像”开关状态 |
| `UI_Debug_Clean_Record_Set_Ntf` | — | UI→Glavo | `DebugOnOffPayload` | `send_clean_record_set` | 调试页全局开关：清粉录像（仅本次运行） |
| `UI_Debug_Clean_Record_Get_Ntf` | — | UI→Glavo | Empty | `request_clean_record_state` | 查询当前“清粉录像”开关状态 |
| `UI_Debug_Env_Sim_Set_Ntf` | — | UI→Glavo | `DebugOnOffPayload` | `send_env_sim_set` | 调试页全局开关：环境上报模拟（仅本次运行，默认关；开则 `McuLink` 用变化测试数据驱动 `APP_Update_Ntf`，关则用串口真实数据） |
| `UI_Debug_Env_Sim_Get_Ntf` | — | UI→Glavo | Empty | `request_env_sim_state` | 查询当前“环境上报模拟”开关状态 |
| `UI_Debug_Env_Sim_Powder_Set_Ntf` | — | UI→Glavo | `DebugOnOffPayload` | `send_env_sim_powder_set` | 环境模拟粉量缺粉开关（仅 Env Sim=On 可设；切换后延迟 5 秒生效） |
| `UI_Debug_Env_Sim_Powder_Get_Ntf` | — | UI→Glavo | Empty | `request_env_sim_powder_state` | 查询当前“环境模拟粉量缺粉”开关状态 |
| `UI_Debug_FirstLayer_Visual_Check_Set_Ntf` | — | UI→Glavo | `DebugOnOffPayload` | `send_first_layer_visual_check_set` | 调试页开关：首层视觉检测（仅本次运行，默认关） |
| `UI_Debug_FirstLayer_Visual_Check_Get_Ntf` | — | UI→Glavo | Empty | `request_first_layer_visual_check_state` | 查询当前“首层视觉检测”开关状态 |
| `GLAVO_Debug_Laser_Ntf` | — | Glavo→UI | `DebugLaserStatePayload` | `register_laser` | 激光器状态/报警/功率/温度 |
| `GLAVO_Debug_Print_H264_Record_Ntf` | — | Glavo→UI | `DebugOnOffPayload` | `register_print_h264_record` | 回读当前“打印期连续 H264 录像”开关状态 |
| `GLAVO_Debug_Clean_Record_Ntf` | — | Glavo→UI | `DebugOnOffPayload` | `register_clean_record` | 回读当前“清粉录像”开关状态 |
| `GLAVO_Debug_Env_Sim_Ntf` | — | Glavo→UI | `DebugOnOffPayload` | `register_env_sim` | 回读当前“环境上报模拟”开关状态 |
| `GLAVO_Debug_Env_Sim_Powder_Ntf` | — | Glavo→UI | `DebugOnOffPayload` | `register_env_sim_powder` | 回读当前“环境模拟粉量缺粉”开关状态 |
| `GLAVO_Debug_FirstLayer_Visual_Check_Ntf` | — | Glavo→UI | `DebugOnOffPayload` | `register_first_layer_visual_check` | 回读当前“首层视觉检测”开关状态 |
| `UI_Debug_Fan_Duty_Ntf` | — | UI→Glavo | `DebugFanPayload` | `send_fan_duty` | 仅设风扇占空比（0–100） |
| `UI_Debug_PrintParam_Set_Ntf` | — | UI→Glavo | `DebugPrintParamPayload` | `send_print_layer_param` | 打印层参数（层厚 + 平衡×1000） |
| `GLAVO_Debug_Sensor_Ntf` | — | Glavo→UI | `DebugSensorPayload` | `register_sensor` | 7 路传感器 |
| `GLAVO_Debug_Axis_Ntf` | — | Glavo→UI | `DebugAxisStatePayload` | `register_axis` | Z/S/D + Z 工作坐标 |
| `GLAVO_Debug_Device_Ntf` | — | Glavo→UI | `DebugDeviceStatePayload` | `register_device` | 设备行状态 |
| `GLAVO_Debug_Galvo_Ntf` | — | Glavo→UI | `DebugGalvoStatePayload` | `register_galvo` | 振镜 X/Y |
| `GLAVO_USB_Hotplug_Ntf` | — | Glavo→UI | `UsbHotplugPayload` | `register_usb_hotplug` | USB 挂载状态变化通知（文件页 USB Tab 自动刷新） |
| `UI_Mcu_Version_Ntf` | — | UI→Glavo | Empty | `send_mcu_version_query` | 进入 Settings/About 页查询一次 MCU 固件版本（串口 A2/0x10 TLV 0x01） |
| `GLAVO_Mcu_Version_Ntf` | — | Glavo→UI | `McuVersionPayload` | `register_mcu_version` | MCU 版本上报（TLV 0x02）：About 页显示 `V<major>.<minor>.<patch>.<laser>.<z>` |
| `UI_Galvo_Version_Ntf` | — | UI→Glavo | Empty | `send_galvo_version_query` | 进入 Settings/About 页读取振镜控制板版本缓存（TCP 0x0F 首包，非实时查询下位机） |
| `GLAVO_Galvo_Version_Ntf` | — | Glavo→UI | `GalvoVersionPayload` | `register_galvo_version` | 振镜板版本：TCP 连上后首包主动推一次；About 进页 Req 再回缓存。显示 `V<(v>>16)>.<(v>>8)&0xff>.<(v&0xff)>` |
| `GLAVO_Device_Event_Ntf` | — | Glavo→UI | `DeviceEventPayload` | `register_device_event` | 设备事件/告警上报，喂消息中心。只发数值码 + 等级 + 触发时刻，文案在 UI 侧码表（`ui_message_store`）。码表见 [`doc/设备通信协议-数据结构定义（v0.9）.md`](../doc/设备通信协议-数据结构定义（v0.9）.md) 第五节。**Glavo 侧尚未实现**，当前只有 UI 侧接好了订阅 |
| `GLAVO_Alarm_Report_Ntf` | — | Glavo→UI | `AlarmReportPayload` | `register_alarm_report` | MCU 单条报警/EVENT 上报（串口 0xA3/0x20 TLV 0x01 原样透传，McuLink 不查字典）。UI 侧 `ui_alarm_state` 按 state_mask 推导生命周期并喂消息中心；字典与开发指导见 `doc/主控板串口通信协议 - 报警&事件表.csv`、`doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md` |
| `UI_Alarm_Reset_Ntf` | — | UI→Glavo | `AlarmResetReqPayload` | `send_alarm_reset` | 请求复位某条 CONDITION 报警（串口 0xA3/0x82 TLV 0x01，alarm_code 小端）。ACK 不代表复位成功，最终状态以后续 `GLAVO_Alarm_Report_Ntf` 为准 |
| `UI_Alarm_Snapshot_Ntf` | — | UI→Glavo | Empty | `send_alarm_snapshot_req` | 请求 MCU 回一轮报警快照（串口 0xA3/0x81 TLV 0x05 len=0）。MCU **不会主动推**快照，不发这条就永远收不到；触发点：UI 启动、串口重连、进消息中心、30s 轮询、开始打印前 |
| `GLAVO_Alarm_Snapshot_Ntf` | — | Glavo→UI | `AlarmSnapshotPayload` | `register_alarm_snapshot` | 报警快照的**一个 slot**（串口 0xA3/0x81 TLV 0x04 原样透传）。响应**稀疏**：只回 `(active｜latched)!=0` 的 slot，没回的按全零处理；`nonzero_slot_count=0` 表示当前无报警。槽位→报警号用固定公式 `alarm_snapshot_code_of()` 不查表。攒帧与原子对齐在 UI 侧 `ui_alarm_state` 做。用于重启/重连后补齐——单条上报只在状态变化时发，错过就再也收不到 |
| `GLAVO_Alarm_Reset_Result_Ntf` | — | Glavo→UI | `AlarmResetResultPayload` | `register_alarm_reset_result` | 复位**失败**回执（串口 0xA3/0x82 TLV 0x04）。只在失败时发，成功仍以后续 NORMAL 单报为准；Seq 固定 0，按 alarm_code 关联。UI 收到即解除 `reset_pending`，不用干等 10s 超时 |

`CameraStreamInfoPayload.dma_mode` 当前固定为 `1`：
- 预览统一走 **dma fd 旁路**：后端经主 IPC（`/tmp/upc_camera.sock`）`SCM_RIGHTS` 附带 dma_buf fd；UI 侧 `MsgPort::take_peer_dma_fds(APP_CAMERA)` 接收。SHM `/upc_camera_preview` 仅保留 slot 状态元数据，像素不再走 SHM payload。
- `dma_socket_path` 字段为 ABI 保留，当前未使用独立 socket。

### TCP CMD=0x0A（通用紧急指令）字段口径

`app/protocol/tcp/tcp_tx.*` 组帧时，`DATA[1..3]` 按 **协议枚举值原样透传**，不得按 bool 压缩：

- `DATA[1] Stop`：`1=急停`，`0=不修改`
- `DATA[2] Pause`：`2=暂停`，`1=继续`，`0=不修改`
- `DATA[3] Red`：`2=激光`，`1=红光`，`0=不修改`

实现注意：

- 禁止写法：`payload[x] = v ? 1u : 0u;`（会把 `2` 误压成 `1`）
- 正确写法：`payload[x] = v;`
- 典型场景：
  - 调试红光开关：`stop=0 pause=0 red=1/2`
  - 打印暂停/恢复：`stop=0 pause=2/1 red=0`
  - 急停：`stop=1 pause=0 red=0`

---

## 消息契约 · 新增消息（固定四步）

1. **`upc_contract.hpp`** — 增加 `MsgType` + POD + 注释（本表同步一行）
2. **发送侧进程** — `subscribe` + `handle_*` 或 `publish_*`（Glavo / Camera / UI 各自实现）
3. **UI 侧** — 若需界面交互：`ui_msg_proxy.*` 或 `camera_msg_proxy.*` 的 `send_*` + `register_*`（纯后端互访可跳过）
4. **Broker 转发** — `target_id` 指向其它 ModuleId 时由 Broker 转发；业务侧一般无需改 `ipc_wire`

Wire 协议版本：`WIRE_VERSION`（[`ipc_wire.hpp`](../ipc/ipc_wire.hpp)）。仅当帧格式变更时递增。

---

## 消息契约 · 多后端拓扑

### 5.1 现状（Broker 已落地）

```
rongsu_broker ──listen──► /tmp/upc_broker.sock ◄── UI / Glavo / Camera / Visual (Client)
```

- 各进程：`set_mode_ipc_client(ModuleId, UPC_BROKER_SOCKET_DEFAULT)`
- Broker：`set_mode_ipc_broker`（或 `IpcMsgBus::init_broker`），零业务 subscribe
- **后端互访**：Glavo `publish_async` 且 `target_id = APP_MEDIA`（录像）/`APP_CAMERA`（预览）→ Broker 转发
- **大流量**：预览 NV12 走 dma fd 旁路 + `/upc_camera_preview` 元数据环；控制仍走 MsgPort

详见下文 [§ IPC](#ipc跨进程通信)。

### 5.2 历史（已废弃 Hub 拓扑）

旧方案：UI Hub + Glavo/Camera 各自 listen（`/tmp/upc_glavo.sock`、`/tmp/upc_camera.sock`）。仓库已切换为 Broker，勿再使用 `set_mode_ipc_hub` / `set_mode_ipc_server`。

新增进程时在 `ModuleId` 追加（如 `APP_LOG`），`MsgType` 用域前缀（`LOG_` 等），遵循 §2 命名规范。

---

## 文件职责

| 路径 | 职责 | 维护方 |
|------|------|--------|
| `common/COMMON.md` | 本文档、消息总表 | 架构 / 双组评审 |
| `common/upc_contract.hpp` | **唯一源码契约**（MsgType / Payload / IPC 配置 / 日志开关 / 进程表 / WIRE ABI） | 双组评审 |
| `common/middleware/message_bus/msg_runtime.hpp` | 进程内 `Message` / `MsgPayload`（依赖 BufferPool） | 平台组 |
| `common/CMakeLists.txt` | 统一静态库 `upc::common`（兼容别名 `upc::ipc` 等） | 平台组 |
| `common/fs_manifest/gcode_manifest.*` | Local `/home/Gcode/config.json` 读写（UI 读列表 + Glavo 写） | 平台 / UI / Glavo |
| `common/fs_manifest/print_history_manifest.*` | `/home/media/video/config.json`（Media 写，UI 历史页读） | 平台 / Media / UI |
| `common/fs_manifest/media_paths.hpp` | `/home/media/{photo,video}` 路径宏；`UPC_TIMELAPSE_PLAYBACK_PNG`（默认 1=PNG 播延时） | 平台 / Media / UI |
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

---

## IPC（跨进程通信）

## 文件职责

| 文件 | 职责 |
|------|------|
| `msg_port.*` | 业务门面：InProcess / IPC 模式切换 |
| `ipc_msg_bus.*` | Broker 路由 / Client 出站、订阅表、epoll I/O |
| `ipc_reg.*` | connect 后 reg 帧与上下线 notify |
| `ipc_wire.*` | 线协议编解码（`WIRE_VERSION`） |
| `ipc_endpoint.*` | Unix stream socket listen/connect/帧收发 |
| `ipc_poller.*` | Linux epoll 封装（非 Linux 走 fallback 轮询） |
| `ipc_reg.*` | connect 后 reg 帧与上下线 notify |
| `dma_heap_buffer.*` | dma-heap 单个缓冲区分配 |

`dma_heap_buffer.*` 同时被 UI `RgaDmaBuf`、camera `CameraDmaFrameSource` 与 Media
`timelapse_util::create_raw_thumb_from_nv12`（RTHM 缩略图）复用，避免各自维护 dma-heap 候选列表。

**契约勿随意改**：`upc_contract.hpp`、本文档消息总表

## 拓扑（Broker）

```
rongsu_broker ──listen──► /tmp/upc_broker.sock ◄── rongsu_ui / rongsu_glavo / rongsu_camera (Client)
```

- connect 后首帧 **reg**（`IPC_REG_MAGIC`，不进 MsgType）；同 `ModuleId` 重连踢旧连接。
- Broker **零业务 subscribe**；仅按 wire `target_id` 转发（含 UPDF fd 透传）。
- 路由靠 **`MsgType` + `target_id`**，不靠 socket fd。
- 启动顺序：`broker → glavo → camera → ui`（`scripts/rongsu.sh`）。

## MsgPort 速查

| API | 用途 |
|-----|------|
| `set_mode_ipc_broker` | Broker 进程 listen |
| `set_mode_ipc_client(ModuleId)` | UI / Glavo / Camera 连 Broker |
| `subscribe` / `publish_async` / `process` | 共用 |
| `take_peer_dma_fds(ModuleId)` | UI 从指定 sender 取 SCM_RIGHTS 附带的 dma fd（预览开流） |
| `publish` | 同步投递（InProcess 或 IPC `publish_sync`；Glavo 工作线程慎用） |
| `publish_async_broadcast` | `target_id=0` 广播 |
| `peer_connected(ModuleId)` | Broker 在线 + reg notify 已知 peer |

### 进程主循环

- **UI**：`process(0)` → `lv_timer_handler()` → `process(min(delay,8))` → `process(0)`
- **Glavo / Camera**：`process(2)`（I/O + 本地队列投递）

## 发布与投递

### 出站（本进程 `publish_async`）

`send_outbound_from_local` 规则：

| 模式 | `target` | 行为 |
|------|----------|------|
| Client | 本进程 | 仅本地队列/同步投递 |
| Client | 其它 ModuleId | 经 Broker 转发（wire `target_id` 不变） |
| Client | `0`（广播） | Broker 转发至所有已注册 client（除发送方） |

### 入站（`deliver_frames`）

1. `wire_decode` → 可选 sender 校验（见下）
2. `IPC_RX` 日志
3. **Broker**：按 `target_id` 转发至对应 client（含 UPDF fd）；本进程 Client 只 `deliver_local`
4. 否则 `deliver_local` → 匹配 `MsgType` + `module_id==target_id` 的订阅回调
5. 无订阅 → `IPC_DELIVER miss`（仅对应在本地应处理的报文）

### `publish_async` vs `publish_sync`

| | async | sync |
|--|-------|------|
| 本地投递 | `enqueue_local` → 下次 `process()` | 立即 `deliver_local` |
| 发送失败 | 打 `IPC_TX` Warn | 静默返回 false |

跨进程回复 UI：**必须** `publish_async`，目标 `ModuleId::UI`。

## Broker 转发（后端互访）

```
Glavo publish_async(target=APP_CAMERA)
  → Client 发往 Broker
  → Broker 按 target_id 转发
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
| Broker | `/tmp/upc_broker.sock`（`UPC_BROKER_SOCKET` 可覆盖） |

`upc_contract.hpp` §1：`UPC_IPC_PEER_RECONNECT_INTERVAL_MS`（Client 重连）、`UPC_IPC_PEER_STARTUP_WAIT_MS`（`rongsu.sh` 等待 listen）。

## 新增后端（平台层）

1. `upc_contract.hpp` — `ModuleId` / `MsgType` / Payload（contract 四步）
2. 新进程 `main` — `set_mode_ipc_client` + `process` 循环
3. `scripts/rongsu.sh` — 启动顺序（Broker 最先）
4. UI 业务 — 独立 `XxxMsgProxy`

大流量：控制走 MsgPort；像素走主 IPC SCM_RIGHTS 传 dma fd + `ipc_dma_frame_ring` 元数据环（camera 预览：NV12，UI 侧 RGA/SW 转 BGRX）。
详见 `app/camera/lib/camera_dma_frame_source.*` 与 `app/ui/lib/core/camera_preview.*`。



---

## Middleware（进程内消息总线）

## 子模块

| 目录 | 职责 |
|------|------|
| `message_bus/` | `MsgBus`、`MsgType`、`MsgPayload`、异步队列 |
| `memory_pool/` | 固定块池、`BufferPool` |
| `debug/` | `MW_LOG_*`（middleware 内部调试） |

无 LVGL 依赖；UI / Glavo / Camera 均可链接。

## 核心类型（源码为准）

- **`Message`**：`type`、`source_id`、`target_id`、`payload`
- **`ModuleId`**：`UI`、`APP_GLAVO`、`APP_CAMERA` 等
- **`MsgType`**：见 本文档 [消息总表](#消息契约--消息总表)
- **`MsgPayload`**：Inline / BufferRef

改消息：**contract 四步** + `upc_contract.hpp`，不要只改文档。

## 用法要点

```cpp
// 订阅（Subscription 须持久保存）
auto sub = MsgBus::instance().subscribe(type, module_id, handler);

// 异步投递（跨线程 / 回复 UI 推荐）
MsgPort::instance().publish_async(msg);

// UI / 后端主循环消费队列
MsgPort::instance().process(timeout_ms);
```

跨进程时 UI/Glavo 用 **`MsgPort`**，内部再进 `MsgBus`。详见 [§ IPC](#ipc跨进程通信)。

## 打印数据路径（不经 MsgBus  payload）

```
ParserManager → RingBuffer → McuLink（TCP + 串口）
```

## 相关文档

- [app/glavo/GLAVO.md](../../app/glavo/GLAVO.md) · [app/ui/UI.md](../../app/ui/UI.md)

---

## Process（进程守护）

## 职责

| 文件 | 职责 |
|------|------|
| `process_ctl.cpp` | daemonize、PID 文件、信号处理、`process_log_boot_banner` |
| （已移除）`log_rotating.cpp` | 旧的按进程三文件轮转方案，已由 `/var/log/topic` 专题日志替代 |
| `upc_contract.hpp` §3 | 各进程 PID / 日志路径 |

## 进程配置速查

| 函数 | 可执行文件 | PID 文件 | 日志 |
|------|------------|----------|------|
| `process_config_ui()` | `rongsu_ui` | `/var/run/rongsu_ui.pid` | `/var/log/topic/*.log` |
| `process_config_glavo()` | `rongsu_glavo` | `/var/run/rongsu_glavo.pid` | `/var/log/topic/*.log` |
| `process_config_camera()` | `rongsu_camera` | `/var/run/rongsu_camera.pid` | `/var/log/topic/*.log` |

各进程 `main.cpp` 支持 `{start|stop|status|run}`；`start` 时 stderr/stdout 重定向到 `/var/log/topic/topic_boot_lifecycle.log`，业务日志由 `rs_log` 按专题写入 `/var/log/topic/*.log`。

- 启动脚本：[scripts/rongsu.sh](../scripts/rongsu.sh)

---

## Debug（日志与监控）

## 职责

| 文件 | 职责 |
|------|------|
| `rs_log.hpp` / `rs_log.cpp` | `UPC_LOG_*` 宏、`proc=` 进程名 |
| `upc_contract.hpp` §2 | 模块子开关 `UPC_MOD_*` |
| `rs_msg_format.cpp` | IPC 消息摘要格式化 |
| `upc_sys_monitor.*` | HostMonitor（CPU/内存/温度） |

## 模块开关（`UPC_DEBUG=ON` 时）

| 宏 | module 标签 | 典型进程 |
|----|-------------|----------|
| `UPC_MOD_GLAVO_SERVICE` | GlavoService | rongsu_glavo |
| `UPC_MOD_MCU_LINK` | McuLink | rongsu_glavo |
| `UPC_MOD_CAMERA_SERVICE` | CameraService | rongsu_camera |
| `UPC_MOD_CAMERA_PREVIEW` | CameraPreview | rongsu_ui |
| `UPC_MOD_ALARM_STATE` | AlarmState | rongsu_ui |
| `UPC_MOD_MSG_BUS` / `UPC_MOD_IPC` | MSGBUS / IPC | 全部 |

编译降噪示例：

```bash
cmake -B build -DUPC_DEBUG=ON \
  -DCMAKE_CXX_FLAGS="-DUPC_MOD_MCU_LINK=0 -DUPC_MOD_CAMERA_PREVIEW=0"
```

详阅 [doc/LOG_GUIDE.md](../doc/LOG_GUIDE.md)
