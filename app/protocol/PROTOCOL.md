# protocol/ — MCU 线协议库

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 命名空间：无（`extern "C"` API）· 独立静态库 **`upc_protocol`** / `upc::protocol`

## 权威文档

| 通道 | 帧头 | 协议表 |
|------|------|--------|
| TCP 以太网 | `BE EF` | [doc/SOC-_MCU TCP报文.xlsx](../../doc/SOC-_MCU%20TCP报文.xlsx) |
| 串口 | `5A A5` | [doc/SOC_-_MCU串口通信协议.xlsx](../../doc/SOC_-_MCU%E4%B8%B2%E5%8F%A3%E9%80%9A%E4%BF%A1%E5%8D%8F%E8%AE%AE.xlsx) |

## 目录

| 路径 | 说明 |
|------|------|
| `tcp/tcp_frame.*` | RX 分帧（Length 字段，总长 = 3 + Length） |
| `tcp/tcp_tx.*` | SOC→MCU 组包（pkg_num + 11B 子包） |
| `tcp/tcp_rx.*` | MCU→SOC 应答解析 |
| `serial/serial_frame.*` | 串口公共帧格式、校验、AA 组帧、BB/BA 分帧 |
| `serial/serial_ack.*` | AB/BA ACK 组包与匹配 |
| `serial/serial_print.*` | 打印握手/换层（8 步命令表）+ 自动转运请求/结果（A2/0x41，TLV 0x02 请求 / 0x04 结果） |
| `serial/serial_env.*` | 环境传感器上报（A5/9B，TLV 0x02） |
| `serial/serial_debug.*` | 调试页手动控制：三轴/设备/粉桶/三通/风扇/开关/清粉 组帧 + 运动位置上报（A5/9B，TLV 0x05）解析 |
| `serial/serial_project_info.*` | 工程版本查询/上报（A2/0x10，TLV 0x01 查询 / 0x02 上报）：MCU 固件版本 + 激光器/Z 电机 + 运行槽 `value[5]` + OTA 状态 `value[6]` |
| `serial/serial_ota.*` | MCU 串口 OTA 组帧/应答解析（A2/0x30 请求，A6/0x31 数据与收尾）；停等与阶段四见 [MCU_OTA丢包重传机制.md](../../doc/MCU_OTA丢包重传机制.md) |
| `serial/serial_alarm.*` | 报警：单条上报解析（A3/0x20，TLV 0x01 len=9）+ 快照分片解析（A3/0x81，TLV 0x04 len=11）+ 复位请求组帧（A3/0x82，TLV 0x01 len=2）；协议见 [doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md](../../doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md)。快照只解一帧，攒帧与对齐在 UI 侧 |

## 链接

```cmake
target_link_libraries(my_target PRIVATE upc::protocol)
```

## 红线

- **禁止**依赖 `glavo/`、`parser/`、`ui/`、`lvgl`、`ipc`
- **无 I/O**：不含 socket/串口 fd 操作
- 业务状态机留在 `glavo/McuLink`

## TCP CMD=0x0A 编码口径（重点）

`tcp/tcp_tx.*` 里 `CMD=0x0A` 的 `payload[0..2]` 必须按协议枚举值原样写入，不得 bool 化：

- `payload[0]`（Stop）：`1=急停，0=不修改`
- `payload[1]`（Pause）：`2=暂停，1=继续，0=不修改`
- `payload[2]`（Red）：`3=红光常亮，2=激光，1=红光，0=不修改`

注意：

- 禁止写法：`payload[i] = code ? 1u : 0u;`
- 正确写法：`payload[i] = code;`

否则会把 `2` 误压为 `1`，导致“激光/暂停”语义失真。

## 相关

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [parser/PARSER.md](../parser/PARSER.md)
