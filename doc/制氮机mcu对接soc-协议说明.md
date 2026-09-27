# 制氮机 ↔ 主机 SoC 通信协议

本文说明 SoC 收到的是什么、怎么过来的。  
依据：`N2-APP-V2/App/Src/host_uart.c`、`N2-ESP32C3-Bridge`（`gd32_uart.c` / `n2_status_frame.c` / `ble_phone.c`）。

当前版本**只有设备 → SoC 的状态上报**，无下行命令。

---

## 1. 怎么传过去

```text
GD32 USART2                ESP32-C3                         主机 SoC
  PC10 TX ──UART 115200──►  UART1 RX(GPIO4)
  PC11 RX ◄──────────────  UART1 TX(GPIO5)
                           解析 15 字节二进制帧
                           转成 ASCII 字符串
                           ──BLE Notify──►  订阅特征值
```

| 段 | 介质 | 内容 |
|----|------|------|
| GD32 → ESP | UART 115200 8N1 | 二进制状态帧（ESP 内部用，见第 3 节） |
| ESP → SoC | **BLE GATT Notify** | **ASCII 字符串**（SoC 实际要解析的，见第 2 节） |

ESP **不是**透传二进制帧，也**不是** Wi-Fi/TCP。SoC 与设备通过蓝牙连接。

---

## 2. SoC 收到什么（BLE ASCII）

### 2.1 BLE 参数

| 项 | 值 |
|----|-----|
| 广播名 | `N2_Machine` |
| Service UUID | `0x4FFC`（16-bit） |
| Characteristic UUID | `0x1B9A` |
| 属性 | **READ + NOTIFY**（无 WRITE） |
| 载荷 | UTF-8/ASCII 文本，最长 48 字节 |

连接后需打开该特征的 CCCD（Notify），之后 ESP 每收到一帧 GD32 上报就会 Notify 一次。未开 Notify 时不推送；仍可用 READ 读当前缓存。

### 2.2 文本格式

| 条件 | 字符串示例 |
|------|------------|
| 浓度有效 | `N2=99.50% STATE=RUN seq=1` |
| 氧预热 / 浓度无效 | `N2=warming STATE=WARMUP seq=0` |
| 尚未收到过有效 UART 帧（仅 READ） | `N2=waiting` |

字段说明：

| 字段 | 含义 |
|------|------|
| `N2=` | 氮气浓度百分比，两位小数；或 `warming` / `waiting` |
| `STATE=` | `WARMUP` / `BUILD` / `RUN` / `SAFE` / `UNKNOWN` |
| `seq=` | 帧序号，对应 GD32 发出的 Seq（0～255 循环） |

格式化代码（`ble_phone.c`）：

```c
/* 无效：N2_x100 == 0xFFFF */
"N2=warming STATE=%s seq=%u"
/* 有效 */
"N2=%u.%02u%% STATE=%s seq=%u"   /* 整数部分 / 小数两位 */
```

### 2.3 SoC 建议处理

1. 扫描并连接广播名 `N2_Machine`（或按 Service `0x4FFC` 过滤）。  
2. 发现特征 `0x1B9A`，使能 Notify。  
3. 每次 Notify：按空格拆字段，解析 `N2` / `STATE` / `seq`。  
4. `N2=warming` 或 `N2=waiting` 时不要当数值显示。  
5. 约每秒一条；连续约 5 s 无 Notify 可判离线（以实际连接状态为准）。

板端确认蓝牙驱动、扫描、连接、Notify 的命令与 2026-09-18 实测记录见 [制氮机对接协议.md](doc/制氮机对接协议.md)「板端蓝牙探测与真机验证」。

---

## 3. GD32 → ESP 二进制帧（桥接内部）

SoC **收不到**这段原始帧；仅说明 ESP 从哪解析出上面的字符串。

每 **1 秒** 一帧，固定 **15 字节**，小端：

```text
5A A5 BB B0 11 <Seq> 07 20 02 <N2_lo> <N2_hi> 10 01 <state> <CheckSum>
```

| 字段 | 含义 |
|------|------|
| `BB B0 11` | 主动上报 / 制氮机 / 氮气状态 |
| N₂ | `uint16` LE = 百分比 ×100；`0xFFFF` = 预热无效 → BLE 写成 `warming` |
| state | `0` WARMUP / `1` BUILD / `2` RUN / `3` SAFE |
| CheckSum | 前 14 字节累加低 8 位 |

示例（99.50%、RUN、Seq=1）：

```text
5A A5 BB B0 11 01 07 20 02 DE 26 10 01 02 BC
```

→ BLE：`N2=99.50% STATE=RUN seq=1`

固件现状：氧未就绪报 WARMUP + 无效浓度；就绪后暂报 RUN。

---

## 4. 当前不做的事

- SoC → 设备命令（BLE 特征无 WRITE；GD32 USART2 也不解析下行）  
- Wi-Fi / TCP / MQTT 上云  
- 传感器全量、阀时序等（那些在 GD32 USART0 调试文本口，不经本 BLE 链路）
