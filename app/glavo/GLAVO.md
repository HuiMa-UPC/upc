# glavo/ — Glavo 进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_glavo`** · 库 **`upc::glavo`**（CMake target `upc_glavo`）

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 进程守护、IPC Server、McuLink 连接、`MsgPort::process` 循环 |
| `lib/glavo_service.*` | IPC 业务 + 工作线程 + 订阅/发布（USB 目录/打印/门控/进度）；`init()` 内绑定 RingBuffer → `McuLink` |
| `lib/mcu_link.*` | TCP 运动下发 + 串口握手/传感器/换层 Snap 触发；RX 侧解析 MCU 报警单条上报（0xA3/0x20）与快照分片（0xA3/0x81），攒齐后并入 SOC 1120 再 `publish_async` `GLAVO_Alarm_Report_Ntf` / `GLAVO_Alarm_Snapshot_Ntf` 给 UI |
| `lib/print_video_notify.*` | 换层 Snap / 录像会话 → Media（经 UI Hub） |
| `lib/optical_path_check.*` | 打印前光路检测状态机（独立线程，结果回灌 worker） |
| `lib/height_points.hpp` | 测高/校准共用 8 点、Z 零点偏置、门限、单点采样 |
| `lib/height_lut_2d.*` | 8 点二维高度 LUT |
| `lib/height_calibration.*` | 调试页高度校准（关舱灯 + Red_On → 回零先下降 20mm → 写 LUT → 开舱灯，红光保持） |
| `lib/plate_height_measure.*` | 打印前两步测高调 Z（开关默认关；光路之后；回零先下降 20mm；开始再发关灯，结束不开舱灯） |
| `lib/galvo_motion_encode.*` | 调试运动包编码 + 五点坐标（点位测试与光路检测共用） |
| `lib/soc_sensors.*` | SOC 本地：舱内外 PMSA UART、AHT20（≥2s）、双路 CPM121、IIO 氧气（须 `/run/o2_ready`；V=raw×3.3/2^31，100Ω；K 读 `/userdata/CAL/o2_cal.json` 缺省≈655）。链路启动即周期采集。六路舱灯进程启动即默认全开。六路舱灯/`fa_valve` sysfs 调试可随时写；业务采样结束强制关阀。背光不在本模块。 |
| `lib/spi_status_led.*` | SPI 灯效板：`/dev/spidev1.0` 40 颗 WS 单色。CONDITION+ACTIVE 红，`print_running_` 橙，空闲蓝。进程启动即打开（不等串口）。 |
| `lib/device_config.hpp` | TCP/串口默认、换层/Layer Snap 开关、`GLAVO_TEST_GCODE_PATH`、PM UART/`fa_valve`/`lgt*` 路径宏 |
| `lib/mcu_ota_session.*` | Galvo 以太网 OTA：扫描 `/userdata/image/galvo` 最高版本 AB 对 |
| `lib/mcu_serial_ota_session.*` | MCU 串口 OTA：扫描 `/userdata/image/mcu` 最高版本 AB 对；停等重传 + 阶段四版本确认 |
| `common/fs_manifest/gcode_manifest.*` | Local `config.json` 读写（与 UI 共用；Prepare/Delete 时更新） |

解析库见 [parser/PARSER.md](../parser/PARSER.md)；线协议见 [protocol/PROTOCOL.md](../protocol/PROTOCOL.md)。

## 生产链路

```
UI → GlavoService（工作线程）→ ParserManager → RingBuffer → McuLink → 下位机
```

## 初始化（`main.cpp`）

1. `set_mode_ipc_client(APP_GLAVO)`
2. `GlavoService::init`
3. `McuLink::arm_soc_peripherals()`：六路舱灯全开 + SPI 灯效空闲蓝（不等串口）
4. `McuLink::connect_tcp` + `open_serial` + `start_link_thread`，然后 `GlavoService::start`
   - TCP 刚连上：只发 `CMD=0x0A` `Red=1`（普通红光）
   - 串口 MCU 工程版本上报成功后：再发 `0x12` 关激光使能；出队后再补一次 `Red=1`（避免 `0x12` 掐瞄准红光）。Env Sim 假版本不走这条
5. 主循环 `port.process(2)`

## IPC 处理

| 消息 | 处理 |
|------|------|
| `Fs_DirOpen_Req` | **仅 USB** 扫盘；非 Usb 源忽略并回空列表 |
| `Fs_Delete_Ntf` | 删除 Local 文件（磁盘 + config.json） |
| `UI_Print_Prepare_Req` | 轻量预处理（USB 复制；清单无 `layer_count` 时才单趟扫 `M3000`；打开解析器并写 config.json）。已有总层数时不数行、不把 `get_total_lines()` 写回 `line_count`；Start 不再补扫 |
| `UI_Parser_Exit_Ntf` | 取消解析（stop_parse + 清会话） |
| `UI_Print_Start_Req` | 置打印态 → 光路检测 gate（若开）→ 基板测高 gate（`Plate Height Adj` 开且有 LUT 才跑）→ 正式解析 + 首层参考图 gate + `begin_sending` |
| ↑ 起印失败 | 打印态在 `launch_print_from_prepared` 最前面就置（检测那十几秒 UI 已在打印页），所以「运动还没开始就失败」的分支**一律走 `abort_print_start()`** 回滚，否则 UI 停在打印页 0% |
| `UI_Print_Terminate_Req` | 光路/测高阶段：打断对应线程并回红光安全态；否则 `stop_parse` + `request_terminate` + `on_print_abort` |
| `UI_Print_Pause_Req` | `set_user_dispatch_paused(true)`，仅停 TCP 运动下发 |
| `UI_Print_Resume_Req` | `set_user_dispatch_paused(false)`，恢复 TCP 运动下发 |
| `UI_Debug_TestImage_Set_Ntf` | 红光循环打印 `GLAVO_TEST_GCODE_PATH`（默认 `/userdata/test/test.gcode`）；无录像/延时/换层握手/首页打印态；Off 仅 `request_terminate`，不切激光 |
| `UI_Debug_Galvo_AbsMove_Ntf` | 调试页 Abs X/Y + Move：snowx 口径单点 TCP 运动包（`send_galvo_abs_pos`）；打印中拒绝 |
| `UI_Debug_OpticalPath_Check_Set_Ntf` | 打印前光路检测开关（仅本次运行，默认关）。开则 Confirm Print 后、出光前逐点等视觉识别；失败阻断并 `APP_Print_Start_Rejected_Ntf` |
| `UI_Debug_PlateHeight_Adj_Set_Ntf` | 打印前基板测高调 Z（仅本次运行，默认关）。开则 Confirm Print 后、光路之后测 8 点并 AXIS_INC；无 LUT 或失败不阻断打印 |
| `UI_Debug_Galvo_Param_Save_Ntf` | 写 `cal.csv` 第 4 行中心偏移 + `ParserContext::load_calibration`；打印/任一振镜测试进行中拒绝 |
| `UI_PM25_Req` / 门控 Req | 独立线程开 `fa_valve` 采 3s 舱内平均后回 `APP_PM25_Rsp`（不堵 worker）。门控关则跳过、直接判合格 |
| `UI_Debug_Soc_Led_Set_Ntf` / `Get` | 六路舱灯 `lgt1l`…`lgt3r` sysfs，不看 MCU 版本 |
| `UI_Debug_Cabin_Pm_Valve_Set_Ntf` / `Get` | 舱内 PM `fa_valve`，不看 MCU 版本；业务采样结束**强制关阀**并回读 Off |
| `UI_Debug_O2_Calib_Req` / `Get_Ntf` | 单点标定 K（参考浓度由 UI 输入）→ `/userdata/CAL/o2_cal.json`；Req 只回 `GLAVO_Debug_O2_Calib_Rsp`（SocSensors 独立线程采样，不堵 worker）；Get 回 `Ntf`；打印/测高/测试中拒绝 |
| `UI_Alarm_Reset_Ntf` | `serial_build_alarm_reset` 组 0xA3/0x82 帧直发串口；结果不回 Rsp，以 MCU 后续单条上报为准 |

### 传感器

- **1ST（TLV 0x02）**：只认 Tlv-Len=15（滤芯氧/滤芯气压/基板/NTC/开关/红外）。粉量缺粉看 OUTL/OUTR；发给 UI 的 `powder_bits` 低 2 位为「左/右匣仍有粉」。
- **EXT（TLV 0x06）**：电气量+风机，解析到即打日志。
- **SOC 采集**：舱外常显 PM、舱内 PM（阀开窗口可信）、AHT20 温湿度（最短 2s）、CPM121 气压、IIO 氧气（须 `/run/o2_ready` 后读 `in_voltage0_raw`；本板无 scale，V=raw×3.3/2^31，I[uA]=V/100×1e6，O2%=100×(1−e^(−I/K))）。**K** 优先读 `/userdata/CAL/o2_cal.json`（调试页 **O2 Calib**：输入参考浓度后一键标定，启动加载、标定后立即生效）；缺文件回退出厂默认 `GLAVO_O2_K`≈655。首页 `pm25_indoor` 为舱外 PMSA。进程启动即六路舱灯 `lgt1l`…`lgt3r` 写 1 全开（不等串口；调试页仍可单独关）。
- **打印氧门（SOC）**：MCU 串口 `PRINT_CONDITION_OK`（`5A A5 BB A4 01 … 02`）之后：氧门开启时 SOC 发洗气开（`0xA1/0x86` ON）并须 SOC 氧 **&lt; `GLAVO_O2_PRINT_MAX_PERCENT`（默认 0.8%）** 才发 `PRINT_START`；等氧期间氧高同样合成报警 **1120**（不 TCP 暂停）。打印中氧 **≥ 0.8%** TCP 暂停并合成 **1120**（`GLAVO_Alarm_Report_Ntf`，AUTO），回落 **≤ `GLAVO_O2_PRINT_RESUME_PERCENT`（0.7%）** 自动恢复。终止/`PRINT_FINISHED` 时若本作业 SOC 开过阀则发洗气关。Env Sim / 测试图 / 调试页 **Skip O2 Gate**（默认关，仅本次运行）跳过：不起印洗气开、不等氧、打印中不因氧暂停。`o2_hold_` 期间不判自然完成。MCU 快照在 Glavo 攒齐后若缺 slot3 则补 bit20（count+1），全零快照改为只发 slot3，避免 UI 对齐把 1120 清掉。
- **SPI 灯效板**：`SpiStatusLed` 写 `/dev/spidev1.0`。红=MCU CONDITION 且 ACTIVE；橙=Confirm 后的打印会话；否则蓝。EVENT / 仅 latched 不红。无设备只 Warn 一次。进程启动即打开，不等串口。
- **SOC→UI 推送**：`poll_soc_env_tick` 每 500ms 独立 `publish_async` `APP_Update_Ntf` + `GLAVO_Debug_Sensor_Ntf`，**不依赖** MCU 1ST。MCU 仍负责基板/粉量/门等；未收到 1ST 前这些字段顶栏显示 `--`。MQTT `telemetry/chamber` 用 SOC 舱内 PM/温湿/气压/氧，`telemetry/chamber_ext` 上报舱外 PM；无效氧发 `null` 不夹成 0。`sensor_v4` 固定填 1（ABI 保留，不再表示版本切换）。
- **开门前清粉（真实门控开时 UI 才会发；收到即执行）**：风机固定吹 10s → 关风机 → 开阀 3s 平均后关阀；仍超标则循环。

跨进程回复 UI：**必须** `publish_async`（`GlavoService` 工作线程内禁止 `MsgPort::publish` 同步路径）。

### MCU / Galvo 固件 OTA

镜像命名与选包：[common/fs_manifest/OTA_IMAGE.md](../../common/fs_manifest/OTA_IMAGE.md)。  
`UI_Mcu_Ota_Start_Req`（UI 或 MQTT）→ `/userdata/image/galvo`；`UI_Serial_Mcu_Ota_Start_Req` → `/userdata/image/mcu`。只刷最高版本成对的 SlotA→SlotB，不成对或名称非法则启动失败。两路互斥。状态 `APP_*_Ota_Status_Ntf` 双发 UI 与 MQTT，供云端进度映射。

MCU 串口 OTA 是停等协议（规则 [MCU_OTA丢包重传机制.md](../../doc/MCU_OTA丢包重传机制.md)）：只认 `0xBB` 业务应答；镜像须为打包工具的 128 对齐产物，原样切片、不补齐。`FINISHED_ACK=1` 只表示落槽，成功看阶段四 `value[5]==目标槽 && value[6]==4`。会话进行中 link 线程不发其它串口命令（擦除窗硬静默；阶段四只发版本查询）。

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

### 打印进度（`APP_Print_Progress_Ntf`）

`GlavoService::send_print_progress`，打印中按作业类型二选一：

| 策略 | 适用 | 打印中区间 | 说明 |
|------|------|-----------|------|
| 纯层台阶（首选） | G-code 且 `layer_count>0`，且 `GLAVO_ENABLE_LAYER_SNAP=1` | `1–98` | `已完成层 / 总层`。总层 = 清单 `layer_count`（3MF 来自 Metadata.layers；其它可来自 Prepare 预扫的 `M3000`）；已完成层 = 换层 Snap 计数。层内静止，打完一层跳一格 |
| 包进度（回退） | OVF / 无 `M3000` 的 G-code / 关闭 Layer Snap | `0–95` | `sent_packets / denom`，再与层锚点 7:3 融合（`fuse_send_and_layer_progress_95`） |

两者共用尾段：`95–99` 按 MCU cmd/data cache 占用估排空，`100` 只跟 `force_complete` / `job_completed`。

红线两条：

- **打印中绝不出现 99%**。UI（`control.cpp` `print_ready_to_exit`）把 `>=99` 且双缓冲为 0 当作「可退打印页」，而换层排空时 cache rate 正好为 0，中途出现 99 会被误判成打印结束。
- **层数口径差半拍**：`M3000` 标在一层扫描前，Snap 在扫描后；文件头那条被跳过、末层由 `link_try_final_layer_snap` 补一次。所以 `done_layers` 必须钳位，不得因层计数超前而提前顶满。

纯层段一层内百分比恒定，但同一条 Ntf 还带 PM2.5/温度/氧含量，故保留 cache rate 变化推送 + 3s 保活推送，避免 UI 环境读数僵住。

同一条 Ntf 另带 `current_layer` / `total_layers` 供 UI 显示层号。`total_layers` 就是上面那个预扫计数，**0 表示给不出**（OVF / 无 `M3000` / 关闭 Snap），UI 见 0 就整块隐藏，不显示占位。`current_layer` 报的是「正在打第几层」= 已完成层 + 1，同样受上面那条钳位约束，末层排空时不会越过 `total_layers`。

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
