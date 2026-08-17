# SOC `TLV_ALARM_SINGLE_REPORT` 报警显示开发指导

本文面向 SOC/HMI 开发，说明如何解析 MCU 主动上报的
`TLV_ALARM_SINGLE_REPORT`，维护当前报警、显示报警文本和生命周期，并根据
`clear_mode` 提供正确的报警复位入口。

本文适用于当前 V2/V3 报警协议。

## 1. 设计原则

MCU 发布时会提供报警字典，SOC 可根据 `alarm_code` 和 `detail` 查询报警名称、
原因及处理建议。但报警字典与 SOC 软件不一定能和 MCU 固件同时升级，因此必须遵守：

1. **报文字段决定运行时行为。** `severity`、`state_mask` 和 `clear_mode`
   以当前报文为准，不能被本地旧字典覆盖。
2. **报警字典只负责补充文本。** 字典未命中时仍须显示报警，不得丢弃。
3. **保留原始编号。** 界面和日志始终保留 `alarm_code`、`detail` 原始数值，
   方便定位 MCU 与 SOC 字典版本不一致的问题。
4. **未知值安全降级。** 未知报警号、详情号、等级或清除方式不能导致越界、
   断言退出、整帧丢弃或误发 RESET。

因此，即使 MCU 新增或修改了报警而 SOC 字典尚未同步，SOC 仍能根据报文完成基本的
等级显示、状态维护、分组和复位按钮控制。

## 2. 命令和字段布局

- Port：`PORT_ALARM_TASK`（`0xA3`）
- Cmd：`CMD_ALARM_SINGLE_REPORT`（`0x20`）
- TLV type：`TLV_ALARM_SINGLE_REPORT`（`0x01`）
- TLV len：`9`
- 多字节字段：小端序

| Value 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| `0..1` | `alarm_code` | `uint16_t` | 稳定报警号 |
| `2` | `severity` | `uint8_t` | 1 information、2 warning、3 fault、4 trip |
| `3` | `state_mask` | `uint8_t` | bit0 active、bit1 latched、bit2 EVENT，bit3..7 保留 |
| `4` | `clear_mode` | `uint8_t` | 0 AUTO、1 RESET、2 POWER_CYCLE、3 NONE |
| `5..8` | `detail` | `uint32_t` | 报警详情号、诊断快照或按报警定义解释的原因值 |

`severity` 和 `clear_mode` 来自当前 MCU 报警配置表，即使 SOC 报警字典落后，
也应直接使用报文中的值。

## 3. 报警文本与字典降级

建议报警字典支持以 `(alarm_code, detail)` 为组合键查询；如果报警号和详情号使用
独立字典，也应先查报警号，再按该报警定义解释 `detail`。

推荐显示优先级：

1. 报警号和详情号均命中：显示完整报警名称、详情原因和处理建议。
2. 仅报警号命中：显示报警名称，并附加原始 `detail`。
3. 报警号未命中：显示“未知报警”，并附加原始 `alarm_code` 和 `detail`。

建议最低显示内容：

```text
报警号: 1320
详情号: 0
等级: Fault
状态: Active
清除方式: Auto
说明: T1激光器PD1报警
```

未知字典项的降级示例：

```text
报警号: 1399（当前字典未定义）
详情号: 27
等级: Fault
状态: Active
清除方式: Reset
```

注意：`detail=0` 通常表示没有额外详情，但仍应保留该值。部分报警的 `detail`
可能是快照或原因位图，不应在没有对应定义时擅自拆位解释。

## 4. 根据 state_mask 推导生命周期

```text
active  = (state_mask & 0x01) != 0
latched = (state_mask & 0x02) != 0
isEvent = (state_mask & 0x04) != 0
```

SOC 应先判断 `isEvent`，再处理 CONDITION：

| isEvent | active | latched | life state | SOC 处理 |
|---:|---:|---:|---|---|
| 1 | 0 | 0 | EVENT | 追加到消息/历史，不加入当前 CONDITION 报警列表 |
| 0 | 0 | 0 | NORMAL | 生命周期结束，从当前报警列表移除 |
| 0 | 1 | 0/1 | ACTIVE | 故障条件仍存在，保留在当前报警列表 |
| 0 | 0 | 1 | RTN_LATCHED | 条件已恢复，但报警仍锁存并等待清除 |

规则说明：

- EVENT 是一次性消息，没有 RTN/RESET 生命周期。
- `severity != 0` 不代表报警当前有效；NORMAL 上报仍携带配置等级。
- bit3..7 为保留位，当前版本应忽略。
- 如果 EVENT 同时出现 active/latched 位，按异常报文记录原始值并优先作为 EVENT
  处理，不能用它修改某条 CONDITION 的当前状态。

SOC 当前报警缓存建议以 `alarm_code` 为主键。每次收到 CONDITION 单报时，用最新的
`severity`、`state_mask`、`clear_mode` 和 `detail` 覆盖该条缓存；收到 NORMAL 后删除
或转入历史记录。EVENT 应作为独立历史记录追加，不能按 `alarm_code` 覆盖当前报警。

## 5. 根据 clear_mode 显示复位入口

| 值 | 名称 | SOC/HMI 行为 |
|---:|---|---|
| 0 | AUTO | 不显示软件复位按钮；条件恢复后 MCU 自动结束生命周期 |
| 1 | RESET | 对当前锁存报警显示复位按钮；允许向 MCU 下发该 `alarm_code` 的 RESET |
| 2 | POWER_CYCLE | 不显示软件 RESET；提示“需要断电重启/人工处理”，可单独分组 |
| 3 | NONE | EVENT 使用，无 CONDITION 复位流程 |

推荐按钮逻辑：

```text
show_reset = !isEvent && latched && (clear_mode == RESET)
```

对于 `RESET` 报警，active 状态下 MCU 可能拒绝本地复位，也可能把复位请求转发给负责
硬件恢复的业务任务。因此 SOC 可以保留复位按钮，但不能在点击后立即从界面删除报警。
最终状态必须以后续 `TLV_ALARM_SINGLE_REPORT` 为准。

未知 `clear_mode` 必须按“不允许软件复位”处理，同时显示原始值，避免误操作。

建议至少支持以下分组：

- 自动恢复：`AUTO`
- 可软件复位：`RESET`
- 需要重新上电/人工处理：`POWER_CYCLE`
- 一次性事件：`NONE` 或 `isEvent=1`

## 6. RESET 下发与结果判断

SOC 下发单条报警复位：

| Port | Cmd | TLV type | TLV len | Value |
|---|---|---|---:|---|
| `0xA3` | `CMD_ALARM_EVT_RESPONSE`（`0x82`） | `TLV_ALARM_EVT_RESET`（`0x01`） | 2 | `alarm_code`，`uint16_t` 小端 |

示例：复位报警号 `1301`，其十六进制为 `0x0515`：

```text
TLV payload = 01 02 15 05
```

完整串口请求帧格式：

| 字段 | 长度 | 固定值/说明 |
|---|---:|---|
| SOF1 | 1 | `0x5A` |
| SOF2 | 1 | `0xA5` |
| FrameType | 1 | `0xAA`，SOC→MCU 业务命令 |
| Port | 1 | `0xA3`，`PORT_ALARM_TASK` |
| Cmd | 1 | `0x82`，`CMD_ALARM_EVT_RESPONSE` |
| Seq | 1 | SOC 分配的帧序号，暂未使用 |
| Payload Len | 1 | `0x04` |
| TLV Type | 1 | `0x01`，`TLV_ALARM_EVT_RESET` |
| TLV Len | 1 | `0x02` |
| alarm_code | 2 | `uint16_t` 小端 |
| CheckSum | 1 | 从 SOF1 到 alarm_code 末字节累加和的低 8 位 |

复位报警号 `1301`、`Seq=0x01` 的完整命令：

```text
5A A5 AA A3 82 01 04 01 02 15 05 F0
```

其中：

- `5A A5 AA`：帧头和 SOC→MCU 业务帧类型；
- `A3 82`：报警任务端口和报警响应命令；
- `01`：本次请求的 Seq；
- `04`：Payload 共 4 字节；
- `01 02 15 05`：RESET TLV，报警号为 `0x0515=1301`；
- `F0`：本示例的累加校验和。

构造其他报警的复位命令时，只替换小端 `alarm_code`，并根据实际 `Seq` 和报文内容
重新计算 CheckSum，不能固定使用示例中的 `F0`。

必须使用 `alarm_code` 下发 RESET，不能使用 `detail`、报警表序号或 snapshot mask bit。

通信层 ACK 只表示 MCU 已收到合法帧，不表示报警复位成功。SOC 不应在收到 ACK 后直接
删除报警，应等待后续单报：

- `active=0, latched=0`：复位或恢复完成，进入 NORMAL。
- `active=0, latched=1`：条件已恢复，但仍等待复位。
- `active=1`：故障仍存在或硬件恢复尚未完成。

建议复位按钮点击后显示“复位处理中”，并设置合理超时；超时后允许用户重试，但保留
当前报警信息。

## 7. severity 显示建议

| 值 | 名称 | 建议用途 |
|---:|---|---|
| 1 | information | 信息提示/普通消息 |
| 2 | warning | 警告 |
| 3 | fault | 故障 |
| 4 | trip | 安全停机或高危故障 |

颜色、声音和排序可由 SOC 产品规范决定，但等级值必须使用本次报文，而不是旧字典值。
未知等级可显示为 `Unknown(n)`，并采用不低于普通警告的醒目样式。

推荐当前报警排序：

1. severity 从高到低；
2. ACTIVE 优先于 RTN_LATCHED；
3. 同等级按首次出现时间排序。

## 8. 解析伪代码

```text
if cmd != 0x20 or tlv_type != 0x01 or tlv_len != 9:
    reject_frame()

report.alarm_code = read_u16_le(value[0..1])
report.severity   = value[2]
report.state_mask = value[3]
report.clear_mode = value[4]
report.detail     = read_u32_le(value[5..8])

is_event = (report.state_mask & 0x04) != 0
active   = (report.state_mask & 0x01) != 0
latched  = (report.state_mask & 0x02) != 0

text = lookup_dictionary(report.alarm_code, report.detail)
if text not found:
    text = fallback_text_with_raw_numbers(report.alarm_code, report.detail)

if is_event:
    append_event_history(report, text)
else if !active and !latched:
    remove_current_alarm(report.alarm_code)
    append_lifecycle_history("NORMAL", report, text)
else:
    life_state = active ? "ACTIVE" : "RTN_LATCHED"
    upsert_current_alarm(report.alarm_code, report, life_state, text)
```

## 9. 报文示例

报警号 `1204`、Fault、ACTIVE、RESET、detail=0：

```text
01 09 B4 04 03 03 01 00 00 00 00
```

解释：

- `01 09`：TLV type 和 len；
- `B4 04`：`alarm_code=1204`；
- `03`：Fault；
- `03`：active + latched，即 ACTIVE；
- `01`：RESET，应显示软件复位入口；
- `00 00 00 00`：`detail=0`。

EVENT 报警号 `2000`、Information、NONE、detail=258：

```text
01 09 D0 07 01 04 03 02 01 00 00
```

该报文只追加消息/历史，不加入当前 CONDITION 报警列表，也不显示复位按钮。

## 10. 联调检查清单

- 严格校验 Cmd、TLV type 和 `len=9`。
- 所有多字节字段按小端序解析，`detail` 使用无符号 32 位保存。
- 未知报警号或详情号仍能显示原始编号。
- 报文字段优先于本地报警字典中的 severity/clear mode。
- EVENT 不进入当前 CONDITION 报警列表。
- NORMAL 能正确移除当前报警。
- ACTIVE 和 RTN_LATCHED 能正确区分并显示。
- 只有 `RESET + latched` 显示软件复位按钮。
- AUTO 不发送 RESET，POWER_CYCLE 不误发软件 RESET。
- 收到 RESET 请求 ACK 后不立即删除报警。
- 未知枚举和保留位不会导致崩溃或误操作。

## 11. 配套数据

- 报警字典：`Tools/Alarm_Config/alarm_safety_catalog_indexed.csv`
- 报警详情字典：`Tools/Alarm_Config/error_details.csv`
- EVENT 原因定义：`Tools/Alarm_Config/alarm_event_reasons.csv`
- 协议定义：`Core/Inc/def_tlv.h`

每次 MCU 固件发布时，建议同时记录固件版本和报警字典版本。字典可以晚于固件更新，
但 SOC 必须依靠单报中的 `severity`、`state_mask` 和 `clear_mode` 保证基本功能正确。
