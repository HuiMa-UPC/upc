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
| `serial/serial_print.*` | 打印握手/换层（8 步命令表） |
| `serial/serial_env.*` | 环境传感器上报（A5/9B，表外） |

## 链接

```cmake
target_link_libraries(my_target PRIVATE upc::protocol)
```

## 红线

- **禁止**依赖 `glavo/`、`parser/`、`ui/`、`lvgl`、`ipc`
- **无 I/O**：不含 socket/串口 fd 操作
- 业务状态机留在 `glavo/McuLink`

## 相关

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [parser/PARSER.md](../parser/PARSER.md)
