# glavo/ — Glavo 进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_glavo`** · 库 **`upc::glavo`**（CMake target `upc_glavo`）

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 进程守护、IPC Server、McuLink 连接、`MsgPort::process` 循环 |
| `lib/glavo_service.*` | IPC 业务 + 工作线程 + 订阅/发布（USB 目录/打印/门控/进度）；`init()` 内绑定 RingBuffer → `McuLink` |
| `lib/mcu_link.*` | TCP 运动下发 + 串口握手/传感器/换层 Snap 触发；RX 侧解析 MCU 报警单条上报（0xA3/0x20）与快照分片（0xA3/0x81），攒齐后并入 SOC 1120：slot 分片 `GLAVO_Alarm_Snapshot_Ntf`→UI，完整位图 `GLAVO_Alarm_Cloud_Snapshot_Ntf`→MQTT；单报 `GLAVO_Alarm_Report_Ntf` 双发 UI+MQTT。调试页 Alarm Inject 只向 MQTT 补单报。MCU 同源 1120 丢弃（SOC 氧门独占） |
| `lib/print_video_notify.*` | 换层 Snap / 录像会话 → Media（经 UI Hub） |
| `lib/glavo_service.*`（early print prep） | 起印前半段 Z/外门/内门独立线程。关内门后进入测高：粗测 8 点光路、`H>3` 在位、再调高度。无板弹放板窗 |
| `lib/height_points.hpp` | 测高/校准共用 8 点、Z 零点偏置、门限、单点采样。单点等光斑盖住 Visual 的 8s 等新帧 + 1.5s 认斑 |
| `lib/height_lut_2d.*` | 8 点二维高度 LUT |
| `lib/height_calibration.*` | 调试页高度校准（关舱灯 + Red_On → 回零先下降 20mm → 写 LUT → 开舱灯，红光保持） |
| `lib/plate_height_measure.*` | 打印前测高（开关默认开）。回零直接发 Z Home。粗测扫满 8 点，至少 5 个看到光斑才通过，否则光路失败阻断；接着 `H>3` 判断在位，有板再调 Z。期间 SOC 舱灯全关，结束保持关 |
| `lib/galvo_motion_encode.*` | 调试运动包编码 + 五点坐标（点位测试） |
| `lib/soc_sensors.*` | SOC 本地：舱内外 PMSA UART、AHT20（≥2s）、双路 CPM121、IIO 氧气（须 `/run/o2_ready`；V=raw×3.3/2^31，100Ω；K 读 `/userdata/CAL/o2_cal.json` 缺省≈655）。链路启动即周期采集。六路舱灯进程启动即默认全开。六路舱灯/`fa_valve` sysfs 调试可随时写。开门前 PM 判定与持续吹风不开关阀。背光不在本模块。 |
| `lib/spi_status_led.*` | SPI 灯效板：`/dev/spidev1.0` 40 颗 WS 单色。CONDITION+ACTIVE 红，`print_running_` 橙，空闲蓝。进程启动即打开（不等串口）。业务路径只改状态排队；专用 led 线程做 open/ioctl，避免堵 McuLink 串口线程。 |
| `lib/spi_pm25_lcd.*` | ST7735 SPI 小屏：按 fbdev name `st7735rdrmfb` 打开，常显 3~5 µg/m³ 缓变模拟 PM2.5（舱外 PMSA 暂不可靠；布局自 `doc/spilcd`）。`arm_soc_peripherals` 启动；`poll_soc_env_tick` 刷值；无效显示 `--`。同一个值也作 `pm25_indoor` 推首页/监控页（Home PM2.5）与 MQTT `chamber_ext`。 |
| `lib/n2_ble_client.*` / `n2_ble_transport.*` / `n2_ble_parse.*` | 制氮机 BLE：BlueZ GATT Client + ASCII 解析；推 `APP_N2_Status_Ntf`（v1 UI 不订阅） |
| `lib/device_config.hpp` | TCP/串口默认、换层/Layer Snap 开关、`GLAVO_TEST_GCODE_PATH`、PM UART/`fa_valve`/`lgt*` 路径宏、`GLAVO_N2_BLE_ENABLE` |
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
3. `McuLink::arm_soc_peripherals()`：六路舱灯全开 + SPI 灯效空闲蓝 + SPI PM2.5 小屏布局（不等串口）
4. `McuLink::connect_tcp` + `open_serial` + `start_link_thread`，然后 `GlavoService::start`
   - TCP 刚连上：只发 `CMD=0x0A` `Red=1`（普通红光）
   - 串口 MCU 工程版本上报成功后：再发 `0x12` 关激光使能；出队后再补一次 `Red=1`（避免 `0x12` 掐瞄准红光）；同一次再发 Z Home（与 Manual 复位同指令 0x11）。打印中则挂起，空闲后再发。Env Sim 假版本不走这两条
5. 主循环 `port.process(2)`

## IPC 处理

| 消息 | 处理 |
|------|------|
| `Fs_DirOpen_Req` | **仅 USB** 扫盘；非 Usb 源忽略并回空列表 |
| `Fs_Delete_Ntf` | 删除 Local 文件（磁盘 + config.json） |
| `UI_Print_Prepare_Req` | 轻量预处理（USB 复制；清单无 `layer_count` 时先读 Snowpath 文件头 `; Total layers:`，对不上再单趟扫 `M3000`；打开解析器并写 config.json）。头或清单已给出总层数时不数行、不把 `get_total_lines()` 写回 `line_count`；Start 不再补扫 |
| `UI_Parser_Exit_Ntf` | 取消解析（stop_parse + 清会话） |
| `UI_Print_Start_Req` | 置打印态 → **起印前半段**（Z→−10 / 外门 / 关内门）→ 测高一次做完光路、在位、调高。粗测 8 点里至少 5 个看到光斑，不足发 `OpticalPathFail`。`H>3` 才算有板，否则弹放板窗。放板不另占准备进度步，Job 卡停在 Vision check。Keep Z 或 Vision Check 关则整段测高跳过。 |
| ↑ 起印失败 | 打印态在 `launch_print_from_prepared` 最前面就置（检测那十几秒 UI 已在打印页），所以「运动还没开始就失败」的分支**一律走 `abort_print_start()`** 回滚，否则 UI 停在打印页 0%；失败时 SOC 舱灯全开，并按 Manual Top 路径把 Z 绝对回 **1.00 mm**（已在 Home 则空操作）。**Keep Z Position** 开则不发这次回零 |
| `UI_Print_Terminate_Req` | 外门确认/起印前半段线程/关内门套件/光路/测高/开扫前 Prepare，以及尚未 `Ready` 的氧门/首层：打断挂起、回红光安全态，**先** `TerminateDone` 让 UI 进结束页，再发 Z 绝对到 **−10 mm**（`send_z_goto_abs`，不等到位）。**Keep Z Position** 开则这一步不发。准备线程等待可被 `print_prep_abort_` 立刻打断。已 `Ready` 出光后则 `stop_parse` + `request_terminate` + `on_print_abort`，不再动 Z。终止时内门保持当前态 |
| `UI_Print_Pause_Req` | `set_user_dispatch_paused(true)`，仅停 TCP 运动下发 |
| `UI_Print_Resume_Req` | `set_user_dispatch_paused(false)`，恢复 TCP 运动下发 |
| `UI_Debug_TestImage_Set_Ntf` | 红光循环打印 `GLAVO_TEST_GCODE_PATH`（默认 `/userdata/test/test.gcode`）；无录像/延时/换层握手/首页打印态；Off 仅 `request_terminate`，不切激光 |
| `UI_Debug_Galvo_AbsMove_Ntf` | 调试页 Abs X/Y + Move：snowx 口径单点 TCP 运动包（`send_galvo_abs_pos`）；打印中拒绝 |
| `UI_Debug_PlateHeight_Adj_Set_Ntf` | 界面名 **Vision Check**。打印前测高总开关（仅本次运行，默认开）。开则关内门后做粗测光路、在位和调 Z；**无 LUT、查表失败或精测失败阻断起印**（`PlateHeightFail`）。粗测看到光斑不足 5 个是 `OpticalPathFail`。设置页 Advanced Print 与调试页共用这一开关。测高成功、Z 收到约 1mm 之后，再按 `UI_Debug_Plate_Work_Z_Set_Ntf` 的目标补 `(target-1)` mm（正值向上，默认 1 不补） |
| `UI_Debug_Skip_Outer_Door_Set_Ntf` | 跳过外门检测+关内门套件（仅本次运行，**默认关=要做**）；开则不检门、不开→关边沿也不跑套件 |
| `UI_Debug_Hold_Prep_Z_Set_Ntf` | 保持当前 Z（仅本次运行，**默认关**）。开则整个打印会话（`print_running_`）内绝对不动 Z：不起印 −10、跳过测高与工作高度抬升、内门套件前不降 Z、准备终止/起印失败不回 Z，Manual/调试 Z 点动与 Home 一律丢弃。会话结束后可再动 Z |
| `UI_Debug_Galvo_Param_Save_Ntf` | 写 `cal.csv` 第 4 行中心偏移 + `ParserContext::load_calibration`；打印/任一振镜测试进行中拒绝 |
| `UI_PM25_Req` / 门控 Req | 直接读舱内 PM2.5 缓存后回 `APP_PM25_Rsp`（不开阀、不平均）。≥ 阈值则 UI 再发持续吹风；无效或低于阈值直接开门。门控关则跳过、直接判合格 |
| `UI_OpenDoor_Req` | worker 发开门指令后由 `door_open_worker_` 轮询传感器（默认最长 `GLAVO_DOOR_OPEN_TIMEOUT_MS`=1s，传感器报开即返回），线程内 `publish_async` Rsp；重复请求直接拒。门控关时同样下发，只是前面的 PM2.5/吹粉已跳过。UI 失败后隔 300ms 最多再试 2 次，仍失败 toast 并收 overlay |
| `UI_Transfer_Demo_Prep_Req` | 一键演示前半段（与 Transfer Box 取件开关无关）：独立线程 Z 绝对到 −100 → 固定关一次内门 → 开外门（同 `UI_OpenDoor_Req` 传感器轮询，演示侧最多 3 次）。**不发转运**。打印中/准备中/门轮询中拒绝。UI 成功后弹装盒页，Done 走 `UI_Transport_Req` |
| `UI_Transfer_Demo_Confirm_Req` | **已不用**：装盒 Done 走 `UI_Transport_Req`；收到回 `ok=0` |
| `UI_Debug_Soc_Led_Set_Ntf` / `Get` | 六路舱灯 `lgt1l`…`lgt3r` sysfs，不看 MCU 版本 |
| `UI_Debug_Cabin_Pm_Valve_Set_Ntf` / `Get` | 舱内 PM `fa_valve`，不看 MCU 版本；业务采样结束**强制关阀**并回读 Off |
| `UI_Debug_O2_Calib_Req` / `Get_Ntf` | 单点标定 K（参考浓度由 UI 输入）→ `/userdata/CAL/o2_cal.json`；Req 只回 `GLAVO_Debug_O2_Calib_Rsp`（SocSensors 独立线程采样，不堵 worker）；Get 回 `Ntf`；打印/测高/测试中拒绝 |
| `UI_Debug_Status_Led_Set_Ntf` / `Get` | SPI 灯板（`SpiStatusLed`）验色接管：强制 blue/orange/red 或裸 RGB，每次必发一帧；`force=0` 释放回自动推导 |
| `UI_Debug_Spi_Pm25_Lcd_Set_Ntf` / `Get` | SPI 小屏测试刷屏：开则约 500ms 整屏轮换纯色并 `pwrite` fb；关则恢复舱外 PM 布局 |
| `UI_Alarm_Reset_Ntf` | `serial_build_alarm_reset` 组 0xA3/0x82 帧直发串口；结果不回 Rsp，以 MCU 后续单条上报为准 |
| `UI_Debug_Alarm_Inject_Ntf` | 调试页 Alarm Inject：转 `GLAVO_Alarm_Report_Ntf` **只发 MQTT**（不上 UI、不改快照/灯效） |
| `APP_N2_Status_Ntf` | `N2BleClient` 后台线程 `publish_async` 到 UI（v1 UI 不订阅） |

### 制氮机 BLE

- 广播名 `N2_Machine`，特征 `0x1B9A` Notify ASCII。无 libdbus 时 `UPC_GLAVO_HAS_BLUEZ=0` 空跑。`GLAVO_N2_BLE_ENABLE=0` 不启线程。

内门 **1ST bit1（UP_SWPD）只表示全开**（1=全开，0=不在全开位）。**上电不自动关内门**。进套件前读静止位：开仅全开跳过；关仅非全开跳过（关到位未装，静止 0 不当成必须再关）。未知则开/关都跑。Manual / 调试页点击只开关门，不联动 Z。起印关/开内门仍绝对到 **−10 mm** 后发门指令，不回原高度。外门开→关自动补关：先绝对到 **−100 mm**（±1，最多 60 s），等 **2 s**，再回到下降前的高度。这两段 Z 运动期间 `GLAVO_Debug_Inner_Door_Busy_Ntf.reserved[0]=1`，Manual 的 Top/Bottom 与 Z 升降、复位置灰；若收到报警 **1204**（Z axis motion error，active/latched）则立刻解锁并中止本次等待，且不再自动回位。补关套件确定要跑（过了跳过判断）起到结束，同一条的 `reserved[1]=1`（`clearance_kit_active_`，中间关门那 2 s 也保持），UI 据此盖监控页遮罩；结束时与 `on=0` 同一帧清零。挡门和基板视觉都不决定这次位移。**Keep Z Position** 开则跳过下降和回位，门指令照发。套件进行中推 `GLAVO_Debug_Inner_Door_Busy_Ntf`（Manual / 调试 Inner Door 禁钮，含停止）；连点开/关在入队时去重。起印与外门开→关边沿按静止位决定是否关；边沿当时套件忙 / 打印发送中 / 预检中则记下，事后补关。与转运盒流程无共享状态。

### 传感器

- **1ST（TLV 0x02）**：只认 Tlv-Len=15（滤芯氧/滤芯气压/基板/NTC/开关/红外）。粉量缺粉看 OUTL/OUTR；发给 UI 的 `powder_bits` 低 2 位为「左/右匣仍有粉」。设置页 Powder Low Check 默认关；关掉后仍上报粉量位，但不因缺粉暂停、不发 `APP_Print_Powder_Low_Ntf`（仅本次运行）。
- **ADC（TLV 0x07）**：主板电气量，与 1ST/FANBOARD 分帧；当前只识别以免 SENSOR FAIL，不推 UI。
- **FANBOARD（TLV 0x08）**：风机转速/母线电压电流/故障码/状态机（默认与 ADC 同节拍约 1000ms）。经 `GLAVO_Debug_Fan_Ntf` 推调试页风扇卡。`fan_status_bits` bit7=0 表示风机板链路异常，此时 MCU 把转速等字段填 0，UI 按无效显示 `--`。原 EXT TLV 0x06 已停发。
- **SOC 采集**：舱外常显 PM、舱内 PM（阀开窗口可信）、AHT20 温湿度（最短 2s）、CPM121 气压、IIO 氧气（须 `/run/o2_ready` 后读 `in_voltage0_raw`；本板无 scale，V=raw×3.3/2^31，I[uA]=V/100×1e6，O2%=100×(1−e^(−I/K))）。**K** 优先读 `/userdata/CAL/o2_cal.json`（调试页 **O2 Calib**：输入参考浓度后一键标定，启动加载、标定后立即生效）；缺文件回退出厂默认 `GLAVO_O2_K`≈655。首页 `pm25_indoor` **不是**舱外 PMSA 实测，而是 SPI 小屏同一个显示值（见下「SPI PM2.5 小屏」）；`pm1_indoor` / `pm10_indoor` 仍是舱外实测。进程启动即六路舱灯 `lgt1l`…`lgt3r` 写 1 全开（不等串口；调试页仍可单独关）。PMSA：`pmsa outdoor|cabin` 开关口/解帧在 `topic_print_flow.log`（`GlavoService`）；推 UI 的环境量在 `topic_serial_sensor.log` 的 `soc:`（5s：舱内氧、温湿、两路气压、舱外/舱内 PM1.0/PM2.5/PM10，`<0` 无效；`pm_out` 是舱外实测，`pm25_disp` 是推给首页/MQTT、写进小屏的舱外 PM2.5 显示值）。MCU `sensor:` 的 `filterO2` 是滤芯氧。
- **打印氧门（SOC）**：MCU 串口 `PRINT_CONDITION_OK`（`5A A5 BB A4 01 … 02`）之后：氧门开启时 SOC 发洗气开（`0xA1/0x86` ON）并须 SOC 氧 **&lt; `GLAVO_O2_PRINT_MAX_PERCENT`（默认 0.8%）** 才发 `PRINT_START`；等氧期间氧高同样合成报警 **1120**（不 TCP 暂停），并推 Prepare 步 `Oxygen`。打印中氧 **≥ 0.8%** TCP 暂停并合成 **1120**（`GLAVO_Alarm_Report_Ntf`，AUTO），回落 **≤ `GLAVO_O2_PRINT_RESUME_PERCENT`（0.7%）** 自动恢复。终止/`PRINT_FINISHED` 时若本作业 SOC 开过阀则发洗气关。Env Sim / 测试图 / 调试页 **Skip O2 Gate**（默认关，仅本次运行）跳过：不起印洗气开、不等氧、打印中不因氧暂停、Prepare 不计氧步。`o2_hold_` 期间不判自然完成。MCU 快照在 Glavo 攒齐后若缺 slot3 则补 bit20（count+1），全零快照改为只发 slot3，避免 UI 对齐把 1120 清掉。
- **SPI 灯效板**：`SpiStatusLed` 写 `/dev/spidev1.0`。红=MCU CONDITION 且 ACTIVE；橙（RGB 高绿）=Confirm 后的打印会话；否则蓝。起印洗气等氧期间 1120 仍上报，灯保持打印绿，不切红；打印中氧高暂停仍红。EVENT / 仅 latched 不红。无设备只 Warn 一次。进程启动即打开，不等串口。报警/打印态只入队目标色，SPI transfer 在独立 led 线程，不堵串口心跳。
- **SPI PM2.5 小屏**：`SpiPm25Lcd` 按 `/sys/class/graphics/fb*/name == st7735rdrmfb` 打开（勿写死 fb0）。UI 逻辑 160×80，shadow 逆时针 90° 后 `pwrite` 到 80×160 fb（相对旧顺时针再转 180°，适配装配朝向）。**暂不接舱外实测**：`poll_soc_env_tick` 每拍先调 `advance_outdoor_pm25_display()` 生成 3~5 µg/m³ 缓变模拟值（舱外 PMSA 不可靠时的权宜，约 2.5 min/周期），再并进缓存推 UI / MQTT、写小屏，所以首页/监控页的 Home PM2.5、MQTT `chamber_ext.pm25` 与小屏同一拍同一个数（UI 侧按小屏同样的进位出一位小数，逐位一致）。舱外实测只剩调试页顶栏 PM2.5 与 `soc:` 日志的 `pm_out`。Env Sim 时 `poll_soc_env_tick` 直接返回，小屏与 `pm25_indoor` 都改用模拟器的 pm25。与灯效板/主屏 DRM 无关。调试页 **SPI LCD Test** 开则忽略 PM、周期整屏刷红/绿/蓝/白/黑（测 fb 提交；本板 `modetest` 亦可能仅背光）。
- **SOC→UI 推送**：`poll_soc_env_tick` 每 500ms 独立 `publish_async` `APP_Update_Ntf` + `GLAVO_Debug_Sensor_Ntf`，**不依赖** MCU 1ST。MCU 仍负责基板/粉量/门等；未收到 1ST 前这些字段顶栏显示 `--`。MQTT `telemetry/chamber` 用 SOC 舱内 PM/温湿/气压/氧，`telemetry/chamber_ext` 上报舱外 PM；无效氧发 `null` 不夹成 0。`sensor_v4` 固定填 1（ABI 保留，不再表示版本切换）。
- **报警上云**：单报与完整快照经 IPC 进 `rongsu_mqtt`，映射 `snowpod/.../alarm/report`，订 `alarm/ack`；落盘 outbox `/userdata/snowpod/alarm-outbox`，流序号与 `lost_report_ranges` `/userdata/snowpod/alarm-stream.json`。快照条目元数据 unknown。调试页 **Alarm Inject** 只补单报 MQTT（不改快照）。契约 [doc/soc-alarm-contract-v2.md](../../doc/soc-alarm-contract-v2.md)。
- **开门前吹风（真实门控开且舱内 PM ≥ 阈值时 UI 才会发；收到即执行）**：占空比 60% 开风机并持续吹，每 200ms 读舱内 PM 缓存；有效值低于阈值则关风机，UI 再开门。不开电磁阀。取消则关风机。

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

`GlavoService::send_print_progress`，打印中按作业类型二选一；消息**双发 UI 与
`APP_XFR`**（桌面端 `GET /print/status` 轮询缓存）。载荷含百分比、剩余分钟（清单 `duration_sec` × (100−进度)/100 向上取整；无预估为 0）、环境读数、
当前层/总层，以及 `layer_height_um`（当前铺粉高度）。暂停回执、终止回执、起印拒绝
同样双发。`GLAVO_Print_State_Ntf` 双发 UI 与 Xfr：UI 在 Snowpath 起印时进打印页（停印钮、外门/放板确认），Xfr 写桌面 `/print/status` 的开始/结束边沿。云端 `state` 走 `GLAVO_Cloud_State_Ntf`：`idle` 的 `ready`/`not_ready` 看振镜 TCP，`printing` 跟准备步、送层、暂停和结果页终态（`finished`/`canceled`/`aborted`）。结果页收掉前不回 `idle`。`updating` 由 MQTT 在 OTA 进度变化时另写 `state`，不经过 Glavo。

| 策略 | 适用 | 打印中区间 | 说明 |
|------|------|-----------|------|
| 纯层台阶（首选） | G-code 且 `layer_count>0`，且 `GLAVO_ENABLE_LAYER_SNAP=1` | `1–98` | `已完成层 / 总层`。总层 = 清单 `layer_count`（3MF 来自 Metadata.layers；Snowpath G-code 来自文件头 `Total layers`；其它可来自 Prepare 预扫的 `M3000`）；已完成层 = 换层 Snap 计数。层内静止，打完一层跳一格 |
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
