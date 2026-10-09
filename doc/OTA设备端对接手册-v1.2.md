# 设备端 OTA 对接手册

> 版本：v1.2（2026-09-16）
> 定位：面向设备端（SoC/固件团队）的 OTA 对接规范，说明设备端需要实现的 MQTT 行为与消息格式
> 依据：《设备通信协议-数据结构定义（v0.9）》§二/§八（§8.3 心跳、§8.4 检查更新）、OTA 方案 v0.6 设备端状态机
> 配套：《OTA 平台方案说明（对外展示版）》
> v1.2 变更：新增检查更新 pull 模型（`ota/check` + `ota/check_ack`，见 §3.7 与协议 §8.4）；心跳封套升 `v=3`、新增 `data.release_version` 整机版本号字段（§3.5）；整机版本「空」口径收敛为只能 null 或非空串（空串/纯空白按未上报处理）；check-update 的版本对账锚 = 服务端存的 release_version，不采纳设备 check 请求里自报的任何版本。
> v1.1 变更：清单驱动分包（Release = 多组件固件 + 差量 manifest）、两级版本（组件版本 + Release 版本）、
>   通知与下载分离（ota_notify 不再携带 URL，新增 ota/manifest/request + ota/manifest 领取链路）、
>   下载地址统一为完整预签名 HTTPS URL（downloadBaseUrl 机制废弃）、版本来源优先级（心跳库存 + manifest 请求实时校准）。

---

## 一、对接前置

| 项 | 要求 |
|---|---|
| 传输 | MQTT 5.0（paho-mqtt 2.x 兼容），QoS1 |
| Broker | EMQX（企业版 6.x），TLS 可选（8883），明文 1883 仅限本地联调 |
| 认证 | 设备 SN 作为用户名 + MQTT 密码（激活时由云端下发，见激活流程） |
| 身份 | 云端以 MQTT 认证身份（device_sn）为准，payload 中的型号/ID 仅作校验 |

**Topic 前缀**：`snowpod/{region}/{device_id}/`，`{region}` = `cn`（国内）/ `intl`（国际），`{device_id}` = 激活时分配的 `SNOWPOD-{8位大写十六进制}`。

**通信面划分（v1.1）**：
- **控制面走 MQTT**：通知、manifest 领取、进度上报、心跳全部走 MQTT；设备身份在 MQTT 连接层完成鉴权
- **数据面走 HTTPS**：固件二进制下载唯一走 HTTPS（对象存储预签名 URL，自带签名 + 时限鉴权）；设备端不做任何 URL 拼接

---

## 二、消息封套（所有消息共用）

```json
{
  "v": 1,
  "type": "ota_notify",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "req_id": "7f3a1b2c-...",
  "data": {}
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `v` | int | 是 | schema 版本，当前 `1`；字段新增时递增，接收方忽略未知字段 |
| `type` | string | 是 | 消息类型（ota_notify / ota_manifest_request / ota_manifest / ota_progress / heartbeat） |
| `device_id` | string | 是 | 设备唯一 ID（= SN） |
| `ts` | int64 | 是 | Unix 毫秒时间戳，NTP 对时后可信 |
| `seq` | uint32 | 是 | **单调递增序号**，设备重启后可归零；配合 ts 做去重与乱序判定 |
| `req_id` | string | 条件 | 心跳可省；请求-应答场景（如 manifest 请求）必须携带并原样带回 |
| `data` | object | 是 | 业务体 |

**注意**：`seq` 单调递增是乱序门闩的基础。同一次启动内必须严格递增；重启后从 1 重新开始（配合 `boot_id` 变化识别）。

---

## 三、OTA 专属消息

### 3.1 版本通知（下行）— 云端 → 设备

Topic：`snowpod/{region}/{device_id}/ota/notify`，QoS1，type=`ota_notify`

```json
{
  "v": 1,
  "type": "ota_notify",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "data": {
    "task_id": "task_38925537...",
    "release_version": "2.3.0",
    "required": false,
    "approval_status": "PENDING_APPROVAL"
  }
}
```

| 字段 | 说明 |
|---|---|
| `task_id` | **OTA 任务关联键**，一个任务多台设备时相同（一对多）。设备必须在 manifest 请求中原样带回。**不复用封套 req_id 槽位** |
| `release_version` | Release 发布版本号（两级版本的第二级，见 §三.6） |
| `required` | `true` = 强制升级（仅安全场景，跳过用户确认）；`false` = 普通升级（需用户确认后才可领取 manifest） |
| `approval_status` | 任务确认状态：`PENDING_APPROVAL`（待确认，不可领取）/ `READY`（已确认或强制更新，可领取） |

**v1.1 变更：通知不再携带固件下载 URL**。设备收到通知后，按 §3.2 流程领取 manifest 获取实际下载地址。

### 3.2 manifest 请求（上行）— 设备 → 云端

Topic：`snowpod/{region}/{device_id}/ota/manifest/request`，QoS1，type=`ota_manifest_request`

```json
{
  "v": 1,
  "type": "ota_manifest_request",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "req_id": "7f3a1b2c-...",
  "data": {
    "task_id": "task_38925537...",
    "versions": { "soc": "2.2.0", "mcu": "1.5.2" }
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `task_id` | string | 是 | 从 ota_notify 原样带回 |
| `versions` | object | 否 | 设备当前各组件版本（`{component: version}`），服务端优先用它计算差量；缺省组件回退最新心跳库存 |

**领取时机**：`required=false` 时，须在用户确认（任务进入 `APPROVED`）后才能请求；`required=true` 时收到通知即可请求。URL 过期、下载失败或重试时，重新发送本请求以获取新生成的 URL。

### 3.3 manifest 响应（下行）— 云端 → 设备

Topic：`snowpod/{region}/{device_id}/ota/manifest`，QoS1，type=`ota_manifest`

```json
{
  "v": 1,
  "type": "ota_manifest",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "req_id": "7f3a1b2c-...",
  "data": {
    "task_id": "task_38925537...",
    "release_version": "2.3.0",
    "components": [
      {
        "component": "soc",
        "version": "2.3.0",
        "url": "https://storage.example.com/snowx-firmware/soc/2.3.0.bin?X-Amz-...",
        "size_bytes": 1048576,
        "sha256": "6aba25d0c837f136fcea1e0c0c0a7a7b..."
      }
    ]
  }
}
```

| 字段 | 说明 |
|---|---|
| `task_id` | 原样带回 |
| `release_version` | Release 发布版本号 |
| `components` | **差量裁剪后的待升级组件列表**（只含需要升级的组件）；每个组件一个完整预签名 HTTPS URL |

**关键语义**：
- `components` 只包含本次需要升级的组件（设备实时版本 / 心跳库存低于目标版本的组件）；已达到目标的组件不出现，设备不下载
- URL 为**完整预签名 HTTPS 地址**（含签名与时限），**设备端不做任何拼接**；URL 在本次响应时即时生成，过期后重新发 manifest 请求获取新 URL，**不复用旧链接**
- 每个组件独立校验 `sha256`，不匹配视为该组件下载失败

**错误/结果封套**（请求的应答，同样走 `ota/manifest` topic，body 携带业务码）：
- `PENDING_APPROVAL` 状态下请求 → 返回 `TASK_NOT_APPROVED`，不生成任何 URL
- 全部组件已达到目标（无差量）→ 返回 `OTA_NO_COMPONENT_DELTA` 并带逐组件 skip 明细，不返回空 manifest（MQTT 结果码，独立于 HTTP 的 `TASK_NO_TARGET`，后者仅用于创建任务时零目标设备）
- 任务不存在或非本设备任务 → `TASK_NOT_FOUND`
- 任务已取消/不可用 → `TASK_NOT_READY`
- 未上报版本的组件（实时版本与心跳库存都没有）→ 视为未知，必须进入差量（按需升级），不得误判为已达目标

### 3.4 进度上报（上行）— 设备 → 云端

Topic：`snowpod/{region}/{device_id}/ota/progress`，QoS1，type=`ota_progress`

```json
{
  "v": 1,
  "type": "ota_progress",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "data": {
    "task_id": "task_38925537...",
    "component": "soc",
    "state": "downloading",
    "progress_pct": 45,
    "error_code": null
  }
}
```

`state` 枚举：`downloading` / `verifying` / `applying` / `completed` / `failed` / `rolled_back`

| 字段 | 类型 | 说明 |
|---|---|---|
| `task_id` | string | 原样带回（见 §六.1） |
| `component` | string | 组件类型（soc / mcu / galvo），多组件升级时逐组件上报 |
| `state` | string | 状态枚举，见上 |
| `progress_pct` | int（0–100） | 下载/应用进度；非进度阶段可省略 |
| `error_code` | string\|null | 失败原因码；进行中/成功为 null |

**v1.1 变更**：原 `firmware_type` 字段更名为 `component`（组件语义，取值 soc/mcu/galvo）。

| 状态 | 时机 |
|---|---|
| `downloading` | 开始下载 / 下载进度（progress_pct） |
| `verifying` | 下载完成，开始 SHA-256 校验 |
| `applying` | 校验通过，写入固件分区 |
| `completed` | **组件自认刷完**（即将/已重启）——注意：这不是云端终态，见 §四 |
| `failed` | 任一环节失败（error_code 可附原因） |
| `rolled_back` | 回滚完成（回到上一版本） |

**硬性要求**：`task_id` 必须原样带回（云端按 `(task_id, device_id, component)` 定位该台设备该组件的任务行）。

### 3.5 心跳（上行）— 设备 → 云端

Topic：`snowpod/{region}/{device_id}/heartbeat`，QoS1，type=`heartbeat`。默认周期 30 分钟，**上线时也发一次**。

```json
{
  "v": 3,
  "type": "heartbeat",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "boot_id": "b8f3a1c2-...",
  "data": {
    "uptime_s": 86400,
    "release_version": "2.3.0",
    "firmwares": [
      { "firmware_type": "soc", "version": "2.2.5" },
      { "firmware_type": "mcu", "version": "1.5.2" },
      { "firmware_type": "galvo", "version": "1.4.0" }
    ],
    "battery": 87,
    "network": { "type": "wifi", "rssi": -58 }
  }
}
```

| 字段 | 层级 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| `v` | 封套 | int | 是 | 心跳 schema 版本，当前 `3`（见下方演进表） |
| `boot_id` | 封套 | string | 是 | **本次启动唯一 ID，每次重启重新生成**（乱序门闩的关键）；**v2 起从 `data` 迁到封套顶层** |
| `ts` / `seq` | 封套 | — | 是 | 同 §二；`seq` 同一次启动内单调递增 |
| `data.release_version` | data | string/null | 条件 | **整机版本号**（= Release 版本，§3.6），心跳 `v3` 新增，check-update 的版本对账锚。**只能 null 或非空串，空串/纯空白一律按未上报处理** |
| `data.uptime_s` | data | int | 是 | 本次启动后运行秒数 |
| `data.firmwares` | data | array | 是 | 各组件版本清单 `[{firmware_type, version}]`，**必须包含全部组件的最新版本** |
| `data.battery` | data | int/null | 否 | 电量百分比（升级能力检查用） |
| `data.network` | data | object | 否 | 网络摘要 `{type, rssi}`（下载能力检查用） |

**心跳 schema 版本演进（封套 `v` 字段）**：

| `v` | 变化 |
|---|---|
| `1` | `boot_id` 在 `data` 内 |
| `2` | `boot_id` 迁到封套顶层（`envelope.boot_id`） |
| `3` | `data` 顶层新增 `release_version`（整机版本号） |

云端消费端当前 v1/v2/v3 双读兼容（迁移窗口）；设备端**新固件按 `v=3` 发**，才能让 `release_version` 生效、check-update 可用。

**心跳的三重角色（v1.2）**：
1. **服务端版本库存**：心跳版本持久化到 `device_ota_status.versions`，用于任务创建时预筛、灰度筛选、manifest 差量兜底
2. **升级终证据**：升级完成后，设备重启，新 boot_id 的心跳回带**目标组件的新版本**——云端比对 `version == 任务快照 target` 才判定该组件升级完成（ACTIVE）。**这是唯一终证据**，progress 的 `completed` 不算。Release 全部组件到位才判定整个发布完成
3. **整机版本上报**：`release_version`（data 顶层）持久化到 `device_ota_status.release_version`，是 check-update（§3.7）选定下一步版本的**唯一对账锚**；不报或报空 → check 返回 `OTA_VERSION_NOT_READY`（未就绪重试）

**字段映射说明**：心跳 `firmwares[].firmware_type` 沿用协议字段名，其取值即组件类型（soc/mcu/galvo），与 manifest/progress 的 `component` 字段一一对应。

### 3.6 版本模型（v1.1）— 两级版本

- **第一级：组件版本**——每个组件独立演进，语义化版本号（X.Y.Z）：
  - `soc: 2.3.0`，`mcu: 1.5.2`，`galvo: 1.4.0`
- **第二级：Release 发布版本**——代表"这一组组件组合经过测试验证"：
  - `Release 2.3.0 = { soc 2.3.0, mcu 1.5.2, galvo 1.4.0 }`
  - `Release 2.3.1 = { soc 2.3.1, mcu 1.5.2, galvo 1.4.0 }`（只改了 soc）
- 设备对外（APP、云平台、售后）暴露 Release 版本；出问题定位时用组件版本组合
- 一个 Release 关联 1~N 个组件（最少 1 个，最多组件枚举数），同一组件类型最多一个

**版本号格式（v1.1 定稿）**：多段数字点分，段数不限（如 `1.0.1`、`1.0.1.0.2`），每段纯数字；可选展示前缀 `v/V` 仅展示层使用，数据面/上传/比较一律纯数字。比较按段逐段数字比较，缺段补 0（`1.0` 与 `1.0.1` 可比）。**尾随 0 段视为等价**（`1.0.1` == `1.0.1.0`），这是补 0 比较的固有语义：版本去重、幂等复用判定、CONFLICT 冲突检测一律按规范化后（补 0 逐段）比较，不得用字符串精确比对。

**未知版本（v1.1）**：设备未识别的组件版本上报 `unknown`（心跳 `firmwares[].version` 或 manifest 请求 `versions` 均可）。云端将 `unknown` 视为版本未知，与未上报同等处理：进差量（需升级），不判为已达目标，也不判为格式不可比。

**版本来源优先级（v1.1 定稿）**：
1. manifest 请求携带的实时 `versions`（最高优先，当场新鲜度）
2. 心跳库存 `device_ota_status.versions`（兜底）
3. 都没有（或为 `unknown`）→ 视为未上报，必须进差量（需升级），不得误判已达目标

**组件枚举**：`soc` / `mcu` / `galvo`（业务组件类型；与协议遥测类型 motion/safety 不混用）。

### 3.7 检查更新（上行 pull 模型）— 设备主动拉取

与 §3.1 的定向推送（notify → manifest）互补：设备上线/定时**主动问**「我该升到哪个版本」，云端直接回下一步升级清单（含预签名 URL）。设备无论是否收到 notify 都可发起 check。check 一次拿全下载清单，**无需再走 manifest 请求**；notify 驱动的路径仍需 manifest request → manifest 领取。

**请求（上行）**：Topic `snowpod/{region}/{device_id}/ota/check`，QoS1，type=`ota_check`

```json
{
  "v": 1,
  "type": "ota_check",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12345,
  "req_id": "7f3a1b2c-...",
  "data": {}
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `req_id` | string | 是 | 请求关联键，应答原样带回 |
| `data` | object | 是 | **恒为空 `{}`**。版本对账锚用服务端已存的整机版本 `device_ota_status.release_version`（心跳 §3.5 上报），**不采纳 check 请求里自报的任何版本** |

**应答（下行）**：Topic `snowpod/{region}/{device_id}/ota/check_ack`，QoS1，type=`ota_check_ack`

① 有更新（`data` 携带下一步升级清单）：

```json
{
  "v": 1,
  "type": "ota_check_ack",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12346,
  "req_id": "7f3a1b2c-...",
  "data": {
    "next_release_version": "1.5.0",
    "components": [
      { "component": "soc", "version": "2.3.0", "url": "https://...", "size_bytes": 1048576, "sha256": "abcd..." }
    ]
  }
}
```

② 已最新（`error` 封套，`data` 为 null）：

```json
{
  "v": 1,
  "type": "ota_check_ack",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12346,
  "req_id": "7f3a1b2c-...",
  "error": { "code": "OTA_NO_UPDATE", "message": "no update available" }
}
```

③ 未就绪重试（`error` 封套，`data` 为 null）：

```json
{
  "v": 1,
  "type": "ota_check_ack",
  "device_id": "SNOWPOD-A1B2C3D4",
  "ts": 1753228800000,
  "seq": 12346,
  "req_id": "7f3a1b2c-...",
  "error": { "code": "OTA_VERSION_NOT_READY", "message": "device version not ready, retry later" }
}
```

**三态语义**：

| 态 | 判定 | 设备端动作 |
|---|---|---|
| 有更新 | `data.next_release_version` 非空 | 按 `data.components` 下载升级（下载校验同 §3.3/§五） |
| 已最新 | `error.code = OTA_NO_UPDATE` | 无更高版本，不动 |
| 未就绪重试 | `error.code = OTA_VERSION_NOT_READY` | 整机版本还没落库（心跳未到，或 release_version 是空串被归一化）。先确保心跳带 `release_version` 上报成功，稍后重发 check |

**`next_release_version` 是「下一步该升到的版本」，不是最新版**：云端按下发策略选举下一步——存在必经版本（升级关卡）时优先返回必经版本，可能低于更高版本号。设备升完可再次 check 继续往上走（每次只回下一步）。

---

## 四、设备端状态机

```
Idle → Notified → (AwaitingApproval)* → ManifestFetched → Downloading
     → Downloaded → Verifying → Verified → Upgrading → Upgraded → Rebooting
     → Active（心跳回带新版本确认）

任何阶段失败：
     DownloadFailed / VerifyFailed / UpgradeFailed
     → RollingBack → RolledBack（回滚到上一版本）
```

| 阶段 | 设备行为 | 上报 |
|---|---|---|
| Idle | 空闲，等通知 | — |
| Notified | 收到 ota_notify | — |
| AwaitingApproval* | `required=false` 且 `approval_status=PENDING_APPROVAL`：等待用户确认，不请求 manifest（`required=true` 跳过此阶段） | — |
| ManifestFetched | 请求并收到 ota_manifest，拿到差量组件列表与预签名 URL | manifest/request（req_id 关联） |
| Downloading | 从 url 逐组件下载，定期报进度 | `downloading` + progress_pct |
| Verifying | SHA-256 校验 | `verifying` |
| Upgrading | 写入固件分区（升级顺序由设备端自行决定，先子板后主控为推荐顺序） | `applying` |
| Rebooting | 刷写完成，重启 | `completed`（自认刷完） |
| Active | 重启后心跳回带新版本 | `heartbeat` + 新版本（云端判终态） |
| RolledBack | 失败回滚完成 | `rolled_back` |

---

## 五、下载与校验

- 下载地址：**完整预签名 HTTPS URL**（对象存储/MinIO 生成，含签名与时限），由云端在 `ota_manifest` 响应时即时生成。设备端直接使用，**不做任何拼接、不进行 RFC 3986 相对解析**
- **v1.1 变更**：废弃 `downloadBaseUrl` 机制（v1.0 的待定项），固件记录只保存 `bucket/objectKey`，URL 由平台动态生成
- URL 时效：从生成时刻起算（默认 1 小时，可配）；**过期后重新发送 manifest 请求获取新 URL，不复用旧链接**
- **必须校验（当前阶段）**：下载完成后计算 SHA-256，与 `components[].sha256` 比对，不一致 → 报该组件 `verifying` 失败
- **签名校验（待启用）**：整包/组件签名是设计要求，签名算法/编码/签名对象/信任根**尚未在协议层定义（待协议 v0.10）**，当前阶段以 SHA-256 完整性校验为准；协议定义后启用
- 下载失败可重试（设备端实现重试上限，不无限重试）

---

## 六、关键语义约束（设备端必须遵守）

1. **task_id 一对多、原样带回**：同一个 OTA 任务下发的多台设备收到相同 task_id；manifest 请求/进度上报时必须原样带回，云端按 `(task_id, device_id)`（进度按 `(task_id, device_id, component)`）定位任务行。task_id 与封套 req_id 职责不同，各用各的。
2. **seq 单调 + boot_id 重生**：同一次启动内 seq 严格递增；每次重启 boot_id 重新生成、seq 归 1。云端用这两者做乱序门闩（防旧消息覆盖新状态），不遵守会导致状态回跳。
3. **completed 不是终点**：progress 报 completed 后必须重启，重启后的心跳才构成升级完成的证据。如果重启后心跳仍报旧版本（升级未生效），云端判定升级未完成（保持待确认 → 超时判失败）。
4. **required=true 的边界**：仅安全场景使用，**跳过用户确认**；设备端执行前须能力检查（电量/网络）；固件签名校验为设计要求（当前以 SHA-256 完整性校验为准），不得因 required=true 跳过完整性校验直接安装。
5. **身份一致性**：上行 payload 的 model/device_id 必须与注册信息一致，不一致云端会告警并拒绝参与灰度匹配。
6. **禁止并行 OTA**：同一设备同一时刻只允许一个进行中的升级任务；收到新任务时若已有任务在执行，应拒绝/排队，不得并发刷写。
7. **多组件（多 firmware_type）注意**：一台设备可能同时升级 soc 与 mcu；各组件独立判定终态，心跳的 `firmwares` 列表必须包含全部组件的最新版本。Release 全部组件到位才判定整个发布完成。
8. **差量语义（v1.1）**：manifest 只返回需升级组件；已达到目标的组件不下载；全部组件已达目标时服务端返回 `OTA_NO_COMPONENT_DELTA`（不是空 manifest，也不同于创建任务时的 `TASK_NO_TARGET`）。未上报版本的组件按需升级（进差量），不跳过。
9. **下载地址时效（v1.1）**：预签名 URL 有时限，过期重拉 manifest 重新生成；不得把旧 URL 缓存在设备端长期复用。
10. **check-update 的版本锚在服务端（v1.2）**：check 请求 `data` 恒空、不要携带版本号；云端只用服务端已存的 `release_version` 选下一步。设备端要做的是先确保心跳把 `release_version` 上报成功，再发 check。
11. **release_version 只能 null 或非空串（v1.2）**：上报时填语义化数字版本号（§3.6）；无版本能力时不带该字段或填 null，**禁止填空串/纯空白**（会被归一化为未上报）。库侧已加 DB 硬约束，空串入库会被拒。

---

## 七、最小实现清单

| # | 项 | 必/选 |
|---|---|---|
| 1 | MQTT 连接 + 认证（SN + 密码，QoS1） | 必 |
| 2 | 订阅 `snowpod/{region}/{sn}/ota/notify`，解析 ota_notify（含 approval_status） | 必 |
| 3 | 发布 `ota/manifest/request`（携带 task_id + 可选 versions），订阅 `ota/manifest` 解析差量组件与预签名 URL | 必 |
| 4 | 按 manifest 逐组件 HTTPS 下载，校验 SHA-256 | 必 |
| 5 | 发布 `ota/progress`：downloading/verifying/applying/completed/failed/rolled_back（携带 component） | 必 |
| 6 | 发布 `heartbeat`：上线时 + 周期（默认 30min），带 boot_id/firmwares（全部组件） | 必 |
| 7 | seq 单调维护 + 重启时 boot_id 重生 + seq 归 1 | 必 |
| 8 | task_id 原样带回 | 必 |
| 9 | 用户确认交互（`required=false` 时，等待 APPROVED 后再请求 manifest） | 必（C 端确认路径） |
| 10 | 升级失败回滚（RollingBack → RolledBack） | 选（双分区就绪后必） |
| 11 | 固件签名校验（signature） | 待协议 v0.10 定义后启用（当前 SHA-256 完整性校验已覆盖） |
| 12 | 心跳 `v=3`：封套 `v=3`、`boot_id` 在封套顶层、`data` 带 `release_version`（非空串） | 必（check-update 前提） |
| 13 | 发布 `ota/check`（`data` 恒空、带 `req_id`），订阅 `ota/check_ack` 解析三态 | 必（pull 入口） |
| 14 | check 返回 `OTA_VERSION_NOT_READY`：先补报心跳 `release_version`，稍后重发 check | 必 |

---

## 八、常见问题排查

| 现象 | 排查方向 |
|---|---|
| 收不到 ota_notify | ① 设备是否命中灰度（bucket < percent）② 订阅 topic 是否正确（region/sn 拼写）③ MQTT 连接认证是否通过 |
| manifest 请求返回 `TASK_NOT_APPROVED` | 任务处于 `PENDING_APPROVAL`，用户未确认；确认后再请求 |
| manifest 请求返回 `OTA_NO_COMPONENT_DELTA` | 全部组件已达到目标版本（看 skip 明细）；检查设备上报版本与 Release 组件版本 |
| manifest 里缺某个组件 | 该组件版本已达到目标（差量裁剪），属正常行为 |
| 下载失败 / 下载 403 | 预签名 URL 过期（重新发 manifest 请求获取新 URL）或签名 host 与下发 URL 不一致 |
| progress 上报后任务状态不前进 | ① task_id 是否原样带回 ② seq 是否单调（被乱序门闩丢弃）③ component 是否与 manifest 一致 |
| 升级完成但云端判定未完成 | 重启后心跳的 firmwares 里目标组件的 version 是否已更新；boot_id 是否重新生成 |
| 心跳被丢弃 | ① seq 小于已记录值（乱序）② boot_id 相同但 seq 回跳 ③ ts 早于已有记录 |
| 强制升级不执行 | required=true 未通过能力检查 / SHA-256 完整性校验，或电量/网络能力检查不满足 |
| check 一直返回 `OTA_VERSION_NOT_READY` | 心跳没把 `release_version` 上报成功：查心跳封套 `v` 是否 ≥3、`boot_id` 是否在封套顶层、`release_version` 是否非空串 |
| check 返回的版本不是最新版 | 正常：`next_release_version` 是「下一步该升的版本」（必经优先），升到后再 check 继续走 |
| check 返回 `OTA_NO_UPDATE` 但明明有更高版本 | 设备 productModel 与 release 不匹配，或 release 未 PUBLISHED；确认型号与发布状态 |

---

## 九、联调工具

本地联调用 `tools/device/mqtt_client.py` 模拟设备端（凭据从 `.device_fixture.json` 读取）：

```bash
python mqtt_client.py --sn SNOWPOD-XXXXXXXX              # 模拟一台设备上线
python mqtt_client.py --sn X --heartbeat-sec 10          # 短心跳周期（测试用）
python mqtt_client.py --sn X --firmware soc=1.0.0        # 指定固件版本
python mqtt_client.py --sn X --ota-skip-upgrade          # 模拟"刷写不生效"（构造误判场景）
```

模拟器收到 ota_notify 后自动走状态机（确认→拉 manifest→下载→校验→刷写→completed→重启心跳），支持强制升级确认与失败注入，是端到端联调的标准手段。
