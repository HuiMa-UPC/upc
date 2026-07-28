# 项目架构索引

> Agent / 开发者改模块前先查此表。细节见各模块 `*.md`，不要在此重复长文。

## 进程

| 进程 | 可执行文件 | 入口 | 模块文档 | 日志 |
|------|------------|------|----------|------|
| UI | `rongsu_ui` | `app/ui/main.cpp` | [ui/UI.md](../app/ui/UI.md) | `/var/log/rongsu_ui{1,2,3}.log` |
| Glavo | `rongsu_glavo` | `app/glavo/main.cpp` | [glavo/GLAVO.md](../app/glavo/GLAVO.md) | `/var/log/rongsu_glavo{1,2,3}.log` |
| 相机 | `rongsu_camera` | `app/camera/main.cpp` | [camera/CAMERA.md](../app/camera/CAMERA.md) | `/var/log/rongsu_camera{1,2,3}.log` |
| Xfer | `rongsu_xfr` | `app/xfer/main.cpp` | [xfer/XFER.md](../app/xfer/XFER.md) | `/var/log/rongsu_xfr{1,2,3}.log` |
| Visual | `rongsu_visual` | `app/visual/main.cpp` | [visual/VISUAL.md](../app/visual/VISUAL.md) | `/var/log/rongsu_visual{1,2,3}.log` |

启动：`scripts/rongsu.sh start`（Glavo → `rongsu_camera` → `rongsu_xfr` → `rongsu_visual` → UI）。  
编译：`./build.sh`（Release + 工具链，产物在 `output/`；`clean` / `reconfigure` 见脚本头注释）。

## 顶层目录

| 目录 | 文档 | 职责 |
|------|------|------|
| `app/` | — | 应用层进程与业务库（见下表）；构建入口 `app/CMakeLists.txt` |
| `common/` | [COMMON.md](../common/COMMON.md) | 契约、middleware、ipc、debug、process |
| `3rdparty/` | — | LVGL 等第三方依赖 |

### `app/` 子目录

| 目录 | 文档 | 职责 |
|------|------|------|
| `app/ui/` | [UI.md](../app/ui/UI.md) | LVGL 界面、UiMsgProxy、IPC Hub 客户端 |
| `app/glavo/` | [GLAVO.md](../app/glavo/GLAVO.md) | 打印编排、McuLink、IPC Server |
| `app/camera/` | [CAMERA.md](../app/camera/CAMERA.md) | 相机采集、SHM 预览、IPC Server |
| `app/xfer/` | [XFER.md](../app/xfer/XFER.md) | WiFi 发现、PIN 配对、HTTP 文件上传（仅收 `.json/.svg`） |
| `app/visual/` | [VISUAL.md](../app/visual/VISUAL.md) | 视觉 demo：相机流取帧、OpenCV 矩形识别、坐标回传 UI |
| `app/parser/` | [PARSER.md](../app/parser/PARSER.md) | G-code/OVF 解析库（Glavo 链接） |
| `app/protocol/` | [PROTOCOL.md](../app/protocol/PROTOCOL.md) | MCU 线协议（TCP `BE EF` + 串口 `5A A5`） |

## Glavo 库（`app/glavo/lib/` → `upc::glavo`）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `GlavoService` | `glavo_service.*` | IPC 业务编排 + 工作线程（打印/USB 目录/门控）；`init()` 绑定 RingBuffer → `McuLink` |
| `gcode_manifest` | `common/fs_manifest/gcode_manifest.*` | Local `/home/Gcode/config.json`（UI 读列表，Glavo Prepare/Delete 写） |
| `print_history_manifest` | `common/fs_manifest/print_history_manifest.*` | `/home/video/manifest.json`（Camera upsert，UI 历史页读） |
| `print_history_manifest` | `common/fs_manifest/print_history_manifest.*` | `/home/video/manifest.json`（Camera 写，UI 历史页读） |
| `McuLink` | `mcu_link.*` | TCP 运动 + 串口握手/传感器 |
| `PrintJobState` | `print_job_state.*` | 打印任务计数/状态 |
| `PrintVideoNotify` | `print_video_notify.*` | 换层 Snap / 录像会话 IPC → Camera |
| `device_config.hpp` | 头文件 | TCP/串口默认、`GLAVO_ENABLE_LAYER_SNAP` 等 |

## Camera（`app/camera/lib/` → 源编入 `rongsu_camera`，无独立静态库）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `CameraService` | `camera_service.*` | IPC Server + SHM 推流 + 录像分派 |
| `PrintVideoRecorder` | `print_video_recorder.*` | 层 Snap→MPP MJPEG→封装 MP4/缩略图 + upsert `manifest.json` + 发 `APP_Print_Video_Manifest_Ntf` 给 UI |
| `ViCapture` | `vi_capture.*` | rkaiq + V4L2 NV12 |
| `CameraDmaFrameSource` | `camera_dma_frame_source.*` | V4L2 dma fd 导出、stride padding |
| `device_config.hpp` | 头文件 | 设备/分辨率/IQ/SHM/录像路径宏 |

## UI core（`app/ui/lib/core/` → `ui::port` 一部分）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `UiMsgProxy` | `ui_msg_proxy.*` | UI ↔ Glavo IPC 封装 |
| `CameraMsgProxy` | `camera_msg_proxy.*` | 预览开停 IPC + 打印录像 manifest 就绪通知 |
| `CameraPreviewPlayer` | `camera_preview.*` | SHM 元数据环 + dma fd → RGA/SW BGRX → LVGL |
| `RgaDmaBuf` | `rga_dma_buf.*` | dma32 heap 缓冲（RGA fd 导入） |
| `PageManager` / `SimplePage` | `page_manager.*` / `ui_simple_page.*` | 侧边栏 + 页面路由 |
| `app_init()` | `ui_app.*` | 主题、Proxy、页面注册、control |
| `control_*` | `control.*` | 打印状态机（含 `CONTROL_STATE_PRINT_DONE` 结果页）+ overlay UI |
| `VideoPlayer` | `video_player.*` | 延时视频 libav* 软解 → BGRX → lv_image（play/pause/seek + 拖动预览独立解码线程） |

## UI 页面（`app/ui/lib/pages/`）

| Page ID | 文件 | 状态 |
|---------|------|------|
| `user_home` | `page_user_home.*` | 首页：环境、打印模式、预览、打印结果页（完成/终止浮层） |
| `user_history` | `page_user_history.*` | 打印历史：读 manifest + 卡片 + 缩略图(TJPGD) + 左滑删除 |
| `user_control` | `page_user_control.*` | skeleton 占位 |
| `user_settings` | `page_user_settings.*` | 设置列表 + 开发者密码弹窗 |
| `user_message` | `page_user_message.*` | skeleton 占位 |
| `user_timelapse` | `page_user_timelapse.*` | 延时摄影播放器（隐藏页，历史缩略图进入；全屏 `lv_layer_top`） |

注册入口：`ui_app.cpp`（不在 `main.cpp` include 各页面头）。隐藏页（`user_files`/`user_debug`/`user_timelapse`）经 `register_hidden_page` 注册、`switch_to` 进入。

## protocol/ 库（`app/protocol/` → `upc::protocol`）

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

预览像素：`rongsu_camera` → 主 IPC `SCM_RIGHTS` 传 dma_buf fd + SHM `/upc_camera_preview` 元数据环 → UI `CameraPreviewPlayer`（`MsgPort::take_peer_dma_fds` + RGA `importbuffer_fd` 或 SW 转 BGRX；与控制 IPC 分离）。

## 跨模块红线

- UI **禁止**链接 `app/glavo/lib`、`app/parser/` 业务库
- Glavo **禁止**链接 `lvgl`
- Camera **禁止**链接 `app/ui`、`app/glavo`、lvgl
- 跨进程回复 UI：**必须** `publish_async()`，目标 `ModuleId::UI`
- 消息契约：**只改** [common/COMMON.md](../common/COMMON.md) + `upc_contract.hpp`（四步流程）
- MCU 协议实现：`app/protocol/`（`upc::protocol`）+ `mcu_link.*`；权威表见 `doc/*.xlsx`

## 文档与 Agent 入口

| 类型 | 路径 |
|------|------|
| 架构索引 | `doc/ARCHITECTURE.md`（本文件） |
| 模块说明 | `app/{ui,glavo,camera,parser,protocol}/*.md`、`common/{ipc,middleware}/*.md` |
| Cursor Rules | `.cursor/rules/*.mdc` |
| 并发检查 MCP | `.cursor/mcp/cpp-concurrency-check.py` |
| 日志 | [doc/LOG_GUIDE.md](LOG_GUIDE.md) |
| 消息契约 | [common/COMMON.md](../common/COMMON.md)（`upc_contract.hpp` 总表） |
