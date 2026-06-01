# ui/core — 底层核心

应用壳层、页面调度、对外接口与跨页面流程控制。

## 模块

| 文件 | 职责 |
|------|------|
| `ui_app` | `app_init()`：主题、MsgProxy、PageManager 注册、control、默认首页 |
| `page_manager` | 侧边栏图标菜单、内容区、页面切换 |
| `ui_simple_page` | 将 `page_*_create` / `_refresh` 接到 `Page` 生命周期 |
| `ui_msg_proxy` | UI 侧唯一 MsgBus 封装（send / register） |
| `control` | 打印流程：状态机、步骤 overlay、后端异步适配（`control.cpp`） |
| `lvgl_wrap.hpp` | LVGL 统一 include |

## 调用关系

```
app_init
  → PageManager::init / register_page(SimplePage → pages)
  → UiMsgProxy::init
  → control_init
  → switch_to("user_home")
```

`PageManager::switch_to` 在打印中会 consult `control_is_printing()`。

## 原则

- 业务 Tab 的长布局在 **pages/**，不在 core
- 所有后端消息经 **UiMsgProxy**，pages 不直接订阅 MsgBus
