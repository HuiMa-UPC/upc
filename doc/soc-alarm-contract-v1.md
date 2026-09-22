# 设备告警重构：SOC—云端对接契约

版本：对接稿 1.0｜日期：2026-09-18｜状态：云端已实现，待 MR 审核及设备联调

配套文档：[工程实现与部署说明](README.md)。本文只约定云端接收的数据、返回的回执及处理语义，供设备团队对接评审。字段是拟定契约，不代表现有设备已全部支持。

## 1. 对接范围

上行：单条告警/事件、完整状态快照、状态质量信息。下行：持久化接收回执。

本文不规定 SOC 如何获取数据、何时自动快照、串口命令与分片处理、缓存实现、重试策略或硬件资源。设备团队自行实现，只需确认上云数据能够满足本文语义。云端不提供手动同步、流绑定或远程 RESET 接口。

## 2. 告警字段和状态含义

| 字段 | 类型与含义 |
|---|---|
| alarm_code | 0–65535 的整数，事件表 code |
| severity | 0–255 的整数；已知值 1 INFO、2 WARNING、3 ERROR、4 CRITICAL |
| state_mask | 0–255 的整数；bit0 active、bit1 latched、bit2 EVENT |
| clear_mode | 0–255 的整数；0 AUTO、1 RESET、2 POWER_CYCLE、3 NONE；仅展示 |
| detail | 0–4294967295 的整数；detail code，歧义见第 10 节 |

**active/latched 是两个独立布尔位，不是“0 告警、1 消警”。**

| mask 低三位 | 云端解释 | 当前 CONDITION |
|---|---|---|
| 000 | NORMAL：生命周期结束 | 无 |
| 001 | ACTIVE：故障中，未锁存 | 有 |
| 010 | RTN_LATCHED：条件已恢复，但仍锁存 | 有 |
| 011 | ACTIVE：故障中且锁存 | 有 |
| 1xx | EVENT：一次性事件 | 不改变 CONDITION |

bit2 优先。EVENT 同时置 active/latched 时保留原始组合，仍按 EVENT。保留位 bit3–7 忽略语义、保留原值。severity 非零不代表告警未恢复；clear_mode 不参与云端自动消警。

## 3. 身份与顺序的对接约束

云端需要区分消息重发、不同事件、旧消息和当前状态。字段表达的含义如下；由设备团队确认如何提供，不在本文指定 MCU/SOC 内部改造方式。

| 字段 | 云端依赖的语义 |
|---|---|
| message_id | 一条上云消息的唯一身份，同消息重发不变 |
| stream_id + report_seq | 同一上报流内不可复用的递增消息序号 |
| mcu_boot_id | 报警源会话身份；重启导致序号重新开始时必须可区分，不能按 UUID 大小排序 |
| source_seq | 同一源会话内单报的递增序号，同一源事件重发不变 |
| 快照 source_seq=W | 快照状态与单报共享水位，表示包含截至 W 的状态变化，不包含 W 之后的变化 |

单报序号从 1 开始，尚无单报时快照水位允许为 0。如果设备不能提供这些语义，必须在协议冻结前说明；云端不能用接收时间假装源顺序准确。

## 4. MQTT 通道与公共消息头

| 方向 | Topic | QoS / retain |
|---|---|---|
| SOC → 云 | snowpod/{region}/{deviceSn}/alarm/report | 1 / false |
| 云 → SOC 持久化接收回执 | snowpod/{region}/{deviceSn}/alarm/ack | 1 / false |

设备仅能发布自己的 report、订阅自己的 ack。云端以认证主体与 topic 绑定确定设备身份，JSON device_id 必须一致。认证失败不向攻击者发送带设备信息的业务回执。

JSON UTF-8，单条最大 256 KiB。禁止重复 JSON key。上行三种 type：alarm.single、alarm.snapshot、alarm.status。

| 公共字段 | 类型 / 规则 |
|---|---|
| v | 整数，固定 1；协议主版本，与字典版本无关 |
| type | 上述三种消息类型 |
| device_id | 设备 SN，必须与认证主题一致 |
| message_id | UUID，一条持久化消息一个；重试保持不变 |
| stream_id | UUID，上报流身份；同一流内序号不重置 |
| report_seq | 十进制字符串，1 起严格递增，最大 9223372036854775807 |
| ts | 首次生成消息的 UTC 毫秒，时钟未知为 0；重试不改 |
| data | 业务对象 |

序号统一用十进制字符串，避免 JavaScript 大整数精度损失；不带正号、空格或前导零，水位允许字符串 "0"。UINT32 detail 用 JSON 整数，云端数据库用 BIGINT，不能用 Java 有符号 int 截断。

同一消息重发时保持 message_id、stream_id、report_seq 及载荷不变。同一流内 report_seq 不复用；需要重置序号时必须使用不同 stream_id。

源消息身份为 source_component+mcu_boot_id+source_seq。云端按源身份再次去重，相同源身份不同业务内容视为冲突，不以最后一次覆盖。

## 5. 单报 alarm.single

```json
{
  "v": 1,
  "type": "alarm.single",
  "device_id": "SN-DEMO-001",
  "message_id": "c432a2f1-0a88-4ef0-a734-7b4e15b6a001",
  "stream_id": "e78c296d-312e-42d7-b7a4-8abf2418b001",
  "report_seq": "101",
  "ts": 1789704000000,
  "data": {
    "source_component": "main_mcu",
    "catalog_key": "main-mcu",
    "mcu_boot_id": "b220334d-cd93-47a5-bb1e-e4ffd311b001",
    "alarm_definition_version": "1.0.0",
    "source_seq": "45",
    "observed_at": 1789704000000,
    "time_quality": "synced",
    "mcu_uptime_ms": "83000",
    "alarm_code": 1000,
    "severity": 3,
    "state_mask": 3,
    "clear_mode": 1,
    "detail": 0
  }
}
```

示例只演示格式，不宣称这些组合是某个真实故障的业务定义。

- source_component 本期固定 main_mcu；catalog_key 本期固定 main-mcu，不允许设备任意创建云端目录。
- mcu_boot_id 本稿建议 UUID，必须稳定代表报警源会话；普通网络重连不改变同一源事件的身份。
- observed_at 是 SOC 首次观察源消息的时间，时钟未知为 null；time_quality 为 synced/unsynced。ts 不是重试发送时间。
- mcu_uptime_ms 是源事件发生时的 MCU 启动内毫秒数字字符串，可缺省。不能用重传时刻替代。
- alarm_code 范围 0–65535，severity/state_mask/clear_mode 范围 0–255，detail 范围 0–4294967295。范围内未知枚举保留并标识，范围外拒绝。
- alarm_definition_version 可以缺省/null，使用云端明确配置的默认定义并标识。空字符串或不合法格式拒绝；合法但未知版本接收为未匹配。
- 同一 boot 内定义版本保持稳定；定义改变需重新建立源会话并完整校准，不能让同一 code 在一个会话中变义。
- detail=0 按 detail 字典的“无详细原因”语义展示。所有其他值先保留原数值；是否可按 code 解释，受第 10 节待确认事项约束。

一次性 EVENT 使用同一结构，state_mask 的 bit2=1。例如 mask=4 即普通 EVENT；不需要额外发一个 NORMAL，也不创建待清除实例。

## 6. 完整快照 alarm.snapshot

云端只接受一条消息表达的完整状态快照，不接收串口分片。何时采集和上报由设备团队决定；未取得有效完整快照时，云端不声明全量状态已同步。

```json
{
  "v": 1,
  "type": "alarm.snapshot",
  "device_id": "SN-DEMO-001",
  "message_id": "c432a2f1-0a88-4ef0-a734-7b4e15b6a002",
  "stream_id": "e78c296d-312e-42d7-b7a4-8abf2418b001",
  "report_seq": "102",
  "ts": 1789704001000,
  "data": {
    "source_component": "main_mcu",
    "catalog_key": "main-mcu",
    "mcu_boot_id": "b220334d-cd93-47a5-bb1e-e4ffd311b001",
    "alarm_definition_version": "1.0.0",
    "source_seq": "45",
    "observed_at": 1789704001000,
    "time_quality": "synced",
    "coverage": "condition_slots_v1",
    "complete": true,
    "reported_count": 1,
    "highest_severity": 3,
    "entries": [
      {
        "alarm_code": 1000,
        "state_mask": 3,
        "severity": 3,
        "clear_mode": 1,
        "detail": 0,
        "metadata_origin": "single_report",
        "metadata_source_seq": "45"
      }
    ]
  }
}
```

coverage=condition_slots_v1 覆盖 1000–1095、1100–1195、1200–1295、1300–1395，共 384 个位置；各组 xx96–xx99 不在范围内。entries 只列 active 或 latched 非零的 CONDITION；码唯一，不能超范围，bit2 必须为零，reported_count 必须等于 entries 长度（0–384）。空 entries 是合法的全范围正常快照。结构校验不通过时整份拒绝，不能部分应用。

metadata_origin 为 snapshot（原子完整元数据）、single_report（缓存）或 unknown。缓存必须属于同 boot/code/定义，metadata_source_seq 不大于快照水位；无法证明时传 null/unknown。缓存元数据只是最后已知值，不宣称在快照时刻已经刷新。unknown 时 severity、clear_mode、detail、metadata_source_seq 均为 null。snapshot 来源的 metadata_source_seq 等于快照水位。

highest_severity 是设备快照整体字段，只用于整体展示/诊断，不能填到每个 entries 的 severity。字典建议值与运行时元数据分别存储。

云端在一个事务中校准覆盖内所有码，包括没出现在 entries 的码；NORMAL 墓碑也保存水位 W。同 boot 已有源水位大于 W 的码不回退。随后仅将源序号大于 W 的单报作用于当前状态；不大于 W 的迟到单报仅补历史。EVENT 即使不大于 W，也需要独立幂等入历史，快照从不替代 EVENT。

## 7. 状态与重启恢复

alarm.status 使用第 4 节公共头，data 格式如下：

```json
{
  "source_component": "main_mcu",
  "catalog_key": "main-mcu",
  "mcu_boot_id": "b220334d-cd93-47a5-bb1e-e4ffd311b001",
  "alarm_definition_version": "1.0.0",
  "mcu_link": "online",
  "last_source_seq": "45",
  "capabilities": {
    "source_identity": true,
    "atomic_snapshot_watermark": true,
    "snapshot_metadata": false
  },
  "lost_report_ranges": [],
  "lost_source_ranges": []
}
```

mcu_link 为 online/offline/unknown；无有效源会话时 mcu_boot_id、last_source_seq 可为 null。capabilities 描述字段实际能力，不能将不支持的水位保证标记为 true。

lost_report_ranges 元素为 {"from":"90","to":"92"}；lost_source_ranges 另带 mcu_boot_id。区间含两端，表示已知不可恢复的缺失。云端保留缺口记录，后续空列表不抹掉历史。

状态消息表示所报告的源会话及链路情况，不以 SOC 连云推断 MCU 在线。状态新鲜度时限作为双方联调参数确定，本文不规定发送定时器。同流较旧 report_seq 的 status 不更新质量；原消息重发不刷新质量。新鲜度以首次持久接收时间计算，表示云端最近收到状态的时间，不证明源时钟准确。

新流/源会话的有效完整快照可建立新的当前状态；云端在校准前保留最后已知状态并标记 pending。旧流/源会话迟到数据只补历史，不能重新接管当前状态。无需额外绑定接口。

## 8. 云端接收回执

MQTT PUBACK 只证明 MQTT 层接收，不证明云端业务数据库落盘。只有 alarm.receipt 的 accepted 表示云端已持久接收；未收到回执不能当作成功。

```json
{
  "v": 1,
  "type": "alarm.receipt",
  "device_id": "SN-DEMO-001",
  "ts": 1789704000200,
  "data": {
    "message_id": "c432a2f1-0a88-4ef0-a734-7b4e15b6a001",
    "stream_id": "e78c296d-312e-42d7-b7a4-8abf2418b001",
    "report_seq": "101",
    "result": "accepted",
    "code": "ACCEPTED",
    "warnings": []
  }
}
```

| result | 含义 | 对接处理 |
|---|---|---|
| accepted / ACCEPTED 或 DUPLICATE | 原始消息（含待处理状态）、幂等记录持久化完成；不保证投影已完成 | 核对 message_id/stream_id/report_seq，标记本次消息交付成功 |
| rejected / INVALID_PAYLOAD、IDENTITY_CONFLICT | 永久格式问题或身份冲突 | 未接收成功；修正数据或协议问题后处理，不作为成功回执 |

warnings 当前固定为空数组，保留作为可选扩展字段。未知字典/枚举、默认版本和元数据缺失在云端展示，不影响持久接收。数据库暂时失败时不发送 accepted；没有业务回执可原身份重发。

回执是逐条确认，无隐式累计确认。相同 message_id 或 stream+report_seq 内容冲突必拒绝。相同源身份的重复还需比较源载荷，不把 SOC 包装时间差误判为源内容变化。回执自身允许重复，SOC 幂等处理。

云端持久接收旧源会话或旧水位快照后跳过当前态更新。accepted 的语义始终是持久接收，不承诺消息一定改变当前状态。拒绝记录、待处理状态和处理错误由云端保留。

## 9. 云端处理保证与边界

- 同消息重复上报只产生一次业务效果；回执可以重复。
- 旧 ACTIVE 不能覆盖更新的 NORMAL；同源水位的快照不能覆盖更晚单报。
- 完整快照只校准声明范围内的设备 CONDITION，不清除规则告警或 EVENT。
- 新源会话切换造成的实例结束标记 source_reinitialized；快照校准结束标记 snapshot_reconciled，不冒充精确 NORMAL 边沿。
- 快照可以恢复当前态，但不能补出遗漏的 EVENT。当前状态已同步与历史有缺口可以同时存在。
- accepted 后的处理失败由云端有限重试；耗尽后保留失败记录并由运维修复恢复，不要求设备生成新事件。
- 未确认消息可以原样重发；不规定设备缓存、发送顺序或退避实现。无法恢复的缺失以第 7 节约定报告。

## 10. 字典和固件的关系

固件版本独立作为设备信息报告，不参与告警字典选择。设备只报 alarm_definition_version，例如 1.2.0；云端维护该定义的字典 revision，例如 3，完整显示 1.2.0.3。第四段只适用于前三段完全相同的定义。

同固件可报告不同定义，不同固件也可报告同定义；前提是设备实际按所报告定义编码，不能只改版本标签。云端不下发字典给设备，不维护固件×字典兼容矩阵。

建议默认定义 1.0.0，须与首次正式设备定义核准后配置。缺省使用默认并标记；明确未知定义保留 raw 并显示未知，不能套默认。同一版本下码义不可改，云端文本修订发布为新的不可变 revision。

**必须冻结的业务差异：**用户希望 detail 全部是独立 detail code，但设备表部分描述电流值、故障位图等。设备方应确认是否全部转换为 code；若保留原始值，需按定义版本提供明确的 event code→detail 类型清单。确认前，这些歧义条目只展示原始值/语义待确认，不能偶然命中 detail 字典就展示错误原因。本方案不要求建立 event/detail 固定父子关系表。

## 11. 联调验收

| 场景 | 验收结果 |
|---|---|
| mask=1/3→2→0 | 故障中→恢复但锁存→结束，同一实例，时间分开 |
| mask=4，以及 bit2 与其他位同时置位 | 仅记 EVENT，原始组合保留，不污染当前 CONDITION |
| EVENT/单报/回执重复 | 不重复实例、历史事件和通知 |
| 先收到 NORMAL，后收到旧 ACTIVE | 墓碑阻止旧消息重开；历史可追溯 |
| 源序号不连续 / 设备声明丢失区间 | 标记缺口，完整快照可恢复当前态，历史缺口保留；report_seq 用于传输去重和状态排序，不作为全局连续进度 |
| 快照不完整、码重复或覆盖范围错误 | 整份拒绝，不部分清除 |
| 合法空快照 | 只结束覆盖内设备 CONDITION，保留规则与 EVENT |
| 增量跨越快照水位 | 新增量不回退；旧增量只入历史；EVENT 不被快照吞掉 |
| 位图快照首次发现未知告警 | 元数据 null/unknown，不借全局最高等级冒充 |
| MCU 重启且旧包迟到 | 经当前会话完整快照切 boot，旧 boot 不重掌当前态 |
| 新上报流 | 校准前 pending，完整快照建立状态，旧流迟到不回退 |
| 未报版本 / 未知版本 / 未知码 | 默认标识 / 未匹配标识 / 保留原文，无静默丢失 |
| UINT32 最大 detail、未知 severity | 无溢出，保留数值并标识 |
| 云数据库失败、Outbox 重发 | 失败不发 accepted；提交后回执/通知可幂等重发 |
| 缺口声明、源离线、历史重发 | 明确缺口/质量，重复不生成新的通知；迟到事件标记 isHistorical，供后续通知消费方区分 |
| 同身份不同内容 | 冲突隔离和监控，不能覆盖已接受内容 |

联调以 MQTT 请求/回执及云端查询结果为验收依据；设备端内部实现与测试由设备团队负责。

## 12. 冻结前需设备方确认

1. 上报字段、源身份及快照水位语义能否提供；无法提供的字段明确说明。
2. detail 是否全部为 code；如含电流/位图等原始值，明确对应码及类型。
3. 初始告警定义版本及未报版本时的默认值。
4. 报文大小和状态新鲜度等接口参数。

本契约须经双方确认后冻结；云端实现不代表设备已具备源身份和原子水位能力。
