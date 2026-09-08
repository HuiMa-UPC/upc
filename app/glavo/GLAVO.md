# glavo/ — Glavo 进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_glavo`** · 库 **`upc::glavo`**（CMake target `upc_glavo`）

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 进程守护、IPC Server、McuLink 连接、`MsgPort::process` 循环 |
| `lib/glavo_service.*` | IPC 业务 + 工作线程 + 订阅/发布（USB 目录/打印/门控/进度）；`init()` 内绑定 RingBuffer → `McuLink` |
| `lib/mcu_link.*` | TCP 运动下发 + 串口握手/传感器/换层 Snap 触发；RX 侧解析 MCU 报警单条上报（0xA3/0x20）与快照分片（0xA3/0x81），`publish_async` 原样转发 `GLAVO_Alarm_Report_Ntf` / `GLAVO_Alarm_Snapshot_Ntf` 给 UI（不查字典、不攒帧） |
| `lib/print_video_notify.*` | 换层 Snap / 录像会话 → Media（经 UI Hub） |
| `lib/print_job_state.*` | 发送包计数、完成/错误标志（McuLink 与进度共用） |
| `lib/device_config.hpp` | TCP/串口默认、换层/Layer Snap 开关、`GLAVO_TEST_GCODE_PATH` |
| `lib/mcu_ota_session.*` | Galvo 以太网 OTA：扫描 `/userdata/image/galvo` 最高版本 AB 对 |
| `lib/mcu_serial_ota_session.*` | MCU 串口 OTA：扫描 `/userdata/image/mcu` 最高版本 AB 对 |
| `common/fs_manifest/gcode_manifest.*` | Local `config.json` 读写（与 UI 共用；Prepare/Delete 时更新） |

解析库见 [parser/PARSER.md](../parser/PARSER.md)；线协议见 [protocol/PROTOCOL.md](../protocol/PROTOCOL.md)。

## 生产链路

```
UI → GlavoService（工作线程）→ ParserManager → RingBuffer → McuLink → 下位机
```

## 初始化（`main.cpp`）

1. `set_mode_ipc_client(APP_GLAVO)`
2. `GlavoService::init/start`
3. `McuLink::connect_tcp` + `open_serial` + `start_link_thread`
4. 主循环 `port.process(2)`

## IPC 处理

| 消息 | 处理 |
|------|------|
| `Fs_DirOpen_Req` | **仅 USB** 扫盘；非 Usb 源忽略并回空列表 |
| `Fs_Delete_Ntf` | 删除 Local 文件（磁盘 + config.json） |
| `UI_Print_Prepare_Req` | 轻量预处理（USB 复制、行数扫描、写 config.json） |
| `UI_Parser_Exit_Ntf` | 取消解析（stop_parse + 清会话） |
| `UI_Print_Start_Req` | 正式解析 + `begin_sending` + `PrintVideoNotify::on_print_start` |
| `UI_Print_Terminate_Req` | `stop_parse` + `request_terminate` + `on_print_abort` |
| `UI_Print_Pause_Req` | `set_user_dispatch_paused(true)`，仅停 TCP 运动下发 |
| `UI_Print_Resume_Req` | `set_user_dispatch_paused(false)`，恢复 TCP 运动下发 |
| `UI_Debug_TestImage_Set_Ntf` | 红光循环打印 `GLAVO_TEST_GCODE_PATH`（默认 `/userdata/test/test.gcode`）；无录像/延时/换层握手/首页打印态；Off 仅 `request_terminate`，不切激光 |
| `UI_Debug_Galvo_Param_Save_Ntf` | 写 `cal.csv` 第 4 行中心偏移 + `ParserContext::load_calibration`；打印/测试图进行中拒绝 |
| `UI_PM25_Req` / 门控 Req | 环境快照或占位 Rsp |
| `UI_Alarm_Reset_Ntf` | `serial_build_alarm_reset` 组 0xA3/0x82 帧直发串口；结果不回 Rsp，以 MCU 后续单条上报为准 |

跨进程回复 UI：**必须** `publish_async`（`GlavoService` 工作线程内禁止 `MsgPort::publish` 同步路径）。

### MCU / Galvo 固件 OTA

镜像命名与选包：[common/fs_manifest/OTA_IMAGE.md](../../common/fs_manifest/OTA_IMAGE.md)。  
`UI_Mcu_Ota_Start_Req`（UI 或 MQTT）→ `/userdata/image/galvo`；`UI_Serial_Mcu_Ota_Start_Req` → `/userdata/image/mcu`。只刷最高版本成对的 SlotA→SlotB，不成对或名称非法则启动失败。两路互斥。状态 `APP_*_Ota_Status_Ntf` 双发 UI 与 MQTT，供云端进度映射。

### 打印层录像（Glavo → Media）

开关（均在 `device_config.hpp`）：

| 宏 | 默认 | 作用 |
|----|------|------|
| `GLAVO_ENABLE_LAYER_CHANGE` | `1` | 串口换层/打印握手总开关 |
| `GLAVO_ENABLE_LAYER_SNAP` | `1` | 每层拍照录像 |

| 阶段 | 调用点 | IPC |
|------|--------|-----|
| 开始 | `GlavoService` 打印启动 | `APP_Print_Video_Session_Start_Ntf` |
| 每层 | `McuLink::link_try_layer_serial_dispatch` | `APP_Print_Layer_Snap_Ntf` |
| 末层（无尾 M3000） | `run_natural_complete_cleanup` | `APP_Print_Layer_Snap_Ntf`（防重复） |
| 完成 | `PrintCompleted` | `APP_Print_Video_Session_Finish_Ntf` |
| 中止 | 终止 / `PrintError` | `APP_Print_Video_Session_Abort_Ntf` |

`PrintVideoNotify::publish_to_camera`：`publish_async`，`target_id = APP_MEDIA`，经 [common/COMMON.md](../../common/COMMON.md) Hub 转发。落盘与封装见 [media/MEDIA.md](../media/MEDIA.md)。

## 待实现：设备事件上报（`GLAVO_Device_Event_Ntf`）

UI 侧的消息中心（左下通知浮层 + 侧栏红点）已经接好订阅，等 Glavo 发布。契约见
[common/COMMON.md](../../common/COMMON.md) 消息总表 + `DeviceEventPayload`：
只发数值码 + 等级 + `active` + 触发时刻，**不发文案**（人话在 UI 侧码表）。
码表与 [doc/设备通信协议-数据结构定义（v0.9）.md](../../doc/设备通信协议-数据结构定义（v0.9）.md)
第五节那张告警码表逐行对应，`DeviceEventCode` 的枚举名即该表 code 的驼峰写法。

- `stateful` 事件触发发 `active=1`、恢复发 `active=0`，两条边沿都要发；
- `one_shot` 只发 `active=1`；
- `event_ts_ms` 填**实际触发/恢复**时刻而不是发出时刻（断网补发时两者会差很多）。

## 红线

- 禁止 `GcodeWorker::instance()` 等具体解析器；用 `ParserManager::active()`
- 禁止链接 `ui::port`、lvgl
- 新切片格式：[parser/PARSER.md](../parser/PARSER.md)

## 日志

- McuLink：`module=McuLink`，开关 `UPC_MOD_MCU_LINK`
- 详见 [common/COMMON.md § 专题日志字典](../../common/COMMON.md#专题日志字典varlogtopiclog)（McuLink 按内容分流到 `topic_serial_*` / `topic_tcp_*` / `topic_laser_*` / `topic_print_flow.log`）

## 相关文档

- [ui/UI.md](../ui/UI.md) · [camera/CAMERA.md](../camera/CAMERA.md) · [media/MEDIA.md](../media/MEDIA.md) · [common/COMMON.md](../../common/COMMON.md)
