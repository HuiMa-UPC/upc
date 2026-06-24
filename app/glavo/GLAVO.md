# glavo/ — Glavo 进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_glavo`** · 库 **`upc::glavo`**（CMake target `upc_glavo`）

## 文件职责

| 文件 | 职责 |
|------|------|
| `main.cpp` | 进程守护、IPC Server、McuLink 连接、`MsgPort::process` 循环 |
| `lib/glavo_service.*` | IPC 业务 + 工作线程 + 订阅/发布（目录/打印/门控/进度） |
| `lib/mcu_link.*` | TCP 运动下发 + 串口握手/传感器/换层 Snap 触发 |
| `lib/glavo_parser_bridge.*` | `ParserManager` RingBuffer → `McuLink` |
| `lib/print_video_notify.*` | 换层 Snap / 录像会话 → Camera（经 UI Hub） |
| `lib/print_job_state.*` | 发送包计数、完成/错误标志（McuLink 与进度共用） |
| `lib/device_config.hpp` | TCP/串口默认、Layer Snap 开关 |

解析库见 [parser/PARSER.md](../parser/PARSER.md)；线协议见 [protocol/PROTOCOL.md](../protocol/PROTOCOL.md)。

## 生产链路

```
UI → GlavoService（工作线程）→ ParserManager → RingBuffer → McuLink → 下位机
```

## 初始化（`main.cpp`）

1. `set_mode_ipc_server(APP_GLAVO)` + `MsgPort::init`
2. `GlavoService::init/start`
3. `McuLink::connect_tcp` + `open_serial` + `start_link_thread`
4. 主循环 `port.process(2)`

## IPC 处理

| 消息 | 处理 |
|------|------|
| `Fs_DirOpen_Req` | 按 `ParserRegistry` 后缀列目录 |
| `UI_FileInfo_Req` | 打开文件、预解析行数 |
| `UI_Print_Start_Req` | `begin_sending` + `PrintVideoNotify::on_print_start` |
| `UI_Print_Terminate_Req` | `stop_parse` + `request_terminate` + `on_print_abort` |
| `UI_PM25_Req` / 门控 Req | 环境快照或占位 Rsp |

跨进程回复 UI：**必须** `publish_async`（`GlavoService` 工作线程内禁止 `MsgPort::publish` 同步路径）。

### 打印层录像（Glavo → Camera）

开关：`GLAVO_ENABLE_LAYER_SNAP`（`device_config.hpp`，默认 `1`）。

| 阶段 | 调用点 | IPC |
|------|--------|-----|
| 开始 | `GlavoService` 打印启动 | `APP_Print_Video_Session_Start_Ntf` |
| 每层 | `McuLink::link_try_layer_serial_dispatch` | `APP_Print_Layer_Snap_Ntf` |
| 末层（无尾 M3000） | `run_natural_complete_cleanup` | `APP_Print_Layer_Snap_Ntf`（防重复） |
| 完成 | `PrintCompleted` | `APP_Print_Video_Session_Finish_Ntf` |
| 中止 | 终止 / `PrintError` | `APP_Print_Video_Session_Abort_Ntf` |

`PrintVideoNotify::publish_to_camera`：`publish_async`，`target_id = APP_CAMERA`，经 [common/ipc/IPC.md](../../common/ipc/IPC.md) Hub 转发。

## 红线

- 禁止 `GcodeWorker::instance()` 等具体解析器；用 `ParserManager::active()`
- 禁止链接 `ui::port`、lvgl
- 新切片格式：[parser/PARSER.md](../parser/PARSER.md)

## 日志

- McuLink：`module=McuLink`，开关 `UPC_MOD_MCU_LINK`
- 详见 [doc/LOG_GUIDE.md](../../doc/LOG_GUIDE.md)

## 相关文档

- [ui/UI.md](../ui/UI.md) · [camera/CAMERA.md](../camera/CAMERA.md) · [common/ipc/IPC.md](../../common/ipc/IPC.md)
