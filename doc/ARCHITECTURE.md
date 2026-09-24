# 项目架构索引

> Agent / 开发者改模块前先查此表。细节见各模块 `*.md`，不要在此重复长文。
> 第一次看仓库：先读 [快速理解.md](快速理解.md)（进程图 + 打印/预览路径）。

## 进程

| 进程 | 可执行文件 | 入口 | 模块文档 | 日志 |
|------|------------|------|----------|------|
| UI | `rongsu_ui` | `app/ui/main.cpp` | [ui/UI.md](../app/ui/UI.md) | `/var/log/topic/*.log` |
| Glavo | `rongsu_glavo` | `app/glavo/main.cpp` | [glavo/GLAVO.md](../app/glavo/GLAVO.md) | `/var/log/topic/*.log` |
| 相机 | `rongsu_camera` | `app/camera/main.cpp` | [camera/CAMERA.md](../app/camera/CAMERA.md) | `/var/log/topic/*.log` |
| 媒体 | `rongsu_media` | `app/media/main.cpp` | [media/MEDIA.md](../app/media/MEDIA.md) | `/var/log/topic/*.log` |
| Xfer | `rongsu_xfr` | `app/xfer/main.cpp` | [xfer/XFER.md](../app/xfer/XFER.md) | `/var/log/topic/*.log` |
| Visual | `rongsu_visual` | `app/visual/main.cpp` | [visual/VISUAL.md](../app/visual/VISUAL.md) | `/var/log/topic/*.log` |
| MQTT | `rongsu_mqtt` | `app/mqtt/main.cpp` | — | `/var/log/topic/*.log` |

启动：`scripts/rongsu.sh start`（Glavo → `rongsu_camera` → `rongsu_media` → `rongsu_xfr` → `rongsu_visual` → `rongsu_mqtt` → UI）。`/home/rongsu.sh` 只是选 A/B 槽的薄 bootstrap，上面这串顺序实现在槽内的 `scripts/rongsu_app.sh`，随应用层升级一起进槽；槽机制见 [OTA_IMAGE.md](../common/fs_manifest/OTA_IMAGE.md)。  
编译：`./build.sh`（Release + 工具链，产物在 `output/`；`clean` / `reconfigure` 见脚本头注释）。接手/虚拟机交叉编译见 [编译与运行.md](编译与运行.md)。
日志：按专题写 `/var/log/topic`，体积由板端 logrotate 控制，配置源文件 `scripts/topic` → `/etc/logrotate.d/topic`。

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
| `app/media/` | [MEDIA.md](../app/media/MEDIA.md) | 拍照/录像/延时封装 + 直播（云端 RTSP 推流、局域网 live555 RTSP Server；消费 Camera DMA 流） |
| `app/xfer/` | [XFER.md](../app/xfer/XFER.md) | WiFi 发现、PIN 配对、HTTP 分块上传；OVF 走 snowslice，传输 3MF 提取单份 G-code + PNG/RTHM 缩略图 |
| `app/visual/` | [VISUAL.md](../app/visual/VISUAL.md) | 视觉：Camera DMA 取帧、首层检测、打印前光路检测（红点桩） |
| `app/mqtt/` | — | MQTT 网关：激活/凭证轮换、环境遥测、报警上云（`alarm/report`+`alarm/ack`，契约 [soc-alarm-contract-v1.md](soc-alarm-contract-v1.md)）、`set_light`、云端 `start/stop_rtsp_push`→Media |
| `app/parser/` | [PARSER.md](../app/parser/PARSER.md) | G-code/OVF 解析库（Glavo 链接） |
| `app/protocol/` | [PROTOCOL.md](../app/protocol/PROTOCOL.md) | MCU 线协议（TCP `BE EF` + 串口 `5A A5`） |

## Glavo 库（`app/glavo/lib/` → `upc::glavo`）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `GlavoService` | `glavo_service.*` | IPC 业务编排 + 工作线程（打印/USB 目录/门控）；`init()` 绑定 RingBuffer → `McuLink` |
| `OpticalPathCheck` | `optical_path_check.*` | 打印前光路检测状态机（独立线程，结果回灌 worker） |
| `HeightLut2d` / `HeightCalibration` / `PlateHeightMeasure` | `height_points.hpp` `height_lut_2d.*` `height_calibration.*` `plate_height_measure.*` | 高度校准与打印前测高 |
| `galvo_motion_encode` | `galvo_motion_encode.*` | 调试运动包编码 + 五点坐标（点位测试与光路检测共用） |
| `gcode_manifest` | `common/fs_manifest/gcode_manifest.*` | Local `UPC_LOCAL_GCODE_DIR/config.json`（UI 读列表，Glavo Prepare/Delete 写） |
| `print_history_manifest` | `common/fs_manifest/print_history_manifest.*` | `UPC_VIDEO_CONFIG`（Media upsert 写；UI 历史页读，删除后重写） |
| `media_paths` | `common/fs_manifest/media_paths.hpp` | `UPC_MEDIA_ROOT`（默认 `/userdata/media`：photo/video/tmp + 首层视觉诊断图，各进程共用） |
| `image_paths` | `common/fs_manifest/image_paths.hpp` | `UPC_IMAGE_ROOT`（默认 `/userdata/image`：soc/mcu/galvo/tmp） |
| `ota_image_pair` | `common/fs_manifest/ota_image_pair.*` | MCU/Galvo AB 文件名与最高版本选取（规则 [OTA_IMAGE.md](../common/fs_manifest/OTA_IMAGE.md)） |
| `McuLink` | `mcu_link.*` | TCP 运动 + 串口握手/传感器；环境量合并 `SocSensors` 本地采集 |
| `SocSensors` | `soc_sensors.*` | 舱内外 PMSA UART、AHT20、CPM121、IIO 氧气周期采集；六路舱灯/`fa_valve` sysfs 随时可写（调试页）；与 UI 背光无关 |
| `SpiStatusLed` | `spi_status_led.*` | SPI 灯效板状态色（空闲蓝 / 打印橙 / CONDITION 红） |
| `SpiPm25Lcd` | `spi_pm25_lcd.*` | ST7735 SPI 小屏（fbdev `st7735rdrmfb`）常显舱外 PM2.5 |
| `PrintJobState` | `print_job_state.*` | 打印任务计数/状态 |
| `PrintVideoNotify` | `print_video_notify.*` | 换层 Snap / 录像会话 IPC → Media |
| `device_config.hpp` | 头文件 | TCP/串口默认、`GLAVO_ENABLE_LAYER_SNAP`、`GLAVO_N2_BLE_ENABLE` 等 |
| `N2BleClient` | `n2_ble_client.*` / `n2_ble_transport.*` / `n2_ble_parse.*` | 制氮机 BLE GATT Client → `APP_N2_Status_Ntf` |

## Camera（`app/camera/lib/` → 源编入 `rongsu_camera`，无独立静态库）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `CameraService` | `camera_service.*` | IPC Server + 多 peer SHM 推流（UI/Visual/Media） |
| `ViCapture` | `vi_capture.*` | rkaiq + V4L2 NV12 |
| `CameraDmaFrameSource` | `camera_dma_frame_source.*` | V4L2→隔离 pool 拷贝、dma fd 导出 |
| `device_config.hpp` | 头文件 | 设备/分辨率/IQ/SHM 宏 |

## Media（`app/media/lib/` → 源编入 `rongsu_media`，无独立静态库）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `MediaService` | `media_service.*` | IPC 客户端 + DMA 消费 + 拍照/录像/直播分派 |
| `PrintVideoRecorder` | `print_video_recorder.*` | 层 Snap→PNG + thumb/manifest + 开门 finalize |
| `PrintH264Recorder` | `print_h264_recorder.*` | 打印期 RKMPP 边编边封装 `print_h264.mp4` |
| `CleanVideoRecorder` | `clean_video_recorder.*` | 清粉延时接续 PNG |
| `H264StreamEncoder` | `h264_stream_encoder.*` | 直播共用 RKMPP，多 sink 分发 |
| `LiveRtspPusher` | `live_rtsp_pusher.*` | 云端 RTSP 推流（ffmpeg `-c copy`） |
| `LocalRtspServer` | `local_rtsp_server.*` | 局域网 live555 RTSP Server |
| `device_config.hpp` | 头文件 | 缩略图/ffmpeg/码率/延时 fps |

## UI core（`app/ui/lib/core/` → `ui::port` 一部分）

| 符号 | 路径 | 一句话 |
|------|------|--------|
| `UiMsgProxy` | `ui_msg_proxy.*` | UI ↔ Glavo IPC 封装 |
| `CameraMsgProxy` | `camera_msg_proxy.*` | 预览开停 IPC + 打印录像 manifest 就绪通知 |
| `CameraPreviewPlayer` | `camera_preview.*` | SHM 元数据环 + dma fd → RGA/SW BGRX → LVGL |
| `RgaDmaBuf` | `rga_dma_buf.*` | dma32 heap 缓冲（RGA fd 导入） |
| `PageManager` / `SimplePage` | `page_manager.*` / `ui_simple_page.*` | 侧边栏 + 页面路由 + 常驻设备信息条/通知浮层显隐 + 侧栏红点 |
| `ui_device_state_*` | `ui_device_state.*` | 设备名/联网/运行状态/环境快照的唯一来源（env 单槽订阅 + 观察者扇出） |
| `ui_device_bar_*` | `ui_device_bar.*` | 常驻设备信息条（WiFi + 设备名 + 状态点/字），PageManager 持有 |
| `ui_message_*` | `ui_message_store.*` | 消息中心：设备事件去重/排序/未读计数 + 码→文案表（`GLAVO_Device_Event_Ntf`）；`push_ex` 供报警等外部文案接入 |
| `ui_alarm_*` | `ui_alarm_state.*` | MCU 报警唯一数据源：生命周期缓存 + 复位在途/失败回执 + 历史落盘 + 快照请求与稀疏攒帧 + 打印阻断/会话抑制 |
| `ui_clean_state_*` | `ui_clean_state.*` | 清粉（clean_test）发起与回执的唯一来源：接管两个单槽 Proxy 回调，按发起方（结果页 Purify / 控制页手动清粉）扇出；`record_video` 决定是否连带清粉录像 |
| `ui_alarm_catalog_*` | `ui_alarm_catalog.hpp` / `ui_alarm_catalog_gen.cpp` | 报警字典（生成物，改 `tools/gen_alarm_catalog.py` + `doc/alarm_catalog_en.csv`）；槽位映射不在此，是 `upc_contract.hpp` 里的固定公式 |
| `ui_notice_stack_*` | `ui_notice_stack.*` | 右上通知三态：toast / 铃铛 / 面板互斥（**只在首页**；监控页不建铃铛与面板，靠画面左上的状态胶囊与阻塞弹窗，其余进消息中心），PageManager 持有 |
| `ui_sys_hud_*` | `ui_sys_hud.*` | 调试红字蒙版（`lv_layer_sys` 右上角：CPU 温 / 整机与本进程占用 / 内存 used/total+rss；调试页 Media & Sim「Sys HUD」，`upc_sys_monitor_sample` 读本机 `/proc` 与 thermal sysfs，仅本次运行） |
| `ui_backlight_*` | `ui_backlight.*` | LCD 背光 sysfs（设置页 Display），不经 Glavo、不看 MCU 版本 |
| `control_*` | `control.*` | 打印状态机（含 `CONTROL_STATE_PRINT_DONE` 结果页、`CONTROL_STATE_TRANSFER_BOX` 取件转运盒）+ overlay UI |
| `VideoPlayer` | `video_player.*` | 延时视频 libav* 软解 → BGRX/RGB565 → lv_image（play/pause/seek；拖动预览缩小帧 + latest-wins） |
| `PngScrubPreview` | `png_scrub_preview.*` | PNG 序列拖动预览：后台 libpng 解码缩放到小图 RGB565 |
| `PhotoDecoder` | `photo_decoder.*` | 相册大图后台解码：等比贴合屏幕 → RGB565 → lv_image（latest-wins；几何由主线程发布） |
| `png_util` | `png_decode_util.*` | PNG→RGBA 解码 + RGB565 缩放（点采样/box）+ CONTAIN 贴合算尺寸；上面两者共用 |
| `ui::album` | `album_index.*` | 相册目录枚举/mtime 倒序/配对 `.rthm`（历史页列表与全屏浏览器必须同序） |

## UI 页面（`app/ui/lib/pages/`）

| Page ID | 文件 | 状态 |
|---------|------|------|
| `user_home` | `page_user_home.*` | 首页：环境、打印模式、预览、打印结果页（完成/终止浮层） |
| `user_history` | `page_user_history.*` | 打印历史：三 Tab + Storage 条 + 日期分组卡片 + 管理态批量删除 + 左滑删除 |
| `user_control` | `page_user_control.*` | 控制面板：两列玻璃卡（环境/耗材滤芯/清粉/激光功率/照明）+ 开门滑块 + 两个调整弹窗；页内切「更多控制」（Motion 三轴点动 / Device 打印参数·风扇·电机·开关） |
| `user_settings` | `page_user_settings.*` | 设置列表 + 开发者密码弹窗 |
| `user_message` | `page_user_message.*` | 消息中心：日期分组混排列表（MCU 报警 + 设备事件）+ 左滑删除历史 + 详情抽屉（Reset 三态） |
| `user_timelapse` | `page_user_timelapse.*` | 延时摄影播放器（隐藏页，历史缩略图进入；全屏 `lv_layer_top`） |
| `user_photo` | `page_user_photo.*` | 相册全屏浏览器（隐藏页，历史页 Album 缩略图进入；滑动/箭头翻页 + 首末回绕 + 窗口化 filmstrip；翻页判据「距离 or 轻扫」+ 滑入动画） |

注册入口：`ui_app.cpp`（不在 `main.cpp` include 各页面头）。隐藏页（`user_files`/`user_debug`/`user_timelapse`/`user_photo`）经 `register_hidden_page` 注册、`switch_to` 进入。

## protocol/ 库（`app/protocol/` → `upc::protocol`）

| 子目录 | 协议表 | 说明 |
|--------|--------|------|
| `app/protocol/tcp/` | `doc/SOC-_MCU TCP报文.xlsx` | 运动/心跳/复位组包，RX 背压解析 |
| `app/protocol/serial/` | `doc/SOC_-_MCU串口通信协议.xlsx` | 打印握手 + 环境上报 + 报警上报/复位（`serial_alarm.*`） |

## 数据流（打印）

```
UI → IPC → GlavoService → ParserManager → RingBuffer
                              ↓
                         McuLink → TCP（运动）+ 串口（握手/传感器）
                              ↓（换层排空后，可选 GLAVO_ENABLE_LAYER_SNAP）
                    PrintVideoNotify → IPC(UI Hub FWD) → Media → PNG/MP4
```

预览像素：`rongsu_camera` → 隔离 pool 拷贝 → 主 IPC `SCM_RIGHTS` 传 dma_buf fd + SHM `/upc_camera_preview` 元数据环 → UI `CameraPreviewPlayer`（`MsgPort::take_peer_dma_fds` + RGA `importbuffer_fd` 或 SW 转 BGRX；与控制 IPC 分离）。RK3568 上 RGA2 无 IOMMU、rkisp 有；所有权见 [camera/CAMERA.md](../app/camera/CAMERA.md) 「ISP IOMMU 与 dma-buf 所有权」。

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
| 快速理解 | [快速理解.md](快速理解.md)（进程全景、打印/预览路径） |
| 架构索引 | `doc/ARCHITECTURE.md`（本文件） |
| 模块说明 | `app/{ui,glavo,camera,media,parser,protocol}/*.md`、`common/{ipc,middleware}/*.md` |
| 专利参考 | [专利参考-系统架构与运行逻辑.md](专利参考-系统架构与运行逻辑.md)、[专利参考-振镜校准算法与增益设置.md](专利参考-振镜校准算法与增益设置.md) |
| Cursor Rules | `.cursor/rules/*.mdc` |
| 并发检查 MCP | `.cursor/mcp/cpp-concurrency-check.py` |
| IPC 契约 MCP | `.cursor/mcp/upc-dev.py`（`lookup_msg` / `lookup_payload` / `check_contract_drift` / `lookup_alarm`） |
| 日志 | [common/COMMON.md § 专题日志字典](../common/COMMON.md#专题日志字典varlogtopiclog)（专题划分、落盘路径、轮转；**新增大类必须同步 `scripts/topic`**） |
| 消息契约 | [common/COMMON.md](../common/COMMON.md)（`upc_contract.hpp` 总表） |
