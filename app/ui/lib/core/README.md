# ui/core — 底层核心

应用壳层、页面调度、对外接口与跨页面流程控制。

## 模块

| 文件 | 职责 |
|------|------|
| `ui_app` | `app_init()`：主题、MsgProxy、PageManager 注册、control、默认首页 |
| `page_manager` | 侧边栏图标菜单、内容区、页面切换 |
| `ui_simple_page` | 将 `page_*_create` / `_refresh` 接到 `Page` 生命周期 |
| `ui_msg_proxy` | UI ↔ Glavo MsgBus 封装（send / register） |
| `camera_msg_proxy` | UI ↔ Camera 预览开停 IPC |
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
