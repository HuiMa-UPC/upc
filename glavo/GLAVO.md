# glavo/ — Glavo 进程

> 索引：[doc/ARCHITECTURE.md](../doc/ARCHITECTURE.md)  
> 可执行文件：`rongsu_ui` 的对端 **`rongsu_glavo`** · 库 **`upc_glavo`**

## 职责

| 组件 | 路径 | 说明 |
|------|------|------|
| `GlavoService` | `lib/glavo_service.*` | IPC：目录/打印/门控/进度 |
| `McuLink` | `lib/mcu_link.*` | TCP 运动下发 + 串口握手/传感器 |
| `protocol` | [`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md) | 线协议编解码（`upc::protocol`，无 I/O） |
| `ServiceBase` | `lib/service_base.*` | 工作线程 + MsgBus |
| `glavo_parser_bridge` | `lib/glavo_parser_bridge.*` | RingBuffer → McuLink |
| 解析 | [`parser/PARSER.md`](../parser/PARSER.md) | 独立库，禁止在 Glavo 里 `#include` 具体 worker |

## 生产链路

```
UI → GlavoService（工作线程）→ ParserManager → RingBuffer → McuLink → 下位机
```

## 初始化（`main.cpp`）

1. `set_mode_ipc_server(APP_GLAVO)` + `MsgPort::init`
2. `GlavoService::init/start`
3. `McuLink::connect_tcp` + `open_serial` + `start_link_thread`
4. 主循环 `port.process(2)`

## 打印相关 IPC

| 消息 | 处理要点 |
|------|----------|
| `UI_Print_Start_Req` | `McuLink::begin_sending()` |
| `UI_Print_Terminate_Req` | `stop_parse` + `request_terminate` |
| `Fs_DirOpen_Req` | 按 `ParserRegistry` 后缀过滤 |

## 红线

- 禁止 `GcodeWorker::instance()` 等具体解析器；用 `ParserManager::active()`
- `handle_message` 在工作线程：**必须** `publish_async` 回 UI
- 新切片格式：见 [parser/PARSER.md](../parser/PARSER.md)

## 日志

- McuLink：`module=McuLink`，开关 `UPC_MOD_MCU_LINK`
- 详见 [doc/LOG_GUIDE.md](../doc/LOG_GUIDE.md)

## 相关文档

- [ui/UI.md](../ui/UI.md) · [common/ipc/IPC.md](../common/ipc/IPC.md) · [common/middleware/MIDDLEWARE.md](../common/middleware/MIDDLEWARE.md)
