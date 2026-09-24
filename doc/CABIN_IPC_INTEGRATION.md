# 舱检算法 IPC 接入约定（SOC 评审稿）

基于 `gsx` 的 `7c01d22`（协议修复）。本次补齐算法模块、Visual 收发适配与下面六条契约，**不是 SOC 已确认的协议，也不是整机起印流程已接通**。SOC 同事评审后再接发送端。唯一可编译契约是 `common/upc_contract.hpp`；本文解释调用方法。

## 1. 工作边界

- Visual：接指令、从现有 Camera DMA + SHM 取得紧凑 NV12、调用算法、稳定判断、回复、结束本轮。
- SOC / Glavo：何时检测、灯和机构准备、收到结果后的业务动作。此改动不增加开门、关门、Z 轴、打印、UI 弹窗或传感器融合代码。
- 没有请求就不跑检测。基板检测返回任意结果后停止；放板期间不自行重检，下一次检测必须由 SOC 发新请求。
- 内门是独立能力：SOC 可发一次有时限的 `WaitOpen`，Visual 在窗口内持续等稳定近黑，成功或超时后只给一个终态。不是永远运行的状态订阅。

## 2. 六条消息

| MsgType | 数值（新增，待评审） | 方向 | Inline 载荷 |
|---|---|---|---|
| `GLAVO_Visual_Cabin_Session_Start_Ntf` | `0x1720` | APP_GLAVO → APP_VISUAL | `VisualCabinSessionPayload`，4 B |
| `GLAVO_Visual_Cabin_Session_Stop_Ntf` | `0x1721` | APP_GLAVO → APP_VISUAL | 同上，4 B |
| `GLAVO_Visual_InnerDoor_Occlusion_Req` | `0x1722` | APP_GLAVO → APP_VISUAL | `VisualCabinCheckReqPayload`，16 B |
| `APP_Visual_InnerDoor_Occlusion_Rsp` | `0x1723` | APP_VISUAL → APP_GLAVO | `VisualCabinCheckRspPayload`，16 B |
| `GLAVO_Visual_Plate_Present_Req` | `0x1724` | APP_GLAVO → APP_VISUAL | 请求 POD，16 B |
| `APP_Visual_Plate_Present_Rsp` | `0x1725` | APP_VISUAL → APP_GLAVO | 回复 POD，16 B |

六条加入可靠投递白名单；这是断连后的暂存重发，不是业务执行成功确认，也不保证恰好送达一次。接收方依靠 ID 去重，调用方仍须计时。没有新 ModuleId、新 socket 或新的 Camera 取流协议。

### 字段

```cpp
struct VisualCabinSessionPayload { uint32_t session_id; };
struct VisualCabinCheckReqPayload {
    uint32_t session_id;
    uint32_t request_id;
    uint32_t timeout_ms;
    uint8_t cabin_led_mask;
    uint8_t mode;
    uint8_t reserved[2];
};
struct VisualCabinCheckRspPayload {
    uint32_t session_id;
    uint32_t request_id;
    uint32_t frame_seq;
    uint8_t result;
    uint8_t reserved[3];
};
```

- `session_id`、`request_id` 必须非零；回复原样回显。请求与回复分辨靠 MsgType，不只看 `result`。
- `timeout_ms` 范围 `250..60000`，非法值返回 `unknown`，不默默改值；算法窗口从 Visual 接受请求起计。建议基板先用 `5000`，门 `WaitOpen` 先用 `10000`，SOC 根据实机动作时长评审。
- `cabin_led_mask` 是实际六路 SOC 灯状态，只接受 `0x3f`；调用方在 Start 前开灯并保持，不能填假值。灯关／状态不全不能据黑画面判断门开。
- `mode=0`（`Snapshot`）：在短窗口内取得稳定的门／基板结论；并非只处理一帧。`mode=1`（`WaitOpen`）：仅门检测可用，清晰帧继续等待，稳定近黑返回 1，等不到返回 0。基板用 1 或任意不支持的 mode 返回 0。
- `reserved` 全部填零。错误 sender / target、非 Inline 或字节数不匹配的包忽略；可解析但字段无效的请求返回 0。
- `frame_seq` 是最后参与处理的帧序号，无帧时为 0；不能把非零帧号当成功。

### 结果

| 检测 | 0 | 1 | 2 |
|---|---|---|---|
| 内门 | `Unknown` 无法判断 | `Occluded` 稳定近黑、符合全开挡镜头特征 | `Clear` 舱内可见，**不代表机械关到位** |
| 基板 | `Unknown` 无法判断 | `QrFound` 认出约定底座码特征，SOC 解释为无板 | `QrAbsentSceneOk` 有效场景未找到约定码特征，SOC 按已验收条件解释为有板 |

`Unknown` 绝不能映射为有板／门已开；发起方超时或 Visual 掉线也按未知处理。门视觉只是图像证据，不替代机械安全互锁。

## 3. 会话、去重和停止

1. 同一 IPC 连接先 Start 再 Req；Start 只起流预热、不检测，无单独 Ack。灯全开后建议预热至少 `1500 ms` 再 Req（须实机标定）；SOC 必须继续处理 IPC 消息，不能阻塞整个消息循环。
2. 每轮推荐使用新 `session_id`，例如单调毫秒计数截为 uint32、跳过 0，并保证连续生成时递增。服务采用 uint32 串行号比较，短期内新 ID 必须比旧 ID 更新，不能同时用多个独立计数器发起。
3. 同一会话的 `request_id` 在两种检测间统一递增。重发必须使用相同 MsgType、相同两级 ID 和**完全相同的请求字段**；不要通过重发修改超时时间或任务类型。
4. 正在运行的相同请求不会启动第二个线程或延长窗口；最近完成的完全相同请求可重发缓存结果。旧 ID、改变字段、并发的另一个请求不能启动新窗口。SOC 只接受当前等待的 MsgType + 两级 ID，重复结果只消费一次。
5. 匹配 session 的 Stop 取消正在进行的检测，未完成工作不再回结果。取消后 SOC 也立即撤销等待，不应把可能已在链路中的旧回复用于新的业务。迟到的其他 session Stop 不影响当前任务。
6. 正常完成前先停检测，再回复；本轮不再需要 Camera 流时释放，其他已有视觉会话不受流释放影响。为便于重复结果去重，完成后短暂保留会话；**没有新命令就不会再检测**。空闲 `15 s` 后关闭会话，推荐 SOC 收到结果即 Stop。
7. 对 Start 的重复／较旧 ID 有重放保护。会话完全停止后 `60 s` 的保护期结束，允许新的非零 ID 恢复连接（兼容 SOC 重启）；这不是永久防重放或持久化事务保证。SOC 重启／断线后应放弃旧等待并重新建立会话，不消费旧缓存结果。
8. 舱检与首层、光路、测高互斥。若旧能力已活跃，舱检不会占用它；若旧能力后来启动，舱检停止并以 Unknown 结束未完成工作。SOC 仍应主动避免这些任务重叠。
9. SOC 从发送 Req 起设外部超时，例如 `timeout_ms + 2000 ms`（传输余量待评审）；到时无回复即未知，发送 Stop，丢弃之后迟到包。可靠重发不能代替这个超时。

## 4. SOC 发送端示例（示意，不含机构动作）

下面是三个**分别由事件调用**的发送函数，不能按顺序立即调用，否则 Stop 会取消刚发起的检测。SOC 先订阅回复并保存当前两级 ID，再执行第 1 个函数；仅预热定时器回调执行第 2 个；仅匹配的终态回复、外部超时或取消执行第 3 个。SOC 负责取消旧定时器，回调先确认仍是当前会话。

```cpp
#include "msg_port.hpp"
using namespace middleware;

// 在已有 APP_GLAVO 进程中使用其现有 MsgPort；不新开同身份客户端。
template<class T>
bool send_to_visual(MsgType type, const T& payload) {
    Message m(type, ModuleId::APP_GLAVO, ModuleId::APP_VISUAL);
    m.payload = MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    return MsgPort::instance().publish_async(m);
}

// 1. 业务事件：六灯开且场景已准备好；成功后 SOC 安排预热定时器。
bool begin_cabin_session(uint32_t new_session_id) {
    if(new_session_id == 0) return false;
    return send_to_visual(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,
                          VisualCabinSessionPayload{new_session_id});
}

// 2. 仅由预热定时器回调调用（建议 >=1500 ms 后），不堵 IPC 消息线程。
// 返回成功后 SOC 启动本轮外部超时计时；实际灯状态须在此重新取值。
bool send_plate_after_warmup(uint32_t session_id, uint32_t request_id,
                             uint8_t actual_cabin_led_mask) {
    if(session_id == 0 || request_id == 0) return false;
    VisualCabinCheckReqPayload request{};
    request.session_id = session_id;
    request.request_id = request_id;
    request.timeout_ms = 5000;
    request.cabin_led_mask = actual_cabin_led_mask;
    request.mode = static_cast<uint8_t>(CabinCheckMode::Snapshot);
    return send_to_visual(MsgType::GLAVO_Visual_Plate_Present_Req, request);
}

// 3. 仅在匹配终态回复、外部超时或取消后调用。
// SOC 先撤销等待和旧定时器，再调用；绝不紧接第 2 个函数立即执行。
bool end_cabin_session(uint32_t session_id) {
    if(session_id == 0) return false;
    return send_to_visual(MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,
                          VisualCabinSessionPayload{session_id});
}
```

以上只提供可调用的发送函数，省略 SOC 的订阅、定时器和业务状态机。回复订阅必须校验 sender、target、载荷长度、MsgType 和两级 ID；错误／迟到回复丢弃，匹配回复只消费一次。放板完成后若需再检，生成新的会话 ID，再次启动流程；检门时改用门 Req/Rsp 与 `WaitOpen`、相应有限超时。

`publish_async=true` 只说明本地接受发送，不能显示成“检测成功”；任何发送失败都应由 SOC 结束该次等待，不自动进入下一步。实际 SOC 自动发起的位置、英文状态提示、结果处理由 SOC 同事选择并评审。

## 5. 相对原需求草稿的明确变化

原 [视觉协作-内门遮挡与基板在位.md](视觉协作-内门遮挡与基板在位.md) 当时写的是 IPC 待合入。此实现把其接口建议细化为：

- Empty Start/Stop 改为 4 B session POD，避免旧 Stop 取消新任务。
- Req/Rsp 增加 `request_id`，支持一次会话多次显式检测及可靠消息去重。
- 增加门 `WaitOpen`，符合“收到门检测命令后可等待画面变黑”的逻辑；它不意味着 Visual 控制开门或全时监听。
- 只实现视觉和通信，不引入原稿中的 Z 动作、打印步骤、UI 开关、无板弹窗与 bit1 融合。它们不是本改动的附带承诺。
- 只追加六个 MsgType，不照搬其他工作区的 UI／Glavo 修改；原有数值保持。

## 6. 算法与上线限制（必须一同评审）

- 当前 `QrFound` 是对标定的底座码图形做模板／特征判定，不是二维码 payload 解码；该命名保留协议映射，不应宣传为任意二维码识读器。
- 基板需对应机器的 ROI、模板和参考分辨率配置；缺失／无效配置必须 Unknown。默认读取 `/userdata/visual/cabin_plate_config.json`，可由 `UPC_CABIN_PLATE_CONFIG` 覆盖。门同样必须有有效标定配置：默认 `/userdata/visual/cabin_door_config.json`，可由 `UPC_CABIN_DOOR_CONFIG` 覆盖；缺失／无效不使用隐式默认阈值猜测，而是 Unknown。候选门配置必须经过实机校准、明确配置／部署后才使用。
- 新鲜连续帧与质量门只能降低风险，**不能证明任意遮挡都是基板**。粉尘盖码、工具挡码、门局部遮挡均可能产生误判；已有现场单帧检查出现过半开门被判为有板，不能宣称该风险已解决。SOC 必须在场景恢复可用后发基板请求，量产前必须专门验证这些负样本。
- 黑画面仅在确认灯正常且相机正常的假设下支持门遮挡判断；相机输出有效但实际故障黑帧等情况仍需现场验证和安全设计。
- 算法不负责 Z 运动；工作距离变化会影响已有标定，不等于可以不校准。
- `gsx` 的 UPDF SCM_RIGHTS 头含 `fd_count`，相关 Broker/Camera/Visual/UI/Media 传输版本必须一致。不要把旧 `new-ui` 二进制混入这套运行环境。
- 集成测试必须使用独立 Broker socket / SHM 与模拟 Camera；不要在生产 Broker 上再注册 APP_GLAVO 或 APP_VISUAL，同 ModuleId 会挤掉正在运行的真实进程。

SOC 评审重点：确认六个消息号未被其它并行开发占用；确认字段与结果映射、WaitOpen 窗口、超时策略、会话去重规则；确认真实场景标定和负样本验收；最后才选择发送端挂载点并进行整机联调。

本次编译、自动测试以及保留的现场失败样本见 [CABIN_TEST_REPORT.md](CABIN_TEST_REPORT.md)。
