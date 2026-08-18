# SOC/HMI `TLV_ALARM_EVT_SNAPSHOT` 报警快照开发指导

## 1. 文档目的

本文面向 SOC 和上位机开发人员，说明如何请求、接收和解析 MCU 的报警快照。

当前快照采用“按需请求 + 稀疏响应”方式：

- 报警状态变化时，MCU 继续实时发送 `TLV_ALARM_SINGLE_REPORT`。
- MCU 不会因报警变化自动发送快照。
- SOC/HMI 在设备重连、页面初始化或需要全量校准时主动请求快照。
- MCU 只发送 active/latched 非零的 slot，减少通信数据量。
- 无报警时，MCU 返回一帧 `count=0` 的全零响应。

快照只包含 CONDITION 报警。2000 段 EVENT 报警仍只通过
`TLV_ALARM_SINGLE_REPORT` 发送，不进入快照位图。

> **只看一处即可开始开发：** 先看第 2 节理解“报警分组、mask 字、slot”的关系，
> 再看第 6 节的 MCU“报警码→slot/bit”和
> SOC“slot/bit→报警码”的正反向公式、速查表和完整示例。

## 2. 先理解报警分组、mask 字和 slot

### 2.1 为什么要分组

CONDITION 报警码按百位范围固定分成 4 组：

| 报警组 group | 报警码段 | 可进入快照的报警码 | 保留报警码 |
|---:|---|---|---|
| 0 | 1000 段 | `1000..1095` | `1096..1099` |
| 1 | 1100 段 | `1100..1195` | `1196..1199` |
| 2 | 1200 段 | `1200..1295` | `1296..1299` |
| 3 | 1300 段 | `1300..1395` | `1396..1399` |

每组在报警码上预留 100 个编号，但快照只为前 96 个编号分配位。每组最后 4 个编号
永久保留，不允许出现在快照中。

```text
group 0: 1000..1095
group 1: 1100..1195
group 2: 1200..1295
group 3: 1300..1395
```

### 2.2 一个 mask 字表示什么

一个 mask 字是 `uint32_t`，有 32 个 bit，因此一个 mask 字最多表示 32 个报警：

```text
bit0  表示该 word 的第 0 个报警
bit1  表示该 word 的第 1 个报警
...
bit31 表示该 word 的第 31 个报警
```

每个报警组有 96 个有效报警编号，所以每组正好拆成 3 个 mask 字：

```text
一个报警组 = 3 个 mask word × 每个 word 32 bit = 96 个报警位
```

以 group 1（1100 段）为例：

| 组内 word | 覆盖报警码 | bit 与报警码关系 |
|---:|---|---|
| 0 | `1100..1131` | bit0=`1100`，bit31=`1131` |
| 1 | `1132..1163` | bit0=`1132`，bit31=`1163` |
| 2 | `1164..1195` | bit0=`1164`，bit31=`1195` |

例如报警码 `1134` 属于 group 1 的 word 1，并对应 bit2：

```text
1134 = 1100 + 1×32 + 2
```

因此 word 1 的 mask 为 `0x00000004` 时，表示报警码 `1134` 置位。

### 2.3 slot_id 是 group 和 word 的扁平编号

通信帧不分别发送 `group` 和 `word`，而是把它们合并成一个 `slot_id`：

```text
slot_id = group × 3 + word
```

所以 4 个报警组、每组 3 个 mask 字，一共得到 12 个 slot：

```text
4 group × 3 word/group = 12 slot
slot_id 范围 = 0..11
```

反向计算：

```text
group = slot_id / 3    // 整数除法
word  = slot_id % 3
```

例如 `slot_id=4`：

```text
group = 4 / 3 = 1
word  = 4 % 3 = 1
```

因此 `slot_id=4` 表示 group 1 的 word 1，覆盖报警码 `1132..1163`。

### 2.4 active_mask 和 latched_mask 使用同一套报警位

每个 slot 同时携带两个 32-bit mask：

- `active_mask`：对应报警条件当前仍然存在。
- `latched_mask`：对应报警已经锁存，可能正在等待复位。

两个 mask 的 `slot_id`、group、word 和 bit 定义完全相同。例如收到：

```text
slot_id     = 4
active_mask = 0x00000004
latched_mask= 0x00000000
```

表示 `slot_id=4` 的 bit2 置位，也就是报警码 `1134` 当前 active，但没有 latched。

必须先按 `slot_id` 找到报警组和 mask 字，再遍历 mask 中的置位 bit，最后才能还原
报警码。关系可以概括为：

```text
报警分组 group
    └── 组内 3 个 mask word
            └── 每个 word 的 32 个 bit
                    └── 每个 bit 对应一个固定 alarm_code

slot_id = group × 3 + word
```

## 3. 请求命令

SOC/HMI 使用以下命令请求报警快照：

| 字段 | 值 |
|---|---|
| Port | `PORT_ALARM_TASK`（`0xA3`） |
| Cmd | `CMD_ALARM_EVT_REPORT`（`0x81`） |
| Seq | 由 SOC 分配 |
| TLV type | `TLV_ALARM_EVT_REQSPT`（`0x05`） |
| TLV len | `0` |
| Value | 无 |

请求 TLV payload：

```text
05 00
```

通信层返回的 ACK 只表示请求帧已被 MCU 接收。ACK 不是报警快照，SOC 必须继续等待
`TLV_ALARM_EVT_SNAPSHOT`。

## 4. 响应命令

MCU 使用以下命令响应：

| 字段 | 值 |
|---|---|
| Port | `PORT_ALARM_TASK`（`0xA3`） |
| Cmd | `CMD_ALARM_EVT_REPORT`（`0x81`） |
| Seq | 回显请求 Seq |
| TLV type | `TLV_ALARM_EVT_SNAPSHOT`（`0x04`） |
| TLV len | `11` |

同一轮快照的所有响应分片具有相同的 Seq、`nonzero_slot_count` 和
`highest_severity`。MCU 在收到请求时一次性复制状态，因此多分片属于同一个状态时刻。

## 5. Snapshot Value 布局

| Value 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| `0` | `nonzero_slot_count` | `uint8_t` | 本轮 `(active_mask \| latched_mask) != 0` 的 slot 总数 |
| `1` | `slot_id` | `uint8_t` | 当前分片 slot，范围 `0..11` |
| `2` | `highest_severity` | `uint8_t` | 快照时刻全部有效 CONDITION 的最高等级 |
| `3..6` | `active_mask` | `uint32_t LE` | 当前 slot 的活动报警位图 |
| `7..10` | `latched_mask` | `uint32_t LE` | 当前 slot 的锁存报警位图 |

`highest_severity`：

- `0`：无有效 CONDITION 报警。
- `1`：INFO。
- `2`：WARNING。
- `3`：ERROR。
- `4`：CRITICAL。

## 6. 报警码与快照位的双向计算

### 6.1 核心结论

一帧快照中的 `value[1]` 是 `slot_id`，mask 中置位的位置是 `bit`。
SOC 计算报警码时只需要这两个数：

```text
group      = slot_id / 3        // 整数除法
word       = slot_id % 3        // 组内 word
alarm_code = 1000 + group * 100 + word * 32 + bit
```

反过来，MCU 已知 `alarm_code` 时：

```text
relative = alarm_code - 1000
group    = relative / 100
offset   = relative % 100
word     = offset / 32
bit      = offset % 32
slot_id  = group * 3 + word
bit_mask = 1U << bit
```

范围固定为：

```text
group   = 0..3
word    = 0..2
bit     = 0..31
slot_id = 0..11
```

每组只允许偏移 `00..95`。`xx96..xx99` 永久保留，不能生成快照位；2000 段
EVENT 也不进入快照。

### 6.2 slot_id 报警码速查表

看到 `slot_id` 后，可以直接使用下表中的基础报警码：

| slot_id | group | word | 报警码计算 | 本 slot 覆盖范围 |
|---:|---:|---:|---|---|
| 0 | 0 | 0 | `1000 + bit` | 1000..1031 |
| 1 | 0 | 1 | `1032 + bit` | 1032..1063 |
| 2 | 0 | 2 | `1064 + bit` | 1064..1095 |
| 3 | 1 | 0 | `1100 + bit` | 1100..1131 |
| 4 | 1 | 1 | `1132 + bit` | 1132..1163 |
| 5 | 1 | 2 | `1164 + bit` | 1164..1195 |
| 6 | 2 | 0 | `1200 + bit` | 1200..1231 |
| 7 | 2 | 1 | `1232 + bit` | 1232..1263 |
| 8 | 2 | 2 | `1264 + bit` | 1264..1295 |
| 9 | 3 | 0 | `1300 + bit` | 1300..1331 |
| 10 | 3 | 1 | `1332 + bit` | 1332..1363 |
| 11 | 3 | 2 | `1364 + bit` | 1364..1395 |

例如收到 `slot_id=7`，mask 的 bit4 为 1：

```text
alarm_code = 1232 + 4 = 1236
```

### 6.3 MCU 侧：由 alarm_code 计算上传位置

MCU 对每条 CONDITION 执行以下步骤：

1. 检查报警号是否属于 1000..1395。
2. 计算 `group` 和组内 `offset`。
3. 若 `offset >= 96`，说明使用了保留号，禁止进入快照。
4. 计算 `word / bit / slot_id / bit_mask`。
5. `active=1` 时置位 `active_mask[slot_id]`。
6. `latched=1` 时置位 `latched_mask[slot_id]`。
7. 只上传 `(active_mask | latched_mask) != 0` 的 slot。

MCU 侧伪代码：

```c
if ((alarm_code < 1000U) || (alarm_code > 1395U))
{
    reject_alarm_code();
    return;
}

relative = alarm_code - 1000U;
group = relative / 100U;
offset = relative % 100U;

if ((group < 4U) && (offset < 96U))
{
    word = offset / 32U;
    bit = offset % 32U;
    slot_id = group * 3U + word;
    bit_mask = 1UL << bit;

    if (active != 0U)
    {
        active_mask[slot_id] |= bit_mask;
    }
    if (latched != 0U)
    {
        latched_mask[slot_id] |= bit_mask;
    }
}
```

MCU 示例，报警号 `1103`：

```text
relative = 1103 - 1000 = 103
group    = 103 / 100 = 1
offset   = 103 % 100 = 3
word     = 3 / 32 = 0
bit      = 3 % 32 = 3
slot_id  = 1 * 3 + 0 = 3
bit_mask = 1 << 3 = 0x00000008
```

因此 MCU 在 `slot_id=3` 的 mask 中置 bit3。如果报警同时 active 和 latched：

```text
value[1]     = 3
active_mask  = 0x00000008
latched_mask = 0x00000008
```

### 6.4 SOC 侧：由 slot_id 和 bit 还原 alarm_code

SOC 收到每个非零 slot 后，应分别遍历 active 和 latched 两个位图。只要某 bit 在任一
mask 中置位，就按固定公式还原报警码。

SOC 侧 C# 示例：

```csharp
static int CalculateAlarmCode(byte slotId, int bit)
{
    if (slotId >= 12)
        throw new ArgumentOutOfRangeException(nameof(slotId));
    if (bit < 0 || bit >= 32)
        throw new ArgumentOutOfRangeException(nameof(bit));

    int group = slotId / 3;
    int word = slotId % 3;
    return 1000 + group * 100 + word * 32 + bit;
}

static void DecodeSlot(byte slotId, uint activeMask, uint latchedMask)
{
    uint unionMask = activeMask | latchedMask;

    for (int bit = 0; bit < 32; bit++)
    {
        uint bitMask = 1u << bit;
        if ((unionMask & bitMask) == 0)
            continue;

        int alarmCode = CalculateAlarmCode(slotId, bit);
        bool active = (activeMask & bitMask) != 0;
        bool latched = (latchedMask & bitMask) != 0;

        UpdateTemporaryAlarm(alarmCode, active, latched);
    }
}
```

SOC 对上面的 `slot_id=3, bit3` 反算：

```text
group      = 3 / 3 = 1
word       = 3 % 3 = 0
alarm_code = 1000 + 1*100 + 0*32 + 3
           = 1103
```

### 6.5 必须避免的旧算法

新版协议禁止使用 MCU 内部 index 计算：

```text
错误：word_index = internal_index / 32
错误：bit_index  = internal_index % 32
```

正确做法只有两种：

```text
MCU：alarm_code -> group/word/bit -> slot_id + mask
SOC：slot_id + mask bit -> group/word/bit -> alarm_code
```

SOC 不需要依赖 `alarm_mask_map_soc.csv` 推算报警号。该 CSV 仅用于报警名称、等级和
说明等显示元数据，不能用其中的旧 internal index/word 替代固定公式。

## 7. active 与 latched

对每个置位 bit 分别读取 active 和 latched：

| active | latched | 状态 | SOC/HMI 行为 |
|---:|---:|---|---|
| 0 | 0 | NORMAL | 不在当前报警列表中 |
| 1 | 0/1 | ACTIVE | 异常条件仍然存在 |
| 0 | 1 | RTN_LATCHED | 条件已恢复，但报警仍锁存或等待复位 |

不能使用 `highest_severity != 0` 判断某一条报警是否有效，必须检查该报警对应的
active/latched bit。

## 8. 稀疏分片接收规则

SOC/HMI 发送请求后，应创建一份 12-slot 全零临时缓存，并按以下规则接收：

1. 只接受 Port、Cmd、TLV type 和 len 均正确的响应。
2. 响应 Seq 必须等于当前请求 Seq。
3. `nonzero_slot_count` 必须在 `0..12` 范围内。
4. `slot_id` 必须小于 12。
5. 同一轮所有分片的 `nonzero_slot_count` 和 `highest_severity` 必须一致。
6. 同一轮不得出现重复 `slot_id`。
7. 当 `count > 0` 时，每个分片必须满足 `(active_mask | latched_mask) != 0`。
8. 收齐 `count` 个不同 slot 后，才允许一次性替换当前全局报警状态。
9. 未发送的 slot 必须按全零处理，不能保留上一轮的旧值。

禁止每收到一帧就直接修改正式报警列表，否则多分片期间可能出现新旧快照混合。

## 9. 无报警清空响应

当 MCU 当前不存在 active 或 latched CONDITION 时，只发送一帧：

```text
nonzero_slot_count = 0
slot_id             = 0
highest_severity    = 0
active_mask         = 0
latched_mask        = 0
```

对应 TLV payload：

```text
04 0B 00 00 00 00 00 00 00 00 00 00 00
```

收到合法的 `count=0` 响应后，SOC/HMI 应原子清空全部 CONDITION 当前状态。
EVENT 历史记录不受影响。

## 10. 多分片示例

假设本轮只有 slot 0 和 slot 3 非零，最高等级为 ERROR：

```text
请求 TLV:
05 00

响应 slot 0:
04 0B 02 00 03 03 00 00 00 03 00 00 00

响应 slot 3:
04 0B 02 03 03 08 00 00 00 08 00 00 00
```

解析结果：

- `count=2`，必须收齐两个不同 slot 后才能提交。
- slot 0 的 bit0、bit1 置位，对应报警号 1000、1001。
- slot 3 的 bit3 置位，对应报警号 1103。
- 其余十个 slot 全部按零处理。

分片发送顺序当前通常按 slot_id 从小到大，但 SOC 不应依赖顺序，应使用
`slot_id` 定位。

## 11. 与 SINGLE_REPORT 并发处理

快照用于全量校准，`TLV_ALARM_SINGLE_REPORT` 用于实时增量更新。请求和接收快照期间，
新的单条报警仍可能到达。

推荐处理方式：

1. 发送快照请求时记录当前请求 Seq，并创建临时快照缓存。
2. 快照未完成期间，将收到的 CONDITION SINGLE_REPORT 按接收顺序暂存。
3. 收齐快照后，先原子提交快照，再按原顺序重放暂存的 SINGLE_REPORT。
4. EVENT SINGLE_REPORT 可直接写入事件历史，不修改 CONDITION 快照缓存。
5. 快照超时时，丢弃临时快照，并将暂存的 SINGLE_REPORT 应用到当前正式状态。

该顺序可以避免“较旧快照在较新单报之后提交”导致界面状态回退。

## 12. 超时、重试和连续请求

- 建议使用可配置的快照接收超时，例如 1～3 秒。
- 超时未收齐时，必须丢弃整轮临时快照，不能提交部分数据。
- 重试必须分配新的 Seq。
- SOC 同时只维护一个快照请求。
- 新请求到达 MCU 后，会用新 Seq 和最新状态重新开始响应。
- SOC 收到新 Seq 的响应后，应丢弃旧 Seq 的未完成分片。
- MCU 通信队列繁忙时会保留当前 slot 并在后续周期继续发送，因此分片可能存在间隔。

## 13. 解析伪代码

```csharp
void OnSnapshot(byte frameSeq, ReadOnlySpan<byte> value)
{
    if (value.Length != 11 || frameSeq != pendingRequestSeq)
        return;

    byte count = value[0];
    byte slotId = value[1];
    byte severity = value[2];
    uint active = ReadUInt32LittleEndian(value.Slice(3, 4));
    uint latched = ReadUInt32LittleEndian(value.Slice(7, 4));

    if (count > 12 || slotId >= 12)
    {
        RejectSnapshot();
        return;
    }

    if (!RoundMetadataMatches(count, severity))
    {
        RejectSnapshot();
        return;
    }

    if (count == 0)
    {
        if (slotId != 0 || severity != 0 || active != 0 || latched != 0)
        {
            RejectSnapshot();
            return;
        }
        CommitAllClear();
        return;
    }

    if ((active | latched) == 0 || SlotAlreadyReceived(slotId))
    {
        RejectSnapshot();
        return;
    }

    CacheSlot(slotId, active, latched);

    if (ReceivedSlotCount() == count)
    {
        CommitSnapshotAtomically();
        ReplayBufferedSingleReports();
    }
}
```

遍历 slot 中置位 bit：

```csharp
int group = slotId / 3;
int word = slotId % 3;

for (int bit = 0; bit < 32; bit++)
{
    uint mask = 1u << bit;
    bool isActive = (activeMask & mask) != 0;
    bool isLatched = (latchedMask & mask) != 0;

    if (isActive || isLatched)
    {
        int alarmCode = 1000 + group * 100 + word * 32 + bit;
        UpdateTemporaryAlarm(alarmCode, isActive, isLatched);
    }
}
```

## 14. 联调检查清单

- 请求使用 `0xA3 / 0x81 / TLV 0x05 / len=0`。
- ACK 和 snapshot 响应能够正确区分。
- 请求前 MCU 不自动发送 snapshot。
- 响应 Seq 与请求 Seq 一致。
- 只收到 active/latched 非零 slot。
- active 和 latched 能同时正确解析。
- 未发送 slot 能清除上一轮旧状态。
- `count=0` 能清空全部 CONDITION。
- 多分片未收齐时不会更新正式报警列表。
- 重复、非法 slot 和不一致 count 会丢弃整轮快照。
- 超时后不会提交部分快照。
- 新 Seq 能终止旧 Seq 的未完成接收。
- 快照期间的 SINGLE_REPORT 能在快照提交后正确重放。
- EVENT 不进入快照，但仍能通过 SINGLE_REPORT 写入历史。
- RESET、SINGLE_REPORT 和 SAFETY_TRIP 行为不因快照解析受到影响。
