# ui/pages — Tab 页面布局

本目录 **只放各页面的布局、样式与页面内刷新逻辑**。

## 文件

| 文件 | 职责 |
|------|------|
| `page_user_home.*` | 首页：设备图、问候语、HOME 空气质量卡 + 四宫格、左下 PRINT、右下开门滑块。**里面的打印态浮层与待机监控层现在是存档、无调用方**，见该文件头的说明 |
| `page_user_monitor.*` | 监控 Tab：左摄像头 + 右栏三 Tab（Job / Status / Manual）+ 全屏切换。打印态界面在这里（改版前在首页浮层） |
| `page_user_settings.*` | 设置：8 项左导航 + 右栏白卡（网络 / 显示与亮度 / 存储 / 关于 / 调试入口） |
| `page_user_files.*` | 选文件：本机/USB 页签 + 文件列表 + 右栏预览与确认打印·删除。只做视图，数据与打印流程在 `core/control.*` |
| `page_user_history.*` | 历史 Tab：三 Tab（只有 Print History 有数据）+ Storage 容量条与告警横幅 + 日期分组卡片 + 管理态多选批量删除 + 左滑删除 + 空态 |
| `page_user_message.*` | 消息 Tab（浅色档）：三 Tab + All read 浮层顶栏 + 日期分组混排列表（MCU 报警当前/历史 + 设备事件）+ 底部 Load More 分页 + 左滑删除（仅历史/已解除）+ 右侧 632 详情抽屉（Reset 三态）；进页即把消息中心全部标已读、熄侧栏红点 |

## 不在本目录的内容

| 模块 | 位置 |
|------|------|
| 页面切换 / 侧边栏 / 常驻设备信息条 | `core/page_manager` + `core/ui_device_bar` |
| 设备名 / 联网 / 运行状态 / 环境快照 | `core/ui_device_state` |
| 设备事件消息 / 右上通知浮层 | `core/ui_message_store` + `core/ui_notice_stack` |
| MCU 报警状态缓存 / 字典 | `core/ui_alarm_state` + `core/ui_alarm_catalog`（生成物，改 `tools/gen_alarm_catalog.py`） |
| 应用初始化、页面注册 | `core/ui_app` |
| 打印流程 | `core/control` |
| 后端消息 | `core/ui_msg_proxy` |
| 设备图等资源 | `assets/` |
| 滑动解锁、弹窗壳等 | `components/` |

## 新增 Tab 页

1. 增加 `page_user_xxx.hpp` / `page_user_xxx.cpp`（`xxx_create` + `xxx_refresh`）
2. 在 `core/src/ui_app.cpp` 用 `SimplePage` 注册
3. 复用 `components`：`ui_user_skeleton`、`ui_user_widgets` 等

## 数据与后端

- 环境/状态快照：**不要**在页面里 `UiMsgProxy::register_env_update`（单槽，会顶掉常驻状态栏的数据源）。
  改用 `ui_device_state_add_observer()` 读 `core/ui_device_state` 的共享快照；运行状态置位用
  `ui_device_state_set_status()`
- 页面不用自己画顶部设备信息条：外壳常驻，`content_panel_` 的 `pad_top` 已让位；
  隐藏页（无侧栏图标）不显示该条，`pad_top` 收回 0
- 打印相关：只调用 `control_start()`、`control_terminate_request()` 等 **core** API
