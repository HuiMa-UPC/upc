# ui/core — 底层核心

应用壳层、页面调度、对外接口与跨页面流程控制。

## 模块

| 文件 | 职责 |
|------|------|
| `ui_app` | `app_init()`：主题、MsgProxy、PageManager 注册、control、默认首页 |
| `page_manager` | 侧边栏图标菜单、内容区、页面切换 |
| `ui_simple_page` | 将 `page_*_create` / `_refresh` 接到 `Page` 生命周期 |
| `ui_msg_proxy` | UI ↔ Glavo MsgBus 封装（send / register） |
| `ui_device_state` | 设备名/联网/运行状态/环境快照的唯一来源（订阅一次，扇出给观察者） |
| `ui_device_bar` | 顶部常驻设备信息条（建在 `shell_` 上，按页显隐） |
| `ui_message_store` | 消息中心数据源：事件去重排序、未读计数、码→文案表、按等级自动收起（`sync` / `emit` 两种写入语义） |
| `ui_notice_stack` | 右上通知三态：toast / 铃铛 / 面板互斥，只在首页；打印态与监控态退回黑玻璃卡 |
| `ui_sys_hud` | 调试红字蒙版：挂 `lv_layer_sys` 右上角，显示 CPU 温 / 整机与本进程占用 / 内存；调试页 Media & Sim「Sys HUD」开关，仅本次运行 |
| `ui_backlight` | LCD 背光 sysfs（设置页 Display，滑条 5~100%），不经 Glavo、不看 MCU 版本 |
| `camera_msg_proxy` | UI ↔ Camera 预览开停 IPC |
| `debug_msg_proxy` | 调试页（手动控制/标定）专用消息门面，独立于 `ui_msg_proxy`；默认目标 APP_GLAVO，少数开关直连其它进程 |
| `camera_preview` | SHM 消费、RGA/SW NV12→BGRX、三缓冲 + lv_image 显示 |
| `video_player` | MP4 软解播放；拖动预览缩小帧 + scrub 世代丢弃 |
| `png_scrub_preview` | PNG 拖动预览异步解码（libpng → 小图 RGB565） |
| `rga_dma_buf` | Linux dma heap（优先 dma32）RAII，供 RGA fd 导入 |
| `control` | 打印流程：状态机、步骤 overlay、后端异步适配（`control.cpp`） |
| `lvgl_wrap.hpp` | LVGL 统一 include |

## 调用关系

```
app_init
  → PageManager::init / register_page(SimplePage → pages)
  → UiMsgProxy::init
  → CameraPreviewPlayer::init   // 注册 stream_start_rsp 回调
  → control_init
  → switch_to("user_home")
```

打印模式（`page_user_home`）→ `CameraPreviewPlayer::start/stop`；详见 [UI.md](../../UI.md) § CameraMsgProxy。

`PageManager::switch_to` 在打印中会 consult `control_is_printing()`。

## 原则

- 业务 Tab 的长布局在 **pages/**，不在 core
- 所有后端消息经 **UiMsgProxy**，pages 不直接订阅 MsgBus
