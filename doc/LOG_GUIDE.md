# UPC 日志阅读指南

> 适用：`rongsu_ui` + `rongsu_glavo` 双进程架构  
> 源码：`common/debug/rs_log.cpp`、`upc_log_modules.hpp`  
> 消息契约：[`common/contract/README.md`](../contract/README.md)

---

## 1. 日志写到哪里

| 启动方式 | UI 日志 | Glavo 日志 |
|----------|---------|------------|
| `scripts/rongsu.sh start` | `/var/log/rongsu_ui.log` | `/var/log/rongsu_glavo.log` |
| `rongsu_ui start` / `rongsu_glavo start` | 同上（各自重定向） | 同上 |
| `scripts/rongsu.sh run` | **终端 stderr**（两进程混在一起） | **终端 stderr** |
| `rongsu_ui run` / `rongsu_glavo run` | 当前终端 | 当前终端 |

- 所有 `UPC_LOG_*` 写到 **stderr**（`start` 时被守护进程重定向到 log 文件）。
- `printf` 等 stdout 输出**不是**结构化日志，一般只有启动提示。
- **没有独立日志进程**；哪条进程打的，就看对应文件或 `proc=` 字段。

实时查看（daemon 模式）：

```bash
tail -f /var/log/rongsu_ui.log /var/log/rongsu_glavo.log
```

前台 `run` 时按进程过滤：

```bash
./scripts/rongsu.sh run 2>&1 | grep 'proc=rongsu_glavo'
./scripts/rongsu.sh run 2>&1 | grep 'proc=rongsu_ui'
```

---

## 2. 单行格式

```
时间戳 level=级别 proc=进程名 src=层级 module=模块名 消息正文…
```

**示例：**

```
2017-08-05 12:56:07.610562 level=DEBUG proc=rongsu_ui src=MW module=MSGBUS IPC_RX APP_OpenDoor_Rsp APP_GLAVO->UI pri=1 queue=0 payload={inline(1,bin) val=1}
2017-08-05 14:22:06.507732 level=INFO  proc=rongsu_glavo src=SYS module=HostMonitor cpu_sys=0.6% cpu_proc=0.1% temp=44.4C mem_avail=3759MB/3901MB rss=51MB
2017-08-05 14:22:10.123456 level=DEBUG proc=rongsu_glavo src=SVC module=McuLink SERIAL_TX frame[11] 5A A5 AA A2 40 01 04 01 00 XX
```

| 字段 | 含义 |
|------|------|
| **时间戳** | 墙钟时间，微秒精度（`YYYY-MM-DD HH:MM:SS.ffffff`） |
| **level** | `ERROR` / `WARN` / `INFO` / `DEBUG` / `TRACE` |
| **proc** | `rongsu_ui` 或 `rongsu_glavo`（`run` 同终端时靠它区分） |
| **src** | 代码分层：`MW` 中间件 / `SVC` 后端服务 / `SYS` 系统监控 |
| **module** | 子模块名（见下表） |
| **消息正文** | 自由格式，因模块而异 |

---

## 3. 模块标签速查

| module | src | 典型进程 | 内容 |
|--------|-----|----------|------|
| **MSGBUS** | MW | UI + Glavo | 进程内/跨进程消息：`IPC_TX` `IPC_RX` `IPC_DELIVER` `IPC_ENQUEUE` |
| **IPC** | MW | UI + Glavo | 套接字：listen/connect、peer 连接/断开、编解码失败 |
| **MemPool** | MW | 少用 | 内存池分配统计 |
| **BufferPool** | MW | 少用 | 缓冲区池统计 |
| **ThreadPool** | MW | 少用 | 线程池状态 |
| **McuLink** | SVC | **仅 Glavo** | TCP/串口连接、协议帧十六进制、传感器、打印握手 |
| **GlavoService** | SVC | **仅 Glavo** | 目录扫描、打印流程、PM2.5/门控等业务 |
| **ServiceBase** | SVC | **仅 Glavo** | 工作线程队列满、丢消息 |
| **CalParser** / **ParserContext** / **GcodeWorker** | SVC | **仅 Glavo** | 解析器插件 |
| **HostMonitor** | SYS | UI + Glavo | 约每 60s 一条 CPU/内存/温度 |

快速过滤：

```bash
grep 'module=MSGBUS'  /var/log/rongsu_ui.log      # UI 侧 IPC
grep 'module=McuLink' /var/log/rongsu_glavo.log # 串口/TCP
grep 'module=HostMonitor' /var/log/rongsu_*.log   # 双进程资源
```

---

## 4. 跨进程消息日志（MSGBUS / IPC）

一次 UI→Glavo→UI 往返，**两边日志对照看**。

### 4.1 阶段含义

| 阶段 | 含义 | 出现在 |
|------|------|--------|
| **IPC_TX** | 本进程经 socket **发出** | 发送方 log |
| **IPC_RX** | 本进程从 socket **收到**并已解码 | 接收方 log |
| **IPC_DELIVER** | 本进程 **投递给订阅回调**（业务处理） | 接收方 log |
| **IPC_ENQUEUE** | 消息进入本进程 **异步队列**（稍后 process 投递） | 本进程 log |
| **IPC_DELIVER miss** | 无订阅者，消息被丢弃 | 本进程 log |

### 4.2 消息行字段

```
IPC_TX UI_OpenDoor_Req UI->APP_GLAVO pri=1 queue=0 payload={empty}
```

| 片段 | 含义 |
|------|------|
| `UI_OpenDoor_Req` | 消息类型（完整表见 contract/README.md §3） |
| `UI->APP_GLAVO` | 发送方 ModuleId → 接收方 ModuleId |
| `pri=1` | 优先级（0=Low … 3=Realtime） |
| `queue=0` | 入队时队列深度（DELIVER 行则为 `handlers=N`） |
| `payload={…}` | 载荷摘要：`empty` / `inline(N)` / `value=N` / `buf(id=…)` |

### 4.3 开门示例（对照）

**UI 日志：**

```
IPC_TX   UI_OpenDoor_Req      UI->APP_GLAVO     # UI 发出请求
IPC_RX   APP_OpenDoor_Rsp     APP_GLAVO->UI     # UI 收到响应
IPC_DELIVER APP_OpenDoor_Rsp  handlers=1         # 交给 UiMsgProxy → control 更新界面
```

**Glavo 日志：**

```
IPC_RX   UI_OpenDoor_Req      UI->APP_GLAVO     # Glavo 收到请求
IPC_DELIVER …                  handlers=1         # 交给 GlavoService::handle_open_door_request
IPC_TX   APP_OpenDoor_Rsp     APP_GLAVO->UI     # Glavo 回响应
```

---

## 5. 串口 / TCP 日志（仅 Glavo · McuLink）

### 5.1 串口帧十六进制

```
SERIAL_TX frame[11] 5A A5 AA A2 40 01 04 01 00 XX
SERIAL_RX frame[24] 5A A5 BB ...
```

- 协议头：`5A A5 AA`（主机发）/ `5A A5 BB`（下位机回）
- 单条最多展示 64 字节，更长显示 `...+NB`
- 同一帧后常跟语义行，例如 `RX SENSOR OK …`、`RX LAYER event=PRINT_READY`

### 5.2 常见 TX 命令名（语义行）

| 日志关键字 | 含义 |
|------------|------|
| `PRINT_REQUEST` | 请求打印条件 |
| `PRINT_START` | 开始某层 |
| `LAYER_DONE` | 本层完成，请求下一层 |
| `PRINT_TERMINATE` | 终止打印 |
| `PRINT_FINISHED` | 正常结束 |

### 5.3 传感器上报（0x9B）

```
SERIAL_RX frame[…] 5A A5 BB …
RX SENSOR OK O2=20.1% press=1013 mbar temp=25.3C hum=45.0% PM2.5=12 powder=1 seq=42
```

周期上报，会驱动 `APP_Update_Ntf` 推送给 UI（在 MSGBUS 日志中可见）。

---

## 6. 系统监控（HostMonitor）

约 **每 60 秒** 一条，`proc=` 标明是哪个进程打的：

```
cpu_sys=0.6% cpu_proc=0.1% temp=44.4C mem_avail=3759MB/3901MB rss=51MB
```

| 字段 | 范围 | 说明 |
|------|------|------|
| **cpu_sys** | 整机 | `/proc/stat` 采样 |
| **cpu_proc** | **本进程** | `cpu_proc` 在 ui.log 是 UI，在 glavo.log 是 Glavo |
| **temp** | 整机 | SoC 温度 |
| **mem_avail / 总分母** | 整机 | 系统可用内存 / 总内存 |
| **rss** | **本进程** | 该进程常驻内存 |

---

## 7. 按场景查日志

### 7.1 UI 点了按钮没反应

1. `rongsu_ui.log` 是否有对应 `IPC_TX UI_*_Req`？
   - 无 → UI 业务未调用 `send_*`
   - 有 → 继续
2. `rongsu_glavo.log` 是否有 `IPC_RX` 同类型 + `IPC_DELIVER`？
   - 无 RX → IPC 未连通，查 `module=IPC` 的 connect/listen
   - 有 RX 无 DELIVER → Glavo 未 subscribe
3. Glavo 是否有 `IPC_TX *_Rsp`？
4. UI 是否有 `IPC_RX` + `IPC_DELIVER`？

### 7.2 IPC 连不上

```bash
grep 'module=IPC' /var/log/rongsu_*.log
```

关注：`listen failed`、`connect failed`、`peer disconnected`、`peer connected`。

### 7.3 打印卡住

```bash
grep -E 'McuLink|GlavoService|PRINT_|LAYER|SERIAL_' /var/log/rongsu_glavo.log
```

看串口握手：`PRINT_REQUEST` → `PRINT_CONDITION_OK` → `PRINT_START` → `PRINT_READY` → `LAYER_DONE` 循环。

### 7.4 环境数据不更新

- Glavo：`RX SENSOR OK`、`APP_Update_Ntf` 的 `IPC_TX`
- UI：`APP_Update_Ntf` 的 `IPC_RX` + `IPC_DELIVER`

---

## 8. 编译开关与降噪

### 8.1 总开关

```bash
cmake -B build -DUPC_DEBUG=OFF    # 关闭全部 UPC_LOG_*（零开销）
cmake -B build -DUPC_DEBUG=ON     # 默认 ON
```

### 8.2 子模块开关

默认值见 `upc_log_modules.hpp`（`UPC_DEBUG=ON` 时多为 1）。关闭某一类日志可在编译时定义，例如：

```bash
cmake -B build -DUPC_DEBUG=ON \
  -DCMAKE_CXX_FLAGS="-DUPC_MOD_MCU_LINK=0 -DUPC_MOD_MEM_POOL=0"
```

常用降噪：

| 宏 | 效果 |
|----|------|
| `UPC_MOD_MCU_LINK=0` | 关闭串口/TCP（含 `SERIAL_*`） |
| `UPC_MOD_MSG_BUS=0` | 关闭 `IPC_*` 消息流（保留 `UPC_MOD_IPC` 连接日志需单独关） |
| `UPC_MOD_IPC=0` | 关闭 IPC 连接类日志 |
| `UPC_MOD_SYS_MONITOR=0` | 关闭 HostMonitor |

---

## 9. 与消息契约的关系

- **日志里的消息名**（如 `UI_OpenDoor_Req`）与 **`msg_types.hpp` / contract 总表** 一一对应。
- 新增业务消息时，除改代码外建议在 [`contract/README.md`](../contract/README.md) §3 补一行，便于后人对照日志。

---

## 10. 限制与说明

1. **stdout 无 `proc=`**：仅 `UPC_LOG_*` 结构化；启动 banner 用 `printf`。
2. **双进程时钟**：时间戳为各进程本机墙钟，一般一致；NTP 偏差大时对照需注意。
3. **传感器日志量大**：`SERIAL_RX` + `RX SENSOR OK` 可能刷屏，调试完可关 `UPC_MOD_MCU_LINK`。
4. **大 payload**：日志只打摘要，不传完整目录列表字节；完整数据在业务层处理。

---

## 11. 相关文件

| 文件 | 作用 |
|------|------|
| `common/debug/rs_log.cpp` | 格式输出、`proc=` |
| `common/debug/rs_log.hpp` | `UPC_LOG_*` 宏 |
| `common/debug/upc_log_modules.hpp` | 模块子开关 |
| `common/debug/rs_msg_format.cpp` | MSGBUS 消息摘要格式化 |
| `common/debug/upc_sys_monitor.cpp` | HostMonitor |
| `common/process/process_config.hpp` | 日志文件路径 |
| `scripts/rongsu.sh` | `start` / `run` 启动方式 |
