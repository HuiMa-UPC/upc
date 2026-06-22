# 项目架构索引（单一真相）

> Agent / 开发者改模块前先查此表。细节见各模块 `*.md`，不要在此重复长文。

## 进程

| 进程 | 可执行文件 | 入口 | 模块文档 | 日志 |
|------|------------|------|----------|------|
| UI | `rongsu_ui` | `app/ui/main.cpp` | [ui/UI.md](../app/ui/UI.md) | `/var/log/rongsu_ui.log` |
| Glavo | `rongsu_glavo` | `app/glavo/main.cpp` | [glavo/GLAVO.md](../app/glavo/GLAVO.md) | `/var/log/rongsu_glavo.log` |
| 相机 | `rongsu_camera` | `app/camera/main.cpp` | [camera/CAMERA.md](../app/camera/CAMERA.md) | `/var/log/rongsu_camera.log` |
| 相机测试 | `test_camera` | `test/camera/main.cpp` | [test/TEST.md](../test/TEST.md) | `/var/log/test_camera.log` |

启动：`scripts/rongsu.sh start`（先后端、再 UI）。  
编译：`./build.sh`（Release + 工具链，产物在 `output/`；`clean` / `reconfigure` 见脚本头注释）。  
注意：测试伪进程和真实进程互斥，比如相机进程和相机测试进程只能启动一个。

## 顶层目录

| 目录 | 文档 | 职责 |
|------|------|------|
| `app/` | — | 应用层进程与业务库（见下表）；构建入口 `app/CMakeLists.txt` |
| `test/` | [TEST.md](../test/TEST.md) | 独立测试进程（camera 等） |
| `common/` | [COMMON.md](../common/COMMON.md) | 契约、middleware、ipc、debug、process |
| `3rdparty/` | — | LVGL 等第三方依赖 |

### `app/` 子目录

| 目录 | 文档 | 职责 |
|------|------|------|
| `app/ui/` | [UI.md](../app/ui/UI.md) | LVGL 界面、UiMsgProxy、IPC Hub 客户端 |
| `app/glavo/` | [GLAVO.md](../app/glavo/GLAVO.md) | 打印编排、McuLink、IPC Server |
| `app/camera/` | [CAMERA.md](../app/camera/CAMERA.md) | 相机采集、SHM 预览、IPC Server |
| `app/parser/` | [PARSER.md](../app/parser/PARSER.md) | G-code/OVF 解析库（Glavo 链接） |
| `app/protocol/` | [PROTOCOL.md](../app/protocol/PROTOCOL.md) | MCU 线协议（TCP `BE EF` + 串口 `5A A5`） |

## Glavo 库（`app/glavo/lib/`）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `GlavoService` | `glavo_service.*` | IPC 业务编排（打印/目录/门控） |
| `McuLink` | `mcu_link.*` | TCP 运动 + 串口握手/传感器 |
| `ServiceBase` | `service_base.*` | MsgBus 生命周期 + 工作线程 |
| `PrintJobState` | `print_job_state.*` | 打印任务计数/状态 |
| `PrintVideoNotify` | `print_video_notify.*` | 换层 Snap / 录像会话 IPC → Camera |

## Camera 库（`app/camera/lib/`）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `CameraService` | `camera_service.*` | IPC Server + SHM 推流 |
| `PrintVideoRecorder` | `print_video_recorder.*` | SHM 取帧 + MPP H.264 + ffmpeg MP4 |
| `ViCapture` | `vi_capture.*` | rkaiq + V4L2 NV12 |

## UI core 预览（`app/ui/lib/core/`）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `CameraPreviewPlayer` | `camera_preview.*` | SHM 元数据环 + dma fd → RGA/SW BGRX → LVGL |
| `CameraMsgProxy` | `camera_msg_proxy.*` | 预览开停 IPC |
| `RgaDmaBuf` | `rga_dma_buf.*` | dma32 heap 缓冲（RGA fd 导入） |

## protocol/ 库（`app/protocol/`）

| 子目录 | 协议表 | 说明 |
|--------|--------|------|
| `app/protocol/tcp/` | `doc/SOC-_MCU TCP报文.xlsx` | 运动/心跳/复位组包，RX 背压解析 |
| `app/protocol/serial/` | `doc/SOC_-_MCU串口通信协议.xlsx` | 打印握手 + 环境上报 |

## 数据流（打印）

```
UI → IPC → GlavoService → ParserManager → RingBuffer
                              ↓
                         McuLink → TCP（运动）+ 串口（握手/传感器）
                              ↓（换层排空后，可选 GLAVO_ENABLE_LAYER_SNAP）
                    PrintVideoNotify → IPC(UI Hub FWD) → Camera → MP4
```

预览像素：后端（`rongsu_camera` / `test_camera`）→ 主 IPC SCM_RIGHTS 传 dma_buf fd + SHM `/upc_camera_preview` 元数据环 → UI `CameraPreviewPlayer`（RGA `importbuffer_fd` 或 SW 转 BGRX；与控制 IPC 分离）。

## 跨模块红线

- UI **禁止**链接 `app/glavo/lib`、`app/parser/` 业务库
- Glavo **禁止**链接 `lvgl`
- test **禁止**链接 `app/ui`、`app/glavo` 业务库
- 跨进程回复 UI：**必须** `publish_async()`，目标 `ModuleId::UI`
- 消息契约：**只改** `common/README.md` + `msg_types.hpp`（四步流程）
- MCU 协议实现：`app/protocol/`（`upc::protocol`）+ `mcu_link.*`；权威表见 `doc/*.xlsx`

## 文档与 Agent 入口

| 类型 | 路径 |
|------|------|
| 架构索引 | `doc/ARCHITECTURE.md`（本文件） |
| 模块说明 | `app/{ui,glavo,camera,parser,protocol}/*.md`、`test/*.md`、`common/{ipc,middleware}/*.md` |
| Cursor Rules | `.cursor/rules/*.mdc` |
| 并发检查 MCP | `.cursor/mcp/cpp-concurrency-check.py` |
| 日志 | [doc/LOG_GUIDE.md](LOG_GUIDE.md) |
| 消息契约 | [common/README.md](../common/README.md)（`msg_types.hpp` 总表） |
