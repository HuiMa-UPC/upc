# MCU串口通信协议文档
本文档描述 MCU 当前对外串口通信的职责划分、现阶段已落地的 UART4 上位机调试协议，以及 UART2 与 SOC 正式通信协议中已启用和预留的内容。

当前默认串口参数：`115200, 8, N, 1`。

## UART2 与 UART4 职能划分

| 串口 | 当前职能 | 协议状态 | 说明 |
| :--- | :--- | :--- | :--- |
| `UART4` | 上位机调试 / 联调串口 | 已落地 | 使用 `0x80~0xFF` 调试命令空间；业务 task 直接处理对应高位命令 |
| `UART2` | SOC 正式通信 | 部分启用 | 使用 `0x00~0x7F` 正式命令空间；当前已启用打印请求 / 打印控制两条 SOC 命令，其余正式协议仍预留 |

**当前约定：**

- 由上位机发起的 UART4 调试业务帧，MCU 必须回复。
- 若 MCU 主动上报的业务帧需要确认，则上位机也必须回复 ACK。
- 警报信息及一些动作完成信息可由 MCU 主动上报。
- UART2 与 UART4 共用帧格式和通信任务，但不共享线上命令号：`Cmd < 0x80` 属于 UART2，`Cmd >= 0x80` 属于 UART4。
- UART4 调试命令使用独立高位命令号；若业务需要 UART4 调试能力，应在 `comm_task_app.h` 中定义高位命令，并由对应业务 task 直接处理。
- UART2 的 SOC 正式通信协议当前仅启用打印相关命令，其余内容仍预留，详见文末 SOC 通信协议章节。

## UART4 上位机调试协议帧格式

| 字段 | 长度(字节) | 说明 |
| :--- | :--------- | :--- |
| SOF1 | 1 | 固定 `0x5A` |
| SOF2 | 1 | 固定 `0xA5` |
| FrameType | 1 | `0xAA`：上位机→MCU 业务命令；`0xBA`：MCU→上位机 对 `0xAA` 的 ACK；`0xBB`：MCU→上位机 主动上报/结果；`0xAB`：上位机→MCU 对 `0xBB` 的 ACK |
| Port | 1 | 端口号 / 任务路由 |
| Cmd | 1 | 命令（见下文 Cmd 表，与 `comm_task_app.h` 一致） |
| Seq | 1 | **帧序号**，由**发送方**设定；**应答方**回复 ACK （或 Response） 时 **Seq 与对应请求帧保持一致** |
| Len | 1 | Payload 字节数 |
| Payload | Len | 配合 Cmd；无参数时 Len=0。多条 TLV 顺序拼接 |
| CheckSum | 1 | 除 CheckSum 外，从 `SOF1` 至 Payload 末字节的累加和之**低 8 位** |

**整帧长度：** `8 + Len` 字节（含 `SOF1`、`SOF2`、`FrameType`、`Port`、`Cmd`、`Seq`、`Len`、`Payload`、`CheckSum`）。

**SOF / FrameType / Seq 约定（当前用于 UART4 上位机调试链路）：**

- 固定起始头：`SOF1 = 0x5A`、`SOF2 = 0xA5`，上位机 与 MCU 收发两侧保持一致，仅用于帧同步。
- 上位机→MCU 业务命令：`FrameType = 0xAA`，Seq 由上位机分配（建议递增），每帧新请求带独立 Seq。
- MCU→上位机 对 `0xAA` 的 ACK：`FrameType = 0xBA`，且 `Port / Cmd / Seq` 与对应请求帧保持一致。
- MCU→上位机 主动上报或结果帧：`FrameType = 0xBB`；若该帧需要上位机确认，则由 MCU 分配 Seq，并由上位机以 `0xAB` 回复。
- 上位机→MCU 对 `0xBB` 的 ACK：`FrameType = 0xAB`，且 `Port / Cmd / Seq` 与对应 `0xBB` 帧保持一致。
- ACK 帧的 Payload 也仅使用 TLV；无附加信息时 `Len = 0`。
- 多帧并行时，收发双方均用 Seq 将 ACK/结果 与原始业务帧一一对应。

## Payload（TLV）

每条 TLV：**Type(1B) + Len(1B) + Value(Len)**。

示例：上报温度，`Type = 0x00`，`float` → Len=4。整帧 Payload 为多条 TLV 拼接。

---

Cmd 表达本帧作用；额外参数放在 Payload 的 TLV 中。

**示例**

主机下发：`5A A5 AA <Port> <Cmd> <Seq> 03 00 01 64 <Sum>`

- `03`：Payload 长 3 字节
- `00 01 64`：TLV — Type=`0x00`，Len=`1`，Value=`0x64`（100）
- `<Seq>`：本帧序号，例如 `0x00`
- `<Sum>`：从 `SOF1`（`0x5A`）到 `Value` 末字节（示例中为 `0x64`）的累加和低字节

从机可先回 **ACK**（`FrameType = 0xBA`，**Port/Cmd/Seq 与请求一致**，Payload 见固件 `CommReply2Soc`），执行完再主动上报结果帧（`FrameType = 0xBB`）；若该结果帧需要上位机确认，则上位机使用 `0xAB` 回复 ACK。

## 示例展示约定

- 下文命令详表中的**业务命令/ACK**统一按**完整串口帧**展示，即 `SOF1 + SOF2 + FrameType + Port + Cmd + Seq + Len + Payload + Sum`。
- 表头中的 `FrameType` 取值为 `0xAA / 0xBA / 0xBB / 0xAB`，其语义见上文“SOF / FrameType / Seq 约定”。
- `Sum` 一律按完整帧计算，即从 `SOF1` 开始累加到 Payload 末字节后取低 8 位。
- OTA 升级的**数据流阶段**为例外：启动命令 ACK 完成后，同一 UART 会切换为 **XMODEM 原始字节流**，不再带 `SOF1 + SOF2 + FrameType + ...` 封装，详见下文“**OTA 升级启动与数据流**”。


## 上位机调试协议

### 字段

本章对应当前 **UART4 上位机调试链路**，字段、命令和 TLV 定义与 `Core/Inc/comm_task_app.h` 一致：

#### Port

| 序号 | Port 值 | 含义 | `#define` |
| :--- | :----- | :--- | :-------- |
| 1 | `0xA0` | 转发至 dedust 任务 | `PORT_DEDUST_TASK` |
| 2 | `0xA1` | 转发至 gas_scrubbing 任务 | `PORT_GAS_TASK` |
| 3 | `0xA2` | 转发至 general 任务 | `PORT_GENERAL_TASK` |
| 4 | `0xA3` | 转发至 alarm_safety 任务 | `PORT_ALARM_TASK` |
| 5 | `0xA4` | 转发至 printing_proc 任务 | `PORT_PRINTING_TASK` |
| 6 | `0xA5` | 转发至 sensors_manager 任务 | `PORT_SENSORS_TASK` |
| 7 | `0xA6` | 转发至 ota 任务 | `PORT_OTA_TASK` |
| 8 | `0xA7` | 转发至 motion 任务 | `PORT_MOTION_TASK` |
| 9 | `0xA8` | 转发至 z_motor 任务 | `PORT_Z_MOTOR_TASK` |
| 10 | `0xA9` | 转发至 powder（落粉/粉路）任务 | `PORT_POWDER_TASK` |
| 11 | `0xAA` | 转发至 laser 任务 | `PORT_LASER_TASK` |

**说明（Z / S / D 轴运动与 Port）：** **`CMD_MOTION_Z_MOVE`（`0xB2`）、`CMD_MOTION_S_MOVE`（`0xB3`）、`CMD_MOTION_D_MOVE`（`0xB4`）** 及其载荷中的 **`TLV_MOTOR_Z_*` / `TLV_MOTOR_S_*` / `TLV_MOTOR_D_*`（SOC 常规轴控）**，SOC **须使用 `PORT_GENERAL_TASK`（`0xA2`）**；经 **`general_task_app`** 入队 **`xQueue_TaskToMotion`** 后由 **`motion_task_app`** 解析（Z 再转 **`z_motor_task_app`**）。若误用 **`PORT_MOTION_TASK`（`0xA7`）**，帧仍会进入 **`xQueue_TaskToMotion`**，但 **`motion_task_app` 不对上述三 Cmd 做 InterBoard 直达解析**，轴控不会生效，**请勿**依赖该路径。

#### Cmd

Cmd for `PORT_GENERAL_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0xA1` | POST / 上电自检等结果上报 | `CMD_POST_RESULT` |
| 2 | `0xAA` | 设备唤醒 | `CMD_MACHINE_WAKE` |
| 3 | `0xB0` | 电机使能/失能及结果 | `CMD_MOTOR_ENABLE` |
| 4 | `0xB1` | 运动状态查询或上报 | `CMD_MOTION_STATUS` |
| 5 | `0xB2` | Z 轴运动指令 | `CMD_MOTION_Z_MOVE` |
| 6 | `0xB3` | S 轴运动指令 | `CMD_MOTION_S_MOVE` |
| 7 | `0xB4` | D 轴运动指令 | `CMD_MOTION_D_MOVE` |
| 8 | `0xB5` | 打印层参数设置（层厚 + 平衡系数） | `CMD_PRINTING_LAYER_PARAM` |

Cmd for `PORT_ALARM_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x81` | 报警事件/报警快照上报 | `CMD_ALARM_EVT_REPORT` |
| 2 | `0x82` | SOC/HMI 对报警的确认、复位等操作 | `CMD_ALARM_EVT_RESPONSE` |

Cmd for `PORT_GAS_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x86` | 手动控制洗气开关 | `CMD_GAS_MANUAL_CONTROL` |

Cmd for `PORT_SENSORS_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x9A` | 传感器参数设置（当前用于风机 / 筛粉电机 / 照明 / 门控 / 内门 / 基板加热 / 激光使能） | `CMD_SENSOR_SET_PARA` |
| 2 | `0x9B` | 传感器数值上报（氧含量 / 压力 / 温度 / 湿度等，典型为 MCU→SOC） | `CMD_SNESOR_VALUE_REPORT` |

Cmd for `PORT_POWDER_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x8A` | 供粉相关命令（粉桶开关等） | `CMD_POWDER_SUPPLY` |

Cmd for `PORT_LASER_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x87` | 激光器手动功率控制 | `CMD_LASER_MANUAL_CONTROL` |

**说明（Cmd 编号）：** 当前调试/联调阶段使用的 **`CMD_*` / `TLV_*` / `PORT_*`** 定义以 `def_cmd.h`、`def_tlv.h` 及相关协议头文件为准；UART4 调试命令统一使用 `0x80~0xFF` 区间。收到 UART4 帧后，通信任务会按原始高位 Cmd 投递给对应业务 task。不同 **Port** 下可复用同一 Cmd 字节，由 **Port + Cmd + TLV** 共同定界。

### 命令详表

命令以 **帧内 Cmd** 区分业务，**Payload 内 TLV 的 Type** 再区分子含义；与 `Core/Inc/comm_task_app.h` 一致。

**校验和：** 完整帧按 `Sum = (uint8_t)(SOF1 + SOF2 + FrameType + Port + Cmd + Seq + Len + Payload 逐字节之和)`；下文若沿用紧凑写法，则表内 `Sum` 为从 `FrameType` 开始计算的结果。

**载荷长度上限：** 单条 TLV 的 `Value` 长度 ≤ `APP_COMM_TLV_VALUE_CAP`（当前 `20`）。

**说明：** 不同 **Cmd** 下可能出现相同的 **Tlv-Type 数值**；解析时必须以 **帧内 Cmd + TLV Type** 联合区分（与 `comm_task_app.h` 分组一致）。

**说明（本文档范围 — 非 SOC↔MCU 串口载荷）：** 下列 TLV 仅用于 **MCU 板内任务间队列**（如 PrintingProc ↔ Motion），**不通过本文档的串口帧载荷定义**；详表从略，板内语义见《MCU板内任务通信协议》及 `motion_task_app.c` 等实现：

- `TLV_MOTOR_S_LAYER`（`0x05`）、`TLV_MOTOR_S_LAYER_OK`（`0x06`）、`TLV_MOTOR_S_RESET`（`0x07`）、`TLV_MOTOR_S_RESET_OK`（`0x08`）
- `TLV_MOTOR_Z_LAYER`（`0x05`）、`TLV_MOTOR_Z_LAYER_OK`（`0x06`）
- `TLV_MOTOR_D_POWDER`（`0x05`）、`TLV_MOTOR_D_POWDER_OK`（`0x06`）（仅板内队列；**不**作为 SOC 串口侧 TLV 定义）

---

#### 手动开始洗气

**简介**

对应的TLV：`TLV_GAS_SCRUB_ON`（Type `0x01`，Cmd `CMD_GAS_MANUAL_CONTROL`）

SOC→MCU：通过 `PORT_GAS_TASK` 下发手动开始洗气命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_GAS_TASK` (`0xA1`) | `CMD_GAS_MANUAL_CONTROL` (`0x86`) | `TLV_GAS_SCRUB_ON` (`0x01`) | `0` | （无） | 手动开始洗气 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA1 | 0x86 | 0x01 | 0x02 | 0x01     | 0x00    | 0xD4 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA1+0x86+0x01+0x02+0x01+0x00)`。

**Copy**: 5A A5 AA A1 86 01 02 01 00 D4

---

#### 手动关闭洗气

**简介**

对应的TLV：`TLV_GAS_SCRUB_OFF`（Type `0x02`，Cmd `CMD_GAS_MANUAL_CONTROL`）

SOC→MCU：通过 `PORT_GAS_TASK` 下发手动关闭洗气命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_GAS_TASK` (`0xA1`) | `CMD_GAS_MANUAL_CONTROL` (`0x86`) | `TLV_GAS_SCRUB_OFF` (`0x02`) | `0` | （无） | 手动关闭洗气 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA1 | 0x86 | 0x01 | 0x02 | 0x02     | 0x00    | 0xD5 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA1+0x86+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 AA A1 86 01 02 02 00 D5

---

#### 报警快照上报与报警操作

**简介**

报警通信使用 `PORT_ALARM_TASK`（`0xA3`）：

- MCU→SOC/HMI 主动上报单条报警状态：`CMD_ALARM_SINGLE_REPORT`（`0x20`）
- MCU→SOC/HMI 主动上报当前报警快照：`CMD_ALARM_EVT_REPORT`（`0x81`）
- SOC/HMI→MCU 下发报警确认/复位操作：`CMD_ALARM_EVT_RESPONSE`（`0x82`）

当前报警显示建议以 **单条报警状态上报** 为主。SOC/HMI 收到 `CMD_ALARM_SINGLE_REPORT` 后按 `alarm_code` 更新对应报警；完整快照仍保留，用于启动、重连、丢包恢复或全量校准。

**Cmd 与 TLV**

`CMD_ALARM_SINGLE_REPORT`（`0x20`）：

| Tlv-Type | `#define` | 方向 | Tlv-Len | Value | 说明 |
| -------- | --------- | ---- | ------- | ----- | ---- |
| `0x01` | `TLV_ALARM_SINGLE_REPORT` | MCU→SOC | `9` | 见下方单条报警 payload | 单条报警状态上报 |

`CMD_ALARM_EVT_REPORT`（`0x81`）：

| Tlv-Type | `#define` | 方向 | Tlv-Len | Value | 说明 |
| -------- | --------- | ---- | ------- | ----- | ---- |
| `0x04` | `TLV_ALARM_EVT_SNAPSHOT` | MCU→SOC | `15` | 见下方快照 payload | 报警完整快照分片 |

`CMD_ALARM_EVT_RESPONSE`（`0x82`）：

| Tlv-Type | `#define` | 方向 | Tlv-Len | Value | 说明 |
| -------- | --------- | ---- | ------- | ----- | ---- |
| `0x01` | `TLV_ALARM_EVT_RESET` | SOC→MCU | `2` | `uint16_t alarm_code` 小端 | 请求复位某条报警 |
| `0x02` | `TLV_ALARM_EVT_ACK` | SOC→MCU | `2` | `uint16_t alarm_code` 小端 | 确认已知晓某条报警 |

说明：

- `TLV_ALARM_EVT_RAISE / RTN / CLEARED` 当前主要用于 MCU 板内报警任务链路或本地事件记录；SOC/HMI 主动显示刷新以 `TLV_ALARM_SINGLE_REPORT` 为主，`TLV_ALARM_EVT_SNAPSHOT` 作为全量同步兜底。
- `TLV_ALARM_EVT_RESET_RESULT` 当前用于业务 task 向 `AlarmSafetyTask` 反馈复位失败/拒绝，属于板内任务间消息，不作为 SOC 串口侧命令。

**单条报警 payload（`TLV_ALARM_SINGLE_REPORT`）**

`TLV_ALARM_SINGLE_REPORT` 在某条报警状态发生变化时发送。报警身份使用稳定的 `alarm_code`，SOC/HMI 不需要通过 snapshot mask bit 反查这条报警。

| Value 字节 | 含义 | 编码 |
| ---------- | ---- | ---- |
| `value[0..1]` | 报警代码 `alarm_code` | `uint16_t` 小端 |
| `value[2]` | 报警 severity | `uint8_t`，`1: information`，`2: warning`，`3: fault`，`4: trip` |
| `value[3]` | 当前状态 mask | `uint8_t`，bit0: active，bit1: latched，bit2: acked |
| `value[4]` | life state | `uint8_t`，`0: normal`，`1: active unacked`，`2: active acked`，`3: rtn latched unacked`，`4: rtn latched acked` |
| `value[5..8]` | Notes/details 报警备注/细节 | `uint32_t` 小端 |

**命令组成：MCU→SOC 单条报警状态**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_ALARM_TASK` (`0xA3`) | `CMD_ALARM_SINGLE_REPORT` (`0x20`) | `TLV_ALARM_SINGLE_REPORT` (`0x01`) | `9` | 单条报警 payload | MCU 主动上报单条报警状态 |

示例 payload（报警号 `1205`，information，active=1，latched=1，acked=0，life state=active unacked，Notes=0）：

```text
01 09 B5 04 01 03 01 00 00 00 00
```

解释：

- `01`：Tlv-Type = `TLV_ALARM_SINGLE_REPORT`
- `09`：Tlv-Len = 9
- `B5 04`：`alarm_code = 1205`
- `01`：报警 severity = information
- `03`：状态 mask = active + latched
- `01`：life state = active unacked
- `00 00 00 00`：Notes/details = 0

**报警快照 payload（`TLV_ALARM_EVT_SNAPSHOT`）**

`TLV_ALARM_EVT_SNAPSHOT` 每帧携带一个 mask word。当前报警数量小于 32 时只有 1 帧；若后续报警数量超过 32，会按 `word_index` 分多帧发送。

| Value 字节 | 含义 | 编码 |
| ---------- | ---- | ---- |
| `value[0]` | mask word 总数 | `uint8_t` |
| `value[1]` | 当前分片的 word index | `uint8_t` |
| `value[2]` | 当前最高报警等级 `highest_severity` | `uint8_t` |
| `value[3..6]` | `active_mask[word_index]` | `uint32_t` 小端 |
| `value[7..10]` | `latched_mask[word_index]` | `uint32_t` 小端 |
| `value[11..14]` | `unacked_mask[word_index]` | `uint32_t` 小端 |

mask bit 含义以固件发布时同步的 `Tools/Alarm_Config/alarm_mask_map.csv` 为准。上位机不要硬编码 bit 含义，应加载该表建立映射：

```text
(word_index, bit_index) -> macro / name
```

每个报警 bit 的位置由表中以下字段决定：

```text
word_index
bit_index
bit_mask_hex
```

解析时，上位机读取 snapshot 中的 `value[1]` 作为 `word_index`，遍历当前 word 内 `bit_index = 0..31`。如果 `active_mask / latched_mask / unacked_mask` 中对应 bit 为 1，则通过 `alarm_mask_map.csv` 查出报警 `macro / name` 并刷新显示。

`alarm_mask_map.csv` 的上位机解析方法见 `Docs/alarm_mask_map_上位机使用说明.md`。

**命令组成：MCU→SOC 报警快照**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_ALARM_TASK` (`0xA3`) | `CMD_ALARM_EVT_REPORT` (`0x81`) | `TLV_ALARM_EVT_SNAPSHOT` (`0x04`) | `15` | 快照 payload | MCU 主动上报报警快照 |

示例 payload（仅示意）：

```text
04 0F 01 00 02 03 00 00 00 03 00 00 00 01 00 00 00
```

解释：

- `04`：Tlv-Type = `TLV_ALARM_EVT_SNAPSHOT`
- `0F`：Tlv-Len = 15
- `01`：mask word 总数 = 1
- `00`：word index = 0
- `02`：highest_severity = 2
- `03 00 00 00`：active_mask[0] = `0x00000003`
- `03 00 00 00`：latched_mask[0] = `0x00000003`
- `01 00 00 00`：unacked_mask[0] = `0x00000001`

**命令组成：SOC→MCU 报警 ACK / RESET**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_ALARM_TASK` (`0xA3`) | `CMD_ALARM_EVT_RESPONSE` (`0x82`) | `TLV_ALARM_EVT_ACK` (`0x02`) | `2` | `alarm_code` 小端 | 用户/SOC 已确认看到报警 |
| `PORT_ALARM_TASK` (`0xA3`) | `CMD_ALARM_EVT_RESPONSE` (`0x82`) | `TLV_ALARM_EVT_RESET` (`0x01`) | `2` | `alarm_code` 小端 | 用户/SOC 请求复位报警 |

示例：SOC 请求复位报警号 `1005`（`0x03ED`，小端为 `ED 03`）：

```text
Payload = 01 02 ED 03
```

解释：

- `01`：Tlv-Type = `TLV_ALARM_EVT_RESET`
- `02`：Tlv-Len = 2
- `ED 03`：alarm_code = `1005`

示例：SOC 确认报警号 `1005`：

```text
Payload = 02 02 ED 03
```

解释：

- `02`：Tlv-Type = `TLV_ALARM_EVT_ACK`
- `02`：Tlv-Len = 2
- `ED 03`：alarm_code = `1005`

**报警操作语义**

- `ACK`：表示用户/SOC 已知晓报警。普通报警只设置 `acked` 并触发 ACK 状态上报；若该报警定义为 `APP_ALARM_CLEAR_ACK`，则 `ACK` 会直接清除该信息提示类报警，对外只上报最终 cleared/normal 状态。
- `RESET`：请求解除报警锁存态。若异常条件仍存在，本地复位类报警会拒绝；业务复位类报警会由 `AlarmSafetyTask` 转发给对应业务 task 执行复位动作。
- MCU 主动上报快照后，SOC/HMI 应以最新快照刷新显示。

---

#### 风机 PWM 占空比设置

**简介**

对应的TLV：`TLV_FAN_PWM_PARA`（Type `0x01`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发风机 PWM 占空比。与 `comm_task_app.h` 一致，`Value[0]` 为 `uint8_t`，合法范围为 `35`~`100`，分别表示 `35%`~`100%` 占空比。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_FAN_PWM_PARA` (`0x01`) | `1` | `uint8_t pwm_duty` | 风机 PWM 占空比，范围 `35`~`100` |

**命令示例（SOC→MCU，风机 PWM = 100%）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | -------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x03 | 0x01     | 0x01    | 0x64     | 0x52 |

注：`Len=3` = `Tlv-Type` + `Tlv-Len` + `1` 字节 Value。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x03+0x01+0x01+0x64)`。

**Copy**: 5A A5 AA A5 9A 01 03 01 01 64 52

---

#### 风机开

**简介**

对应的TLV：`TLV_FAN_SWITCH_ON`（Type `0x02`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发风机开启命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_FAN_SWITCH_ON` (`0x02`) | `0` | （无） | 风机开 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x02     | 0x00    | 0xED |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 02 00 ED

---

#### 风机关

**简介**

对应的TLV：`TLV_FAN_SWITCH_OFF`（Type `0x03`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发风机关停命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_FAN_SWITCH_OFF` (`0x03`) | `0` | （无） | 风机关 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x03     | 0x00    | 0xEE |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x03+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 03 00 EE

---

#### 筛粉电机正转

**简介**

对应的TLV：`TLV_POWDER_STRAINER_ENABLE`（Type `0x13`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发筛粉电机筛粉运行（正转）命令。原三通推杆已改为筛粉电机，与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_POWDER_STRAINER_ENABLE` (`0x13`) | `0` | （无） | 筛粉电机筛粉运行 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x13     | 0x00    | 0xFE |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x13+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 13 00 FE

---

#### 筛粉电机反转

**简介**

对应的TLV：`TLV_POWDER_STRAINER_DISABLE`（Type `0x14`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发筛粉电机反转倒渣命令。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_POWDER_STRAINER_DISABLE` (`0x14`) | `0` | （无） | 筛粉反转倒渣 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x14     | 0x00    | 0xFF |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x14+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 14 00 FF

---

#### 筛粉电机暂停

**简介**

对应的TLV：`TLV_POWDER_MOTOR_STOP`（Type `0x15`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发筛粉电机停止运行命令，停止正转筛粉或反转倒渣动作。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_POWDER_MOTOR_STOP` (`0x15`) | `0` | （无） | 筛粉电机停止运行 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x15     | 0x00    | 0x00 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x15+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 15 00 00

---

#### 打开照明

**简介**

对应的TLV：`TLV_LIGHT_ON`（Type `0x06`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发打开照明命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_LIGHT_ON` (`0x06`) | `0` | （无） | 打开照明 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x06     | 0x00    | 0xF1 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x06+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 06 00 F1

---

#### 关闭照明

**简介**

对应的TLV：`TLV_LIGHT_OFF`（Type `0x07`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发关闭照明命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_LIGHT_OFF` (`0x07`) | `0` | （无） | 关闭照明 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x07     | 0x00    | 0xF2 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x07+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 07 00 F2

---

#### 激光开使能

**简介**

对应的TLV：`TLV_LASER_ENABLE`（Type `0x11`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发激光开使能命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_LASER_ENABLE` (`0x11`) | `0` | （无） | 激光开使能 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x11     | 0x00    | 0xFC |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x11+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 11 00 FC

---

#### 激光关使能

**简介**

对应的TLV：`TLV_LASER_DISABLE`（Type `0x12`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发激光关使能命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_LASER_DISABLE` (`0x12`) | `0` | （无） | 激光关使能 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x12     | 0x00    | 0xFD |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x12+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 12 00 FD

---

#### 开门（开门按钮边沿触发）

**简介**

对应的TLV：`TLV_DOOR_OPEN`（Type `0x08`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发开门命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_DOOR_OPEN` (`0x08`) | `0` | （无） | 开门 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x08     | 0x00    | 0xF3 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x08+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 08 00 F3

---

#### 关门（关门命令不再使用）

**简介**

对应的TLV：`TLV_DOOR_CLOSE`（Type `0x09`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发关门命令。与 `comm_task_app.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_DOOR_CLOSE` (`0x09`) | `0` | （无） | 关门 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x09     | 0x00    | 0xF4 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x09+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 09 00 F4

---

#### 内门开

**简介**

对应的TLV：`TLV_INNERDOOR_OPEN`（Type `0x0C`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发内门开命令，驱动内门电机执行开门动作。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_INNERDOOR_OPEN` (`0x0C`) | `0` | （无） | 内门开 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x0C     | 0x00    | 0xF7 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x0C+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 0C 00 F7

---

#### 内门关

**简介**

对应的TLV：`TLV_INNERDOOR_CLOSED`（Type `0x0D`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发内门关命令，驱动内门电机执行关门动作。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_INNERDOOR_CLOSED` (`0x0D`) | `0` | （无） | 内门关 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x0D     | 0x00    | 0xF8 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x0D+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 0D 00 F8

---

#### 内门停止运动

**简介**

对应的TLV：`TLV_INNERDOOR_STOP`（Type `0x0E`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发内门停止运动命令，停止内门电机当前的开/关动作。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_INNERDOOR_STOP` (`0x0E`) | `0` | （无） | 内门停止运动 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x0E     | 0x00    | 0xF9 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x0E+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 0E 00 F9

---

#### 基板加热开

**简介**

对应的TLV：`TLV_HOT_OPEN`（Type `0x0A`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发基板加热开启命令。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_HOT_OPEN` (`0x0A`) | `0` | （无） | 基板加热开 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x0A     | 0x00    | 0xF5 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x0A+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 0A 00 F5

---

#### 基板加热关

**简介**

对应的TLV：`TLV_HOT_CLOSE`（Type `0x0B`，Cmd `CMD_SENSOR_SET_PARA`）

SOC→MCU：通过 `PORT_SENSORS_TASK` 下发基板加热关闭命令。与 `def_tlv.h` 一致，该命令不携带 `Value`。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SENSOR_SET_PARA` (`0x9A`) | `TLV_HOT_CLOSE` (`0x0B`) | `0` | （无） | 基板加热关 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA5 | 0x9A | 0x01 | 0x02 | 0x0B     | 0x00    | 0xF6 |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA5+0x9A+0x01+0x02+0x0B+0x00)`。

**Copy**: 5A A5 AA A5 9A 01 02 0B 00 F6

---

#### 传感器数值上报（氧含量 / 舱压 / 温湿度 / PM2.5 / 粉料 / 基板温度）

**简介**

与 `def_cmd.h` 中 **`CMD_SNESOR_VALUE_REPORT`（`0x9B`）** 及 `def_tlv.h` 中 **`TLV_SENSOR_REPORT_1ST`（`0x02`）** 一致。典型为 **MCU→SOC** 主动上报（`FrameType = 0xBB`），经 **`PORT_SENSORS_TASK`（`0xA5`）** 路由至传感器管理任务侧语义；载荷为 **单条 TLV**。

**`TLV_SENSOR_REPORT_1ST` 载荷格式**

`Tlv-Len = 13`，`value[0..12]` 布局如下；多字节数值均为 **小端 `int16_t`**：

| 字节 | 含义 | 单位 / 说明 |
| :--- | :--- | :--- |
| `value[0..1]` | 氧含量 | **0.1%**（例如 `209` 表示 **20.9%**） |
| `value[2..3]` | 舱内压力 | **mbar**（整型，设备侧按约定换算后填入） |
| `value[4..5]` | 温度 | **0.1 °C** |
| `value[6..7]` | 湿度 | **0.1%** |
| `value[8..9]` | PM2.5 | **ug/m3** |
| `value[10]` | 接近开关 1 粉料状态 | `0` = 粉已空，`1` = 粉还有 |
| `value[11..12]` | 基板温度（PT100） | **0.1 °C**；无效或未就绪填 `0` |

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SNESOR_VALUE_REPORT` (`0x9B`) | `TLV_SENSOR_REPORT_1ST` (`0x02`) | `13` | 上表字段 | 聚合传感器快照 |

**命令示例（MCU→SOC，示例数值：氧 20.9%、气压 1013 mbar、SHT3x 温度 25.0 °C、湿度 50.0%、PM2.5 = 35 ug/m3、粉还有、基板温度 45.0 °C）**

小端编码：`209 → D1 00`，`1013 → F5 03`，`250 → FA 00`，`500 → F4 01`，`35 → 23 00`，`粉还有 → 01`，`450 → C2 01`；`Len = 15`（`0x0F`）= `Tlv-Type` + `Tlv-Len` + 13 字节 `Value`。

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..12]（hex） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA5 | 0x9B | 0x03 | 0x0F | 0x02     | 0x0D    | `D1 00 F5 03 FA 00 F4 01 23 00 01 C2 01` | 0xBA |

注：`Sum = (uint8_t)(从 SOF1 累加至 Value 末字节)`。

**Copy**: `5A A5 BB A5 9B 03 0F 02 0D D1 00 F5 03 FA 00 F4 01 23 00 01 C2 01 BA`

**说明**

- 头文件宏名为 **`CMD_SNESOR_VALUE_REPORT`**（拼写与源码一致）。
- 若同一 Cmd 下扩展其它 TLV Type，须以 **Port + Cmd + Tlv-Type** 联合解析。
- 当前 `sensors_manager_task_app.c` 中 Hall 读取与 `value[13..14]` 赋值代码处于注释状态，本 TLV 当前不携带 Hall 状态。

**`TLV_SENSOR_REPORT_2ST` 载荷格式**

`Tlv-Len = 8`，`value[0..7]` 布局如下；多字节数值均为 **小端 `uint16_t`**：

| 字节 | 含义 | 单位 / 说明 |
| :--- | :--- | :--- |
| `value[0..1]` | `laser_status` | 激光器状态 |
| `value[2..3]` | `laser_alarm` | 激光器报警打包值 |
| `value[4..5]` | `laser_power` | 激光器功率原始值 |
| `value[6..7]` | `pump_temperature` | 泵浦温度原始值 |

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SNESOR_VALUE_REPORT` (`0x9B`) | `TLV_SENSOR_REPORT_2ST` (`0x03`) | `8` | 上表字段 | 激光器传感器快照 |

---

#### 传感器任务运动位置上报（Z / S / D / Z 工作坐标）

**简介**

对应的 TLV：`TLV_MOTION_POS_REPORT`（Type `0x05`，Cmd `CMD_SNESOR_VALUE_REPORT`）

MCU SensorsTask 周期主动上报运动轴位置快照，典型为 **MCU→SOC** 主动上报（`FrameType = 0xBB`），经 **`PORT_SENSORS_TASK`（`0xA5`）** 输出。当前固件中该上报由 `Sensors_SendMotionPosReportToComm()` 组包，周期由 `SENSORS_TASK_MOTION_POS_REPORT_INTERVAL_MS` 控制，默认 **1500ms**。

> 注：本文后续 `CMD_MOTION_STATUS + TLV_MOTION_ACTUAL_POS` 是 MotionTask 的运动状态位置上报；本节是 SensorsTask 复用 `CMD_SNESOR_VALUE_REPORT` 的传感器侧位置上报，解析时需按 **Port + Cmd + Tlv-Type** 区分。

**命令组成（MCU→SOC）**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_SENSORS_TASK` (`0xA5`) | `CMD_SNESOR_VALUE_REPORT` (`0x9B`) | `TLV_MOTION_POS_REPORT` (`0x05`) | `16` | 见下表 | SensorsTask 周期上报运动轴位置 |

**Value 格式**

`Tlv-Len = 16`，`value[0..15]` 布局如下；多字节数值均为 **小端 `int32_t`**：

| 偏移 | 类型 | 含义 | 单位 / 说明 |
| ---- | ---- | ---- | ----------- |
| `0..3` | `int32 LE` | Z 轴实际位置 | `0.1um` |
| `4..7` | `int32 LE` | Z 轴工作坐标 | `0.1um` |
| `8..11` | `int32 LE` | S 轴实际位置 | `0.1mm` |
| `12..15` | `int32 LE` | D 轴实际位置 | `0.1deg` |

**解析示例**

```c
int32_t z_0p1um = le_i32(value + 0);
int32_t z_work_0p1um = le_i32(value + 4);
int32_t s_0p1mm = le_i32(value + 8);
int32_t d_0p1deg = le_i32(value + 12);

float z_mm = (float)z_0p1um / 10000.0f;
float z_work_mm = (float)z_work_0p1um / 10000.0f;
float s_mm = (float)s_0p1mm / 10.0f;
float d_deg = (float)d_0p1deg / 10.0f;
```

**命令示例（MCU→SOC，FrameType=`0xBB`）**

示例位置：

- Z = `12.3456 mm` → `123456`（0.1um）→ `0x0001E240` → 小端 `40 E2 01 00`
- Z 工作坐标 = `0.0000 mm` → `0`（0.1um）→ 小端 `00 00 00 00`
- S = `25.3 mm` → `253`（0.1mm）→ `0x000000FD` → 小端 `FD 00 00 00`
- D = `-12.3deg` → `-123`（0.1deg）→ `0xFFFFFF85` → 小端 `85 FF FF FF`

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..15] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------ | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA5 | 0x9B | 0x04 | 0x12 | 0x05     | 0x10    | `40 E2 01 00 00 00 00 00 FD 00 00 00 85 FF FF FF` | 0xC7 |

注：`Len=0x12` = `Tlv-Type` + `Tlv-Len` + 16 字节 Value。`Sum = (uint8_t)(从 SOF1 累加至 Value 末字节)`。

**Copy**: `5A A5 BB A5 9B 04 12 05 10 40 E2 01 00 00 00 00 00 FD 00 00 00 85 FF FF FF C7`

---

#### 粉桶开关（CMD_POWDER_SUPPLY）

**简介**

与 `def_cmd.h` / `def_tlv.h` 一致：SOC 通过 **`PORT_POWDER_TASK`（`0xA9`）** + **`CMD_POWDER_SUPPLY`（`0x8A`）** 下发粉桶通断；Payload 内 TLV 区分开/关及目标粉桶。

**Cmd 与 TLV**

| Tlv-Type | `#define` | 方向 | Tlv-Len | Value | 说明 |
| -------- | --------- | ---- | ------- | ----- | ---- |
| `0x01` | `TLV_POWDER_SWITCH_ON` | SOC→MCU | `1` | `value[0]`：`1` = 粉桶 1，`2` = 粉桶 2 | 打开指定粉桶 |
| `0x02` | `TLV_POWDER_SWITCH_OFF` | SOC→MCU | `1` | `value[0]`：同上 | 关闭指定粉桶 |

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_POWDER_TASK` (`0xA9`) | `CMD_POWDER_SUPPLY` (`0x8A`) | `TLV_POWDER_SWITCH_ON`（`0x01`）或 `TLV_POWDER_SWITCH_OFF`（`0x02`） | `1` | `uint8_t` 桶号：`1` / `2` | 开桶或关桶 |

**命令示例（SOC→MCU，打开粉桶 1）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | -------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA9 | 0x8A | 0x01 | 0x03 | 0x01     | 0x01    | 0x01     | 0xE3 |

注：`Len=3`；`Value[0]=0x01` 表示粉桶 1。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA9+0x8A+0x01+0x03+0x01+0x01+0x01)`。

**Copy**: `5A A5 AA A9 8A 01 03 01 01 01 E3`

**命令示例（SOC→MCU，关闭粉桶 2）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | -------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA9 | 0x8A | 0x02 | 0x03 | 0x02     | 0x01    | 0x02     | 0xE6 |

注：`Tlv-Type=0x02` 为 `TLV_POWDER_SWITCH_OFF`；`Value[0]=0x02` 表示粉桶 2。

**Copy**: `5A A5 AA A9 8A 02 03 02 01 02 E6`

---

#### 打印层参数设置

**简介**

对应的TLV：`TLV_PRINT_LAYER_PARAM_SET`（Type `0x01`，Cmd `CMD_PRINTING_LAYER_PARAM`）

SOC→MCU：通过 `PORT_PRINTING_TASK` 下发打印层参数。与 `def_tlv.h` 一致，`value[0..1]` 为层厚 `uint16_t LE`，单位 `um`；`value[2..3]` 为平衡系数 `uint16_t LE`，存储值需除以 `1000` 后得到实际系数，建议范围 `0~2000`（即 `0.000~2.000`）。

**注意**：当前层厚仍为固定值30，暂未使用层厚，上位机中层厚可填任意值，只使用了平衡系数来修改对应的层厚值。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_PRINTING_TASK` (`0xA4`) | `CMD_PRINTING_LAYER_PARAM` (`0xB5`) | `TLV_PRINT_LAYER_PARAM_SET` (`0x01`) | `4` | `layer_thickness_um` + `balance_x1000`，均为 `uint16_t LE` | 打印层参数设置 |

**命令示例（SOC→MCU，层厚 = 50 um，平衡系数 = 1.000）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..3] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ----------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0xB5 | 0x01 | 0x06 | 0x01     | 0x04    | `32 00 E8 03` | 0x2B |

注：`0x0032 = 50`（层厚 50 um），`0x03E8 = 1000`（平衡系数 1.000）。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0xB5+0x01+0x06+0x01+0x04+0x32+0x00+0xE8+0x03)`。

**Copy**: `5A A5 AA A4 B5 01 06 01 04 32 00 E8 03 2B`

---

#### 清粉测试开启

**简介**

**当前逻辑**：清粉测试 = 在 IDLE 下临时接管 Z 轴，开风机后做"先吹粉、再分 5mm 步进上升到记录高度、然后整体降回"的循环，重复 3 轮后停风机收尾；START 触发、CLOSE 中止（但不撤销在途运动）。

对应的TLV：`TLV_PRINT_CLEAN_TEST_START`（Type `0x20`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：通过 `PORT_PRINTING_TASK` 下发，启动清粉测试流程（Z 轴按 5mm 步长上升、暂停吹粉、循环数轮），该命令不携带 `Value`。

**注意**：该命令为调试/测试命令，经 UART4 调试口下发，`Cmd` 实际为 `UART4_CMD_FLAG`（`0x80`）`| CMD_PRINTING_CTRL`（`0x01`）`= 0x81`。仅在打印状态为 `IDLE` 且清粉流程未运行、Z 轴坐标可成功读取时才会启动。

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_PRINTING_TASK` (`0xA4`) | `CMD_PRINTING_CTRL` (`0x81`) | `TLV_PRINT_CLEAN_TEST_START` (`0x20`) | `0` | （无） | 启动清粉测试 |

**命令示例（SOC→MCU，启动清粉测试）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x81 | 0x00 | 0x02 | 0x20     | 0x00    | 0xF0 |

注：`Len=2 = Tlv-Type + Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x81+0x00+0x02+0x20+0x00)`。

**Copy**: `5A A5 AA A4 81 00 02 20 00 F0`

---

#### 清粉测试关闭（暂不使用）

**简介**

对应的TLV：`TLV_PRINT_CLEAN_TEST_CLOSE`（Type `0x21`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：通过 `PORT_PRINTING_TASK` 下发，关闭/中止清粉测试，该命令不携带 `Value`。

**注意**：该命令为调试/测试命令，经 UART4 调试口下发，`Cmd` 实际为 `UART4_CMD_FLAG`（`0x80`）`| CMD_PRINTING_CTRL`（`0x01`）`= 0x81`。当前实现仅停风机并清空清粉上下文，不会撤销已下发给 Motion / Z 轴的运动命令（关闭时正在执行的单步运动可能仍会完成）。**该命令暂不使用。**

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_PRINTING_TASK` (`0xA4`) | `CMD_PRINTING_CTRL` (`0x81`) | `TLV_PRINT_CLEAN_TEST_CLOSE` (`0x21`) | `0` | （无） | 关闭清粉测试（暂不使用） |

**命令示例（SOC→MCU，关闭清粉测试，暂不使用）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x81 | 0x00 | 0x02 | 0x21     | 0x00    | 0xF1 |

注：`Len=2 = Tlv-Type + Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x81+0x00+0x02+0x21+0x00)`。

**Copy**: `5A A5 AA A4 81 00 02 21 00 F1`

---

#### 激光器手动功率控制（暂不使用）

**简介**

对应的TLV：`TLV_LASER_POWER_SET`（Type `0x01`，Cmd `CMD_LASER_MANUAL_CONTROL`）与 `TLV_LASER_POWER_RESET`（Type `0x02`，Cmd `CMD_LASER_MANUAL_CONTROL`）

SOC→MCU：通过 `PORT_LASER_TASK` 下发激光器手动功率控制命令。`TLV_LASER_POWER_SET` 用于设置激光功率百分比，`TLV_LASER_POWER_RESET` 用于取消手动功率控制并恢复外部模拟量控制。与 `def_tlv.h` 一致，前者携带 1 字节功率百分比，后者不携带 `Value`。

**Cmd 与 TLV**

| Tlv-Type | `#define` | 方向 | Tlv-Len | Value | 说明 |
| -------- | --------- | ---- | ------- | ----- | ---- |
| `0x01` | `TLV_LASER_POWER_SET` | SOC→MCU | `1` | `value[0]`：功率百分比，`0`~`100` | 设置激光手动功率 |
| `0x02` | `TLV_LASER_POWER_RESET` | SOC→MCU | `0` | （无） | 取消手动功率控制，恢复外控 |

**命令组成**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_LASER_TASK` (`0xAA`) | `CMD_LASER_MANUAL_CONTROL` (`0x87`) | `TLV_LASER_POWER_SET` (`0x01`) | `1` | `uint8_t` 功率百分比，`0`~`100` | 激光手动功率设置 |
| `PORT_LASER_TASK` (`0xAA`) | `CMD_LASER_MANUAL_CONTROL` (`0x87`) | `TLV_LASER_POWER_RESET` (`0x02`) | `0` | （无） | 激光手动功率恢复外控 |

**命令示例（SOC→MCU，功率 = 80%）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | -------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xAA | 0x87 | 0x01 | 0x03 | 0x01     | 0x01    | 0x50     | 0x30 |

注：`Len=3` = `Tlv-Type` + `Tlv-Len` + `1` 字节 Value。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xAA+0x87+0x01+0x03+0x01+0x01+0x50)`。

**Copy**: `5A A5 AA AA 87 01 03 01 01 50 30`

**命令示例（SOC→MCU，恢复外控）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xAA | 0x87 | 0x01 | 0x02 | 0x02     | 0x00    | 0xDF |

注：`Len=2` = `Tlv-Type` + `Tlv-Len`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xAA+0x87+0x01+0x02+0x02+0x00)`。

**Copy**: `5A A5 AA AA 87 01 02 02 00 DF`

---

#### OTA 升级启动与数据流

**简介**

`PORT_OTA_TASK`（`0xA6`）当前采用“两阶段”方式：

1. **启动阶段**仍使用本文定义的完整串口帧，SOC 发送 `TLV_TYPE_START_OTA`（`0xD9`）启动 OTA。
2. MCU 先回复一帧 `FrameType = 0xBA` 的 ACK；该 ACK 的 `Port / Cmd / Seq` 与请求一致，Payload 回传 `TLV_TYPE_START_OTA + 当前运行分区号`。
3. ACK 完成后，MCU 会在**同一 UART** 上发送原始单字节 `X_C`（`0x43`），通知 SOC 后续按 **XMODEM(CRC16)** 发送固件。
4. **数据阶段**不再使用 `SOF1 + SOF2 + FrameType + ...` 封装，而是直接传输 XMODEM 原始字节流。

**说明（当前固件实现）**

- 当前 `ota_task_app.c` 并未单独定义 `CMD_OTA_*`；启动 OTA 时，固件实际依赖 `Port = PORT_OTA_TASK` 与 `TLV_TYPE_START_OTA` 进入 OTA 流程。
- 因此本文示例中 OTA 启动帧的 `Cmd` 固定写为 `0x00` 作为**当前保留值**；ACK 仍需保持与请求相同的 `Cmd / Seq`。

**命令组成（启动阶段）**

| Port            | Cmd                     | Tlv-Type             | Tlv-Len | Value  | 说明 |
| ---------------- | ----------------------- | -------------------- | ------- | ------ | ---- |
| `PORT_OTA_TASK` | `0x00`（当前保留）      | `TLV_TYPE_START_OTA` | `0`     | （无） | 启动 OTA，随后切换至 XMODEM 数据流 |

**命令示例（SOC→MCU，启动 OTA）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA6 | 0x00 | 0x01 | 0x02 | 0xD9     | 0x00    | 0x2B |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA6+0x00+0x01+0x02+0xD9+0x00)`。

**Copy**: 5A A5 AA A6 00 01 02 D9 00 2B

**ACK 示例（MCU→SOC，分区信息由 `Get_Run_Slot_Num()` 给出）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | -------- | ---- |
| 0x5A | 0xA5 | 0xBA      | 0xA6 | 0x00 | 0x01 | 0x03 | 0xD9     | 0x01    | 0x01     | 0x3E |
| 0x5A | 0xA5 | 0xBA      | 0xA6 | 0x00 | 0x01 | 0x03 | 0xD9     | 0x01    | 0x02     | 0x3F |

注：

- ACK 的 `Port / Cmd / Seq` 与启动请求保持一致。
- `Value[0]` 为 MCU 当前运行分区号（例如 `0x01` 表示当前运行在 APP1，`0x02` 表示当前运行在 APP2）；SOC 可据此决定后续镜像/升级策略。
- 第一行 `Sum = (uint8_t)(0x5A+0xA5+0xBA+0xA6+0x00+0x01+0x03+0xD9+0x01+0x01)`；第二行仅 `Value[0]` 改为 `0x02`，故 `Sum` 加 `1`。

**Copy**:

- 5A A5 BA A6 00 01 03 D9 01 01 3E
- 5A A5 BA A6 00 01 03 D9 01 02 3F

**数据流阶段（XMODEM 原始字节，不带 SOF1/SOF2）**

| 阶段 | 方向 | 字节形式 | 说明 |
| ---- | ---- | -------- | ---- |
| 1 | MCU→SOC | `0x43`（`X_C`） | 表示后续采用 **XMODEM + CRC16**；SOC 收到后开始发送固件数据 |
| 2 | SOC→MCU | `0x01 + pkt_no + ~pkt_no + 128B Data + CRC16` | `X_SOH` 数据包；这些字节是 **XMODEM 原始包**，**不**再套 `SOF1/SOF2` 业务帧 |
| 3 | MCU→SOC | `0x06`（`X_ACK`） | 当前包接收成功，SOC 继续发送下一包 |
| 4 | MCU→SOC | `0x15`（`X_NAK`） | 当前包 CRC / 包号 / 超时异常，请求 SOC 重发当前包 |
| 5 | MCU→SOC | `0x18 0x18 0x18`（`X_CAN` 三次） | 错误累计超限，MCU 终止本次 OTA 传输 |
| 6 | SOC→MCU | `0x04`（`X_EOT`） | 固件文件发送结束 |
| 7 | MCU→SOC | `0x06`（`X_ACK`） | MCU 对 `X_EOT` 的确认；随后固件会执行参数写入/重启等收尾动作 |

**说明（当前对接建议）**

- 当前 `interBoard_comm_task_app.c` 对 OTA 原始数据的收包分流，按首字节 `X_SOH`（`0x01`）/`X_EOT`（`0x04`）识别。
- 当前 `xQueue_OtaBuf` 的消息尺寸也按 `XMODEM-128`（`SOH`，128B 数据）配置。
- 因此，**当前版本建议 SOC 按 `XMODEM-128` 模式对接**；若后续希望改为 1K 包（`X_STX`）模式，需同步确认接收分流与缓冲配置。

---

#### 上电自检结果上报

**简介**

对应的TLV：`TLV_POST_RESULT_TYPE`（Type `0x01`，Cmd `CMD_POST_RESULT`）

MCU 在 POST 结束后向 SOC 上报自检结果；载荷为 POST 结果字（见 `General_SendPostResultToComm`）。

**命令组成**

| Port                | Cmd              | Tlv-Type               | Tlv-Len | Value                          | 说明 |
| ------------------- | ---------------- | ---------------------- | ------- | ------------------------------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_POST_RESULT` | `TLV_POST_RESULT_TYPE` | `4`     | `uint32_t` 小端，位域见业务定义 | 与 `sizeof(post_result)` 一致 |

**命令示例（MCU→SOC，`post_result = 0`，FrameType=`0xBB`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..3]   | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA2 | 0xA1 | 0x01 | 0x06 | 0x01     | 0x04    | `00 00 00 00` | 0x09 |

注：`Len=6` = `Tlv-Type`+`Tlv-Len`+4 字节 Value。`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA2+0xA1+0x01+0x06+0x01+0x04+0x00+0x00+0x00+0x00)`。

**Copy**: 5A A5 BB A2 A1 01 06 01 04 00 00 00 00 09

---

#### 设备唤醒（激活）

**简介**

对应的TLV：`TLV_MACHINE_WAKE_ACTIVE`（Type `0x01`，Cmd `CMD_MACHINE_WAKE`）

SOC→MCU：请求唤醒激活；General 任务置 `CR_MACHINE_WAKE_MASK`（见 `vGeneralTask`）。

**命令组成**

| Port                | Cmd                | Tlv-Type                  | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | ------------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MACHINE_WAKE` | `TLV_MACHINE_WAKE_ACTIVE` | `0`     | （无） | 仅 Type，无 Value |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xAA | 0x01 | 0x02 | 0x01     | 0x00    | 0xF9 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xAA+0x01+0x02+0x01+0x00)`。（注意：Type `0x01` 在 `CMD_POST_RESULT` 下表示「自检结果」载荷，勿与本条混淆。）

**Copy**: 5A A5 AA A2 AA 01 02 01 00 F9

---

#### 设备唤醒（未激活 / 休眠侧）

**简介**

对应的TLV：`TLV_MACHINE_WAKE_INACTIVE`（Type `0x02`，Cmd `CMD_MACHINE_WAKE`）

SOC→MCU：休眠或「唤醒未激活」侧语义；General 清除 `CR_MACHINE_WAKE_MASK`。

**命令组成**

| Port                | Cmd                | Tlv-Type                    | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | --------------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MACHINE_WAKE` | `TLV_MACHINE_WAKE_INACTIVE` | `0`     | （无） | 仅 Type |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xAA | 0x01 | 0x02 | 0x02     | 0x00    | 0xFA |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xAA+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 AA A2 AA 01 02 02 00 FA

---

#### 设备睡眠时间参数

**简介**

对应的TLV：`TLV_MACHINE_WAKE_PARAMETER`（Type `0x03`，Cmd `CMD_MACHINE_WAKE`）

设定睡眠时间，单位为分钟（头文件注释默认 10 分钟）。

**命令组成**

| Port                | Cmd                | Tlv-Type                     | Tlv-Len | Value           | 说明 |
| ------------------- | ------------------ | ---------------------------- | ------- | --------------- | ---- |
| `PORT_GENERAL_TASK` | `CMD_MACHINE_WAKE` | `TLV_MACHINE_WAKE_PARAMETER` | `1`     | `uint8_t` 分钟数 | 睡眠时间 |

**命令示例（10 分钟，SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | -------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xAA | 0x01 | 0x03 | 0x03     | 0x01    | 0x0A     | 0x06 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xAA+0x01+0x03+0x03+0x01+0x0A)`。

**Copy**: 5A A5 AA A2 AA 01 03 03 01 0A 06

---

#### 电机使能（请求）

**简介**

对应的TLV：`TLV_MOTOR_ENABLE`（Type `0x01`，Cmd `CMD_MOTOR_ENABLE`）

SOC→MCU：请求电机使能；General 置 `PEND_MOTOR_ENABLE_MASK`。

**命令组成**

| Port                | Cmd                | Tlv-Type           | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | ------------------ | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTOR_ENABLE` | `TLV_MOTOR_ENABLE` | `0`     | （无） | 请求 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB0 | 0x01 | 0x02 | 0x01     | 0x00    | 0xFF |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB0+0x01+0x02+0x01+0x00)`。

**Copy**: 5A A5 AA A2 B0 01 02 01 00 FF

---

#### 电机失能（请求）

**简介**

对应的TLV：`TLV_MOTOR_DISENABLE`（Type `0x02`，Cmd `CMD_MOTOR_ENABLE`）

SOC→MCU：请求电机失能（与「电机使能（请求）」共用 Cmd，由 Tlv-Type 区分）。

**命令组成**

| Port                | Cmd                | Tlv-Type              | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | --------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTOR_ENABLE` | `TLV_MOTOR_DISENABLE` | `0`     | （无） | 请求 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB0 | 0x01 | 0x02 | 0x02     | 0x00    | 0x00 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB0+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 AA A2 B0 01 02 02 00 00

---

#### 电机已使能（结果上报）

**简介**

对应的TLV：`TLV_MOTOR_ENABLED`（Type `0x03`，Cmd `CMD_MOTOR_ENABLE`）

MCU 侧（如 Motion→General）上报：电机已使能成功。

**命令组成**

| Port                | Cmd                | Tlv-Type            | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | ------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTOR_ENABLE` | `TLV_MOTOR_ENABLED` | `0`     | （无） | 结果 |

**命令示例（MCU→SOC，FrameType=`0xBB`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA2 | 0xB0 | 0x01 | 0x02 | 0x03     | 0x00    | 0x12 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA2+0xB0+0x01+0x02+0x03+0x00)`。

**Copy**: 5A A5 BB A2 B0 01 02 03 00 12

---

#### 电机已失能（结果上报）

**简介**

对应的TLV：`TLV_MOTOR_DISENABLED`（Type `0x04`，Cmd `CMD_MOTOR_ENABLE`）

MCU 侧上报：电机已失能。

**命令组成**

| Port                | Cmd                | Tlv-Type               | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | ---------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTOR_ENABLE` | `TLV_MOTOR_DISENABLED` | `0`     | （无） | 结果 |

**命令示例（MCU→SOC）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA2 | 0xB0 | 0x01 | 0x02 | 0x04     | 0x00    | 0x13 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA2+0xB0+0x01+0x02+0x04+0x00)`。

**Copy**: 5A A5 BB A2 B0 01 02 04 00 13

---

#### 电机使能失败（结果上报）

**简介**

对应的TLV：`TLV_MOTOR_ENABLE_FAILED`（Type `0x05`，Cmd `CMD_MOTOR_ENABLE`）

MCU 侧上报：使能失败。

**命令组成**

| Port                | Cmd                | Tlv-Type                  | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------ | ------------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTOR_ENABLE` | `TLV_MOTOR_ENABLE_FAILED` | `0`     | （无） | 结果 |

**命令示例（MCU→SOC）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA2 | 0xB0 | 0x01 | 0x02 | 0x05     | 0x00    | 0x14 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA2+0xB0+0x01+0x02+0x05+0x00)`。

**Copy**: 5A A5 BB A2 B0 01 02 05 00 14

---

#### 运动状态（活跃）

**简介**

对应的TLV：`TLV_MOTION_ACTIVE`（Type `0x01`，Cmd `CMD_MOTION_STATUS`）

运动活跃状态（SOC↔MCU 约定；General 中按 `MotionTaskHandle` 来源处理）。

**命令组成**

| Port                | Cmd                 | Tlv-Type            | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------- | ------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_STATUS` | `TLV_MOTION_ACTIVE` | `0`     | （无） | 可扩展附加字节 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB1 | 0x01 | 0x02 | 0x01     | 0x00    | 0x00 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB1+0x01+0x02+0x01+0x00)`。（与「设备唤醒（激活）」同 Type 数值 `0x01`，**Cmd 不同**。）

**Copy**: 5A A5 AA A2 B1 01 02 01 00 00

---

#### 运动状态（非活跃）

**简介**

对应的TLV：`TLV_MOTION_INACTIVE`（Type `0x02`，Cmd `CMD_MOTION_STATUS`）

运动非活跃状态。

**命令组成**

| Port                | Cmd                 | Tlv-Type              | Tlv-Len | Value  | 说明 |
| ------------------- | ------------------- | --------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_STATUS` | `TLV_MOTION_INACTIVE` | `0`     | （无） | 可扩展 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB1 | 0x01 | 0x02 | 0x02     | 0x00    | 0x01 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB1+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 AA A2 B1 01 02 02 00 01

---

#### 运动轴实际位置上报

**简介**

对应的TLV：`TLV_MOTION_ACTUAL_POS`（Type `0x03`，Cmd `CMD_MOTION_STATUS`）

MCU MotionTask 每 **500ms** 主动上报一次 Z / S / D 三轴实际位置，并额外上报 **Z 轴工作坐标**。其中 Z 工作坐标由 MCU 内部按 `Z实际位置 + Z工作零点补偿` 直接计算后发送，SOC 无需再额外做零点偏移计算。

> 注：本节为 MotionTask 的 `CMD_MOTION_STATUS + TLV_MOTION_ACTUAL_POS` 上报；SensorsTask 侧另有 `CMD_SNESOR_VALUE_REPORT + TLV_MOTION_POS_REPORT`，见前文“传感器任务运动位置上报”。

**命令组成（MCU→SOC）**

| Port | Cmd | Tlv-Type | Tlv-Len | Value | 说明 |
| ---- | --- | -------- | ------- | ----- | ---- |
| `PORT_MOTION_TASK` (`0xA7`) | `CMD_MOTION_STATUS` (`0xB1`) | `TLV_MOTION_ACTUAL_POS` (`0x03`) | `16` | 见下表 | MotionTask 周期上报 |

**Value 格式**

| 偏移 | 类型 | 含义 | 单位 | 换算关系 |
| ---- | ---- | ---- | ---- | -------- |
| `0..3` | `int32 LE` | Z 轴实际位置 | `0.1um` | `Z_mm * 10000` |
| `4..7` | `int32 LE` | S 轴实际位置 | `0.1mm` | `S_mm * 10` |
| `8..11` | `int32 LE` | D 轴实际位置 | `0.1°` | `D_deg * 10` |
| `12..15` | `int32 LE` | Z 轴工作坐标 | `0.1um` | `(Z_mm + Z_work_zero_offset_mm) * 10000` |

**解析示例**

```c
int32_t z_0p1um  = le_i32(value + 0);
int32_t s_0p1mm  = le_i32(value + 4);
int32_t d_0p1deg = le_i32(value + 8);
int32_t z_work_0p1um = le_i32(value + 12);

float z_mm  = (float)z_0p1um / 10000.0f;
float s_mm  = (float)s_0p1mm / 10.0f;
float d_deg = (float)d_0p1deg / 10.0f;
float z_work_mm = (float)z_work_0p1um / 10000.0f;
```

**命令示例（MCU→SOC，FrameType=`0xBB`）**

示例位置：

- Z = `12.3456 mm` → `123456`（0.1um）→ `0x0001E240` → 小端 `40 E2 01 00`
- S = `25.3 mm` → `253`（0.1mm）→ `0x000000FD` → 小端 `FD 00 00 00`
- D = `-12.3°` → `-123`（0.1°）→ `0xFFFFFF85` → 小端 `85 FF FF FF`
- Z 工作坐标 = `0.0000 mm` → `0`（0.1um）→ `0x00000000` → 小端 `00 00 00 00`

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..15] | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------ | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA7 | 0xB1 | 0x01 | 0x12 | 0x03     | 0x10    | `40 E2 01 00 FD 00 00 00 85 FF FF FF 00 00 00 00` | 0xDA |

注：`Len=0x12` = `Tlv-Type` + `Tlv-Len` + 16 字节 Value。`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA7+0xB1+0x01+0x12+0x03+0x10+40+E2+01+00+FD+00+00+00+85+FF+FF+FF+00+00+00+00)`。

**Copy**: 5A A5 BB A7 B1 01 12 03 10 40 E2 01 00 FD 00 00 00 85 FF FF FF 00 00 00 00 DA

---

#### Z 轴运动（`CMD_MOTION_Z_MOVE`）总览

**MCU 路径（命令字与队列控制块定义见 `Core/Inc/comm_task_app.h`）：** Z 轴执行逻辑在 **`z_motor_task_app.c`**；SOC 经 **`PORT_GENERAL_TASK`（`0xA2`）** 入站后，由 **`general_task_app.c`** 将 **`AppComm_CtrlBlock_t`** 投递 **`xQueue_TaskToMotion`**，**`motion_task_app.c`** 解析 **`CMD_MOTION_Z_MOVE`（`0xB2`）** 并**转发至 Zmotor 任务**（板内队列以 `freertos.c`、`comm_task_app.h` 中 **`xQueue_TaskToZMotor`** 等为准）。

**`TLV_MOTOR_Z_LAYER`（`0x05`）/ `TLV_MOTOR_Z_LAYER_OK`（`0x06`）：** 仍为 **板内任务间**（PrintingProc → Motion → Zmotor 等），**不**作为本文 SOC 串口常规载荷；**Printing→Motion** 段层高载荷见《MCU板内任务通信协议》**§6.2.3**（**`int16` LE μm，`Tlv-Len = 2`**）；**Motion→Z** 的板内位置/轮廓载荷格式见同文档 **§6.3.2 / §6.3.3**，本文不展开。

---

#### Z 轴点动（Jog）

**简介**

对应的TLV：`TLV_MOTOR_Z_JOG`（Type `0x01`，Cmd `CMD_MOTION_Z_MOVE`）

Z 轴点动，**轮廓速度模式**；`Value` 为 **`int16_t` 小端**，单位 **mm/s**（与头文件一致）。**`Tlv-Len = 0`** 时由固件采用缺省点动速度（可与 S 轴对齐为 **10 mm/s**，以 **`motion_task` → `zmotor`** 实现为准）；**`Tlv-Len = 2`** 时附目标速度。

**路由：** SOC 帧 **Port = `0xA2`（`PORT_GENERAL_TASK`）** → **`interBoard_comm_task_app`** 入队 **`xQueue_TaskToGeneral`** → **`general_task_app`** → **`motion_task_app` → `z_motor_task_app`**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type          | Tlv-Len | Value                         | 说明 |
| -------------------- | ------------------- | ----------------- | ------- | ----------------------------- | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_Z_MOVE` | `TLV_MOTOR_Z_JOG` | `0` 或 `2` | 无；或 **`int16_t` 小端**，mm/s | 与 `comm_task_app.h` 一致 |

**命令示例（SOC→MCU，无速度载荷，缺省速度由固件决定，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x02 | 0x01     | 0x00    | 0x01 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x02+0x01+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 02 01 00 01

**命令示例（速度 = 100 mm/s，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x04 | 0x01     | 0x02    | `64 00`             | 0x69 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x04+0x01+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 04 01 02 64 00 69

---

#### Z 轴增量移动（相对位置）

**简介**

对应的TLV：`TLV_MOTOR_Z_INC`（Type `0x02`，Cmd `CMD_MOTION_Z_MOVE`）

Z 轴**相对位置**轮廓运动；`Value` 为 **`int16_t` 小端**，单位 **mm**（有符号整数）。**`Tlv-Len = 2`**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type           | Tlv-Len | Value                    | 说明 |
| -------------------- | ------------------- | ------------------ | ------- | ------------------------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_Z_MOVE` | `TLV_MOTOR_Z_INC` | `2` | **`int16_t` 小端**，mm | 相对位移 |

**命令示例（增量 = 100 mm，`int16`，SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x04 | 0x02     | 0x02    | `64 00`             | 0x6A |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x04+0x02+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 04 02 02 64 00 6A

---

#### Z 轴点动停止

**简介**

对应的TLV：`TLV_MOTOR_Z_JOG_STOP`（Type `0x03`，Cmd `CMD_MOTION_Z_MOVE`）

Z 轴点动（Jog）停止。

**命令组成**

| Port                 | Cmd                 | Tlv-Type               | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ---------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_Z_MOVE` | `TLV_MOTOR_Z_JOG_STOP` | `0`     | （无） | 停止 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x02 | 0x03     | 0x00    | 0x03 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x02+0x03+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 02 03 00 03

---

#### Z 轴绝对位置移动

**简介**

对应的TLV：`TLV_MOTOR_Z_MOVE`（Type `0x04`，Cmd `CMD_MOTION_Z_MOVE`）

Z 轴**绝对位置**轮廓运动；`Value` 为 **`int16_t` 小端**，单位 **mm**（有符号整数）。**`Tlv-Len = 2`**。MCU 在 Motion 内会将 mm 换算后再转发至 ZMotor；板内转发格式见《MCU板内任务通信协议》**§6.3.2**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type            | Tlv-Len | Value                  | 说明 |
| -------------------- | ------------------- | ------------------- | ------- | ---------------------- | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_Z_MOVE` | `TLV_MOTOR_Z_MOVE` | `2` | **`int16_t` 小端**，mm | 目标位置 |

**命令示例（目标位置 = 100 mm，`int16`，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x04 | 0x04     | 0x02    | `64 00`             | 0x6C |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x04+0x04+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 04 04 02 64 00 6C

---

#### Z 轴回 Home

**简介**

对应的TLV：`TLV_MOTOR_Z_HOME`（Type `0x11`，Cmd `CMD_MOTION_Z_MOVE`）

Z 轴回 Home。该命令**不带 Value**，MotionTask 内部将其转换为 Z 轴绝对位置运动：目标位置为固件配置的 `MOTION_Z_HOME_POS_MM`，随后通过板内队列向 ZMotorTask 下发 `TLV_MOTOR_ABS_POS_MODE + 16B`。

**命令组成**

| Port                 | Cmd                 | Tlv-Type                 | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ------------------------ | ------- | ------ | ---- |
| `PORT_GENERAL_TASK`  | `CMD_MOTION_Z_MOVE` | `TLV_MOTOR_Z_HOME`  | `0`     | （无） | Motion 内部转换为 Z 绝对运动，不进入 Homing 状态 |

**命令示例（SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x02 | 0x11     | 0x00    | 0x11 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x02+0x11+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 02 11 00 11

---

#### Z 轴设置工作零点

**简介**

对应的TLV：`TLV_MOTOR_Z_SET_ZERO`（Type `0x12`，Cmd `CMD_MOTION_Z_MOVE`）

Z 轴设置工作零点。该命令**不带 Value**，下发到 MotionTask 后不会触发 Z 轴运动，而是将**当前 Z 轴实际位置的相反数**保存为内部工作零点补偿。后续 `TLV_MOTION_ACTUAL_POS` 周期上报中的 `value[12..15]` 会直接返回按该补偿计算后的 **Z 轴工作坐标**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type                    | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | --------------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK`  | `CMD_MOTION_Z_MOVE` | `TLV_MOTOR_Z_SET_ZERO`      | `0`     | （无） | 将当前位置清零，仅更新工作坐标补偿 |

**命令示例（SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB2 | 0x01 | 0x02 | 0x12     | 0x00    | 0x12 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB2+0x01+0x02+0x12+0x00)`。

**Copy**: 5A A5 AA A2 B2 01 02 12 00 12

---

#### S 轴点动（Jog）

**简介**

对应的TLV：`TLV_MOTOR_S_JOG`（Type `0x01`，Cmd `CMD_MOTION_S_MOVE`）

与 `comm_task_app.h` 注释及 `motion_task_app.c` 中 **`Motion_UnpackFromQueue`**（`sender == GeneralTaskHandle`）一致：S 轴点动走**轮廓速度模式**；`Value` 为 **`int16_t` 小端**，单位 **mm/s**（`Tlv-Len = 2`）。**`Tlv-Len < 2`**（含最简 `Tlv-Len = 0`）时，固件将目标速度按 **10.0 mm/s** 缺省值处理。

**路由：** SOC→**`PORT_GENERAL_TASK`（`0xA2`）**→General→**`motion_task`**；帧内 **`Cmd` = `CMD_MOTION_S_MOVE`（`0xB3`）**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type          | Tlv-Len | Value                         | 说明 |
| -------------------- | ------------------- | ----------------- | ------- | ----------------------------- | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_S_MOVE` | `TLV_MOTOR_S_JOG` | `0` 或 `2` | 无；或 **`int16_t` 小端**，mm/s | 与头文件、源码一致 |

**命令示例（最简：缺省 10 mm/s，SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB3 | 0x01 | 0x02 | 0x01     | 0x00    | 0x02 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB3+0x01+0x02+0x01+0x00)`。

**Copy**: 5A A5 AA A2 B3 01 02 01 00 02

**命令示例（速度 = 100 mm/s，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB3 | 0x01 | 0x04 | 0x01     | 0x02    | `64 00`             | 0x6A |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB3+0x01+0x04+0x01+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B3 01 04 01 02 64 00 6A

---

#### S 轴位移 / 目标位置（增量）

**简介**

对应的TLV：`TLV_MOTOR_S_INC`（Type `0x02`，Cmd `CMD_MOTION_S_MOVE`）

与 `comm_task_app.h` 及 **`Motion_UnpackFromQueue`**（General 转发）一致：S 轴**增量**、**相对位置模式**；`Value` 为 **`int16_t` 小端**，单位 **mm**，**`Tlv-Len` 须为 `2`**。仅当 **`tlv.len >= 2`** 时，Motion 才调用 **`motor_arm_profile_position_params`** 并置 **`MOTOR_REQ_REL_POS_MSK`**；否则**不**登记相对位移请求。

**路由：** **`PORT_GENERAL_TASK`（`0xA2`）**（与 S 点动相同）。

**命令组成**

| Port                 | Cmd                 | Tlv-Type           | Tlv-Len | Value              | 说明 |
| -------------------- | ------------------- | ------------------ | ------- | ------------------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_S_MOVE` | `TLV_MOTOR_S_INC` | `2`     | `int16_t` 小端，mm | 必须与 len≥2 配套 |

**命令示例（增量 = 100 mm，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB3 | 0x01 | 0x04 | 0x02     | 0x02    | `64 00`             | 0x6B |

注：`Len=4` = `Tlv-Type`+`Tlv-Len`+2 字节 Value。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB3+0x01+0x04+0x02+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B3 01 04 02 02 64 00 6B

---

#### S 轴点动停止

**简介**

对应的TLV：`TLV_MOTOR_S_JOG_STOP`（Type `0x03`，Cmd `CMD_MOTION_S_MOVE`）

S 轴点动停止；无 `Value`。解析见 **`Motion_UnpackFromQueue`**（General 转发）。

**路由：** **`PORT_GENERAL_TASK`（`0xA2`）**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type               | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ---------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_S_MOVE` | `TLV_MOTOR_S_JOG_STOP` | `0`     | （无） | 停止 |

**命令示例（SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB3 | 0x01 | 0x02 | 0x03     | 0x00    | 0x04 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB3+0x01+0x02+0x03+0x00)`。

**Copy**: 5A A5 AA A2 B3 01 02 03 00 04

---

#### S 轴绝对位置移动（临时测试）

**简介**

> **说明：本 TLV 为临时测试用途，正式产品协议与语义待定；联调验证后可能调整、合并或删除，请勿作为长期稳定接口依赖。**

对应的TLV：`TLV_MOTOR_S_MOVE`（Type `0x04`，Cmd `CMD_MOTION_S_MOVE`）

SOC→MCU：刮刀轴（S 轴）按**绝对位置**运动。`comm_task_app.h` 与 **`motion_task_app.c`（`Motion_UnpackFromQueue`，`sender == GeneralTaskHandle`）** 一致：`Value` 为 **`int16_t` 小端**，单位 **mm**，**`Tlv-Len = 2`** 且 **`tlv.len >= 2`** 时 **`motor_arm_profile_position_params`** + **`MOTOR_REQ_ABS_POS_MSK`**。与点动/增量/点动停的区分以 **Tlv-Type** 为准。

**命令组成**

| Port                 | Cmd                 | Tlv-Type            | Tlv-Len | Value                 | 说明 |
| -------------------- | ------------------- | ------------------- | ------- | --------------------- | ---- |
| `PORT_GENERAL_TASK`  | `CMD_MOTION_S_MOVE` | `TLV_MOTOR_S_MOVE`  | `2`     | `int16_t` 小端，mm    | 临时测试 |

**命令示例（目标 = 100 mm，SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB3 | 0x01 | 0x04 | 0x04     | 0x02    | `64 00`             | 0x6D |

注：`Len=4`。`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB3+0x01+0x04+0x04+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B3 01 04 04 02 64 00 6D

---

#### S 轴回零（Home）

**简介**

对应的TLV：`TLV_MOTOR_S_HOME`（Type `0x10`，Cmd `CMD_MOTION_S_MOVE`）

SOC→MCU：触发刮刀轴（S 轴）回零。该命令**不携带速度、加速度、减速度载荷**，运行参数由 MCU 固件内部宏配置提供。当前实现链路如下：

- `GeneralTask` 仅校验并转发该 TLV
- `MotionTask` 将其登记为独立 `HOME` 请求
- 执行时走 CiA402 `Homing mode`，即 `0x6060 = 6`
- MCU 内部按如下顺序下发：
  1. `MasterDS402_SetMode(..., 6)`
  2. `MasterDS402_SendControlword(..., 0x000F)`，用于清除 `6040.bit4`
  3. `MasterDS402_SetProfileAccDec(...)`
  4. `MasterDS402_SetTargetVelocity(...)`
  5. `MasterDS402_SendControlword(..., 0x001F)`，触发回零
- 驱动回零完成后，置位 `bit10(Target reached)`，MCU 据此收口状态机

**路由：** **`PORT_GENERAL_TASK`（`0xA2`）**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type            | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK`  | `CMD_MOTION_S_MOVE` | `TLV_MOTOR_S_HOME`  | `0`     | （无） | 回零参数由 MCU 内部宏定义提供 |

**命令示例（SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB3 | 0x01 | 0x02 | 0x10     | 0x00    | 0x11 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB3+0x01+0x02+0x10+0x00)`。

**Copy**: 5A A5 AA A2 B3 01 02 10 00 11

---

#### D 轴点动（Jog）

**简介**

对应的TLV：`TLV_MOTOR_D_JOG`（Type `0x01`，Cmd `CMD_MOTION_D_MOVE`）

与 `comm_task_app.h` 及 **`motion_task_app.c` → `Motion_UnpackFromQueue`**（General 转发）一致：D 轴（`MOTION_D_AXIS_INDEX` / `node_id=3`）点动为**轮廓速度模式**；`Value` 为 **`int16_t` 小端**，单位 **度/s**（语义以 **角速度 °/s** 为准）。**`Tlv-Len < 2`** 时固件使用 **10.0 °/s** 缺省值。

**路由：** SOC→**`PORT_GENERAL_TASK`（`0xA2`）**→General→**`motion_task`**；帧内 **`Cmd` = `CMD_MOTION_D_MOVE`（`0xB4`）**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type          | Tlv-Len | Value                         | 说明 |
| -------------------- | ------------------- | ----------------- | ------- | ----------------------------- | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_D_MOVE` | `TLV_MOTOR_D_JOG` | `0` 或 `2` | 无；或 **`int16_t` 小端**，°/s | 与头文件、源码一致 |

**命令示例（最简：缺省 10 °/s，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB4 | 0x01 | 0x02 | 0x01     | 0x00    | 0x03 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB4+0x01+0x02+0x01+0x00)`。

**Copy**: 5A A5 AA A2 B4 01 02 01 00 03

**命令示例（角速度 = 100 °/s，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB4 | 0x01 | 0x04 | 0x01     | 0x02    | `64 00`             | 0x6B |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB4+0x01+0x04+0x01+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B4 01 04 01 02 64 00 6B

---

#### D 轴位移 / 目标位置（增量）

**简介**

对应的TLV：`TLV_MOTOR_D_INC`（Type `0x02`，Cmd `CMD_MOTION_D_MOVE`）

与 `comm_task_app.h` 及 **`Motion_UnpackFromQueue`**（General 转发）一致：D 轴**相对位置**增量；`Value` 为 **`int16_t` 小端**，单位 **度**，**`Tlv-Len` 须为 `2`**。仅 **`tlv.len >= 2`** 时执行 **`motor_arm_profile_position_params`** 并置 **`MOTOR_REQ_REL_POS_MSK`**；否则不登记相对位移。

**路由：** **`PORT_GENERAL_TASK`（`0xA2`）**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type           | Tlv-Len | Value              | 说明 |
| -------------------- | ------------------- | ------------------ | ------- | ------------------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_D_MOVE` | `TLV_MOTOR_D_INC` | `2`     | `int16_t` 小端，度 | 须 len≥2 |

**命令示例（增量 = 100°，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB4 | 0x01 | 0x04 | 0x02     | 0x02    | `64 00`             | 0x6C |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB4+0x01+0x04+0x02+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B4 01 04 02 02 64 00 6C

---

#### D 轴绝对位置移动

**简介**

对应的TLV：`TLV_MOTOR_D_MOVE`（Type `0x04`，Cmd `CMD_MOTION_D_MOVE`）

与 `comm_task_app.h` 及 **`motion_task_app.c`（`Motion_UnpackFromQueue`，`sender == GeneralTaskHandle`）** 一致：D 轴**绝对位置**；`Value` 为 **`int16_t` 小端**，单位 **度**，**`tlv.len >= 2`** 时 **`motor_arm_profile_position_params`** 并入队 **`MOTOR_REQ_ABS_POS_MSK`**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type           | Tlv-Len | Value              | 说明 |
| -------------------- | ------------------- | ------------------ | ------- | ------------------ | ---- |
| `PORT_GENERAL_TASK`  | `CMD_MOTION_D_MOVE` | `TLV_MOTOR_D_MOVE` | `2`     | `int16_t` 小端，度 | 与头文件、源码一致 |

**命令示例（目标 = 100°，SOC→MCU，`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value[0..1]（小端） | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------------------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB4 | 0x01 | 0x04 | 0x04     | 0x02    | `64 00`             | 0x6E |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB4+0x01+0x04+0x04+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A2 B4 01 04 04 02 64 00 6E

---

#### D 轴点动停止

**简介**

对应的TLV：`TLV_MOTOR_D_JOG_STOP`（Type `0x03`，Cmd `CMD_MOTION_D_MOVE`）

D 轴点动停止；无 `Value`。**`Motion_UnpackFromQueue`** 在 General 转发路径处理。

**路由：** **`PORT_GENERAL_TASK`（`0xA2`）**。

**命令组成**

| Port                 | Cmd                 | Tlv-Type               | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ---------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_MOTION_D_MOVE` | `TLV_MOTOR_D_JOG_STOP` | `0`     | （无） | 停止 |

**命令示例（`PORT_GENERAL_TASK`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0xB4 | 0x01 | 0x02 | 0x03     | 0x00    | 0x05 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0xB4+0x01+0x02+0x03+0x00)`。

**Copy**: 5A A5 AA A2 B4 01 02 03 00 05

---

**说明（通用）：** 头文件中另有 `TLV_INVALID_CMD`（`0xFF`），表示无效命令，具体与哪一帧 Cmd/Port 组合使用以固件为准。仅用于板内队列、**不**列入本文串口详表的 TLV 见上文「**说明（本文档范围 — 非 SOC↔MCU 串口载荷）**」。

---

## SOC通信协议（UART2，部分启用）

本章描述 **UART2** 与 **SOC** 正式通信协议的当前定位和约束。  
现阶段这套协议**仅启用打印相关命令**，完整 Port / Cmd / TLV 表尚未冻结；未在本章列出的 SOC 正式协议内容仍按预留处理。

### 当前定位

| 项目 | 说明 |
| :--- | :--- |
| 物理通道 | `UART2` |
| 对接对象 | `SOC` |
| 当前状态 | 部分启用：已启用打印请求 / 打印控制，其余 SOC 正式命令预留 |
| 命令归属 | 正式协议命令和结构收敛到 `Core/Inc/comm_task_app.h`；UART2 与 UART4 使用独立命令空间 |

### 已启用 SOC 命令

UART2 当前沿用本文前述 `SOF1 + SOF2 + FrameType + Port + Cmd + Seq + Len + Payload + CheckSum` 外层帧结构。  
与 UART4 的区别在于：SOC 通过 UART2 使用低位正式命令号；UART4 使用独立的高位调试命令空间。

#### Cmd

Cmd for `PORT_GENERAL_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x40` | 请求打印；Payload 可挂 `TLV_REQ_PR` 或终止打印用 `TLV_PRIN_ABORTED` | `CMD_REQUEST_PRINTING` |

Cmd for `PORT_PRINTING_TASK`

| 序号 | Cmd 值 | 含义 | `#define` |
| :--- | :------ | :--- | :-------- |
| 1 | `0x01` | 打印过程控制；用于开始、就绪、层完成、结束、错误、暂停、恢复、终止等打印流程控制 / 状态上报 | `CMD_PRINTING_CTRL` |

#### 与 UART4 的关系

`CMD_REQUEST_PRINTING`（`0x40`）和 `CMD_PRINTING_CTRL`（`0x01`）仅属于 UART2 / SOC 正式命令空间。

#### 命令详表

下列示例帧中 `Seq = 0x01`；修改 `Seq`、`Value` 或任一字段后需要重新计算 `Sum`。

##### 打印请求

**简介**

对应的TLV：`TLV_REQ_PR`（Type `0x01`，Cmd `CMD_REQUEST_PRINTING`）

SOC→MCU：经 `PORT_GENERAL_TASK` 发起打印请求。

**命令组成**

| Port                | Cmd                    | Tlv-Type     | Tlv-Len | Value  | 说明 |
| ------------------- | ---------------------- | ------------ | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_REQUEST_PRINTING` | `TLV_REQ_PR` | `0`     | （无） | 最简请求 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0x40 | 0x01 | 0x02 | 0x01     | 0x00    | 0x8F |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0x40+0x01+0x02+0x01+0x00)`。

**Copy**: 5A A5 AA A2 40 01 02 01 00 8F

---

##### 打印终止

**简介**

对应的TLV：`TLV_PRIN_ABORTED`（Type `0x0B`，Cmd `CMD_REQUEST_PRINTING`）

SOC→MCU：经 `PORT_GENERAL_TASK + CMD_REQUEST_PRINTING` 下发，清除 General 内用于禁止 SOC 经 General 下发 Z/S/D 的 `SR_PRINTING`，并由 General 转发到 `PrintingProcTask`。

**命令组成**

| Port                | Cmd                    | Tlv-Type           | Tlv-Len | Value  | 说明 |
| ------------------- | ---------------------- | ------------------ | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_REQUEST_PRINTING` | `TLV_PRIN_ABORTED` | `0`     | （无） | 终止打印并恢复经 General 的轴控 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA2 | 0x40 | 0x01 | 0x02 | 0x0B     | 0x00    | 0x99 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA2+0x40+0x01+0x02+0x0B+0x00)`。

**Copy**: 5A A5 AA A2 40 01 02 0B 00 99

---

##### 打印准备中（请求已受理）

**简介**

对应的TLV：`TLV_PRIN_PREPARING`（Type `0x02`，Cmd `CMD_REQUEST_PRINTING`）

MCU→SOC：表示 MCU 已接受 `CMD_REQUEST_PRINTING` 请求，并进入打印准备阶段。`Seq` 与原始打印请求保持一致。

**命令组成**

| Port                | Cmd                    | Tlv-Type             | Tlv-Len | Value  | 说明 |
| ------------------- | ---------------------- | -------------------- | ------- | ------ | ---- |
| `PORT_GENERAL_TASK` | `CMD_REQUEST_PRINTING` | `TLV_PRIN_PREPARING` | `0`     | （无） | 打印请求已受理，进入准备阶段 |

**命令示例（MCU→SOC）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA2 | 0x40 | 0x01 | 0x02 | 0x02     | 0x00    | 0xA1 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA2+0x40+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 BB A2 40 01 02 02 00 A1

---

##### 氧气与压力条件满足（合并上报）

**简介**

对应的TLV：`TLV_PRIN_OXY_PRESSURE_OK`（Type `0x02`，Cmd `CMD_PRINTING_CTRL`）

MCU→SOC：氧气与压力相关条件均已满足。

**命令组成**

| Port                 | Cmd                 | Tlv-Type                    | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | --------------------------- | ------- | ------ | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_OXY_PRESSURE_OK` | `0`     | （无） | 状态上报 |

**命令示例（MCU→SOC）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA4 | 0x01 | 0x01 | 0x02 | 0x02     | 0x00    | 0x64 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA4+0x01+0x01+0x02+0x02+0x00)`。

**Copy**: 5A A5 BB A4 01 01 02 02 00 64

---

##### 打印开始

**简介**

对应的TLV：`TLV_PRINT_START`（Type `0x03`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：下发打印开始和首层层高。

**命令组成**

| Port                 | Cmd                 | Tlv-Type         | Tlv-Len | Value | 说明 |
| -------------------- | ------------------- | ---------------- | ------- | ----- | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRINT_START` | `2`    | 层高，`uint16_t` 小端 | 主机下发 |

**命令示例（SOC→MCU，层高=`0x0064`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value   | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x01 | 0x01 | 0x04 | 0x03     | 0x02    | `64 00` | 0xBC |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x01+0x01+0x04+0x03+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A4 01 01 04 03 02 64 00 BC

---

##### 打印就绪

**简介**

对应的TLV：`TLV_PRIN_READY`（Type `0x04`，Cmd `CMD_PRINTING_CTRL`）

MCU→SOC：打印准备完成，进入就绪态。

**命令组成**

| Port                 | Cmd                 | Tlv-Type        | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | --------------- | ------- | ------ | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_READY` | `0`    | （无） | 最简上报；可扩展 |

**命令示例（MCU→SOC）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA4 | 0x01 | 0x01 | 0x02 | 0x04     | 0x00    | 0x66 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA4+0x01+0x01+0x02+0x04+0x00)`。

**Copy**: 5A A5 BB A4 01 01 02 04 00 66

---

##### 层完成

**简介**

对应的TLV：`TLV_PRIN_LAY_FINISHED`（Type `0x05`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：当前层完成，通知下一层相关处理。

**命令组成**

| Port                 | Cmd                 | Tlv-Type                | Tlv-Len | Value | 说明 |
| -------------------- | ------------------- | ----------------------- | ------- | ----- | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_LAY_FINISHED` | `2`     | 层高，`uint16_t` 小端 | 主机下发 |

**命令示例（SOC→MCU，层高=`0x0064`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value   | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x01 | 0x01 | 0x04 | 0x05     | 0x02    | `64 00` | 0xBE |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x01+0x01+0x04+0x05+0x02+0x64+0x00)`。

**Copy**: 5A A5 AA A4 01 01 04 05 02 64 00 BE

---

##### 打印结束

**简介**

对应的TLV：`TLV_PRIN_FINISHED`（Type `0x06`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：打印流程结束。

**命令组成**

| Port                 | Cmd                 | Tlv-Type           | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ------------------ | ------- | ------ | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_FINISHED` | `0`    | （无） | 主机下发 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x01 | 0x01 | 0x02 | 0x06     | 0x00    | 0x57 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x01+0x01+0x02+0x06+0x00)`。

**Copy**: 5A A5 AA A4 01 01 02 06 00 57

---

##### 打印错误

**简介**

对应的TLV：`TLV_PRIN_ERROR`（Type `0x07`，Cmd `CMD_PRINTING_CTRL`）

MCU→SOC：打印过程错误上报。

**命令组成**

| Port                 | Cmd                 | Tlv-Type        | Tlv-Len | Value | 说明 |
| -------------------- | ------------------- | --------------- | ------- | ----- | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_ERROR` | `1`    | 错误码 `uint8_t` | 从机上报 |

**命令示例（MCU→SOC，错误码=`0x01`）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Value | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ----- | ---- |
| 0x5A | 0xA5 | 0xBB      | 0xA4 | 0x01 | 0x01 | 0x03 | 0x07     | 0x01    | 0x01  | 0x6C |

注：`Sum = (uint8_t)(0x5A+0xA5+0xBB+0xA4+0x01+0x01+0x03+0x07+0x01+0x01)`。

**Copy**: 5A A5 BB A4 01 01 03 07 01 01 6C

---

##### 打印取消

**简介**

对应的TLV：`TLV_PRIN_CANCELLED`（Type `0x08`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：取消打印。

**命令组成**

| Port                 | Cmd                 | Tlv-Type            | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ------------------- | ------- | ------ | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_CANCELLED` | `0`    | （无） | 主机下发 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x01 | 0x01 | 0x02 | 0x08     | 0x00    | 0x59 |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x01+0x01+0x02+0x08+0x00)`。

**Copy**: 5A A5 AA A4 01 01 02 08 00 59

---

##### 打印暂停

**简介**

对应的TLV：`TLV_PRIN_PAUSED`（Type `0x09`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：暂停打印。

**命令组成**

| Port                 | Cmd                 | Tlv-Type         | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ---------------- | ------- | ------ | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_PAUSED` | `0`    | （无） | 主机下发 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x01 | 0x01 | 0x02 | 0x09     | 0x00    | 0x5A |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x01+0x01+0x02+0x09+0x00)`。

**Copy**: 5A A5 AA A4 01 01 02 09 00 5A

---

##### 打印恢复

**简介**

对应的TLV：`TLV_PRIN_RESUMED`（Type `0x0A`，Cmd `CMD_PRINTING_CTRL`）

SOC→MCU：恢复打印。

**命令组成**

| Port                 | Cmd                 | Tlv-Type          | Tlv-Len | Value  | 说明 |
| -------------------- | ------------------- | ----------------- | ------- | ------ | ---- |
| `PORT_PRINTING_TASK` | `CMD_PRINTING_CTRL` | `TLV_PRIN_RESUMED` | `0`    | （无） | 主机下发 |

**命令示例（SOC→MCU）**

| SOF1 | SOF2 | FrameType | Port | Cmd  | Seq  | Len  | Tlv-Type | Tlv-Len | Sum  |
| ---- | ---- | --------- | ---- | ---- | ---- | ---- | -------- | ------- | ---- |
| 0x5A | 0xA5 | 0xAA      | 0xA4 | 0x01 | 0x01 | 0x02 | 0x0A     | 0x00    | 0x5B |

注：`Sum = (uint8_t)(0x5A+0xA5+0xAA+0xA4+0x01+0x01+0x02+0x0A+0x00)`。

**Copy**: 5A A5 AA A4 01 01 02 0A 00 5B

---

### 与 UART4 的边界

- `UART4` 当前承载的是**上位机调试 / 联调协议**，命令定义在 `comm_task_app.h`。
- `UART2` 当前承载的是**SOC 正式通信协议**中已启用的打印相关命令，未列入本章的 SOC 命令仍为预留。
- 打印请求 / 打印控制当前只作为 UART2 / SOC 低位正式命令定义。
- 若后续需要 UART4 具备某项调试控制能力，应在 `0x80~0xFF` 范围内定义独立命令，并在业务 task 中显式处理。

### 当前预留约束

1. `UART2` 的 SOC 协议应与当前 `UART4` 调试协议保持线上命令号解耦，避免调试链路和正式业务链路默认混用。
2. 正式 SOC 协议的 `Port / Cmd / TLV` 建议单独命名、单独分组，并在 `comm_task_app.h` 中集中维护。
3. UART4 高位调试命令必须由业务层直接支持。
4. 若后续仍沿用本文前述 `SOF1 + SOF2 + FrameType + Port + Cmd + Seq + Len + Payload + CheckSum` 外层帧结构，应在本章补齐正式版字段表和命令详表；若不沿用，也应在本章重新定义完整帧格式。
5. 在 SOC 正式协议未补齐前，除本章已启用的打印相关命令外，其余联调命令继续以 **UART4 上位机调试协议** 为准。

### 待后续补充内容

- UART2 与 SOC 的正式帧格式是否完全沿用当前调试协议外层封装
- 正式 SOC 协议的 Port / Cmd / TLV 命名与编号规则
- UART2 收发、ACK、重传和异常响应约束
- UART2 与 UART4 并行工作时的职责边界和兼容策略

---

## 测试用实例（含 Seq）

格式：`5A A5` + **FrameType** + **Port** + **Cmd** + **Seq** + **Len** + **Payload** + **Sum**（与上文「协议帧整体格式」一致，共 `8+Len` 字节）。

下文 Seq 取 `0x01`；**Cmd** 在示例中占位为 `0x00`（实际须与业务/ `comm_task_app.h` 一致）；改任一字段须重算 Sum。

1. 风机类示例（Port=`0xA0`，Cmd=`0x00`，Payload 示意）：

   `5A A5 AA A0 00 01 03 C8 01 00 16`

   `Sum=0x16` = 从 `SOF1`（`0x5A`）到 Payload 末字节 `00` 共 10 字节累加和之低字节。

2. 另一路（TLV 首字节 `C9`）：

   `5A A5 AA A0 00 01 03 C9 01 00 17`

   `Sum=0x17` 为同规则低字节。

3. OTA 升级首条（详见上文“OTA 升级启动与数据流”）

   SOC 发送：`5A A5 AA A6 00 01 02 D9 00 2B`

   MCU 回复：`5A A5 BA A6 00 01 03 D9 01 01 3E`（示意：后续发 2 分区镜像）

   `5A A5 BA A6 00 01 03 D9 01 02 3F`（示意：后续发 1 分区镜像）

   随后 MCU 再发送原始单字节：`43`（`X_C`，表示切换至 XMODEM/CRC16 数据流；此字节**不**包 `SOF1/SOF2`）



