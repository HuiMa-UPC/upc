# ui/pages — Tab 页面布局

本目录 **只放各页面的布局、样式与页面内刷新逻辑**。

## 文件

| 文件 | 职责 |
|------|------|
| `page_user_home.*` | 首页：顶栏、设备区、环境数值、打印中模式 UI |
| `page_user_settings.*` | 设置列表、开发者密码弹窗 |
| `page_user_history.*` | 历史 Tab（当前为 skeleton 占位） |
| `page_user_control.*` | 控制 Tab（占位） |
| `page_user_message.*` | 消息 Tab（占位） |

## 不在本目录的内容

| 模块 | 位置 |
|------|------|
| 页面切换 / 侧边栏 | `core/page_manager` |
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

- 首页环境数据：在 `page_user_home.cpp` 内通过 `UiMsgProxy::register_env_update` 绑定（页面级数据源，仍不直接碰 MsgBus）
- 打印相关：只调用 `control_start()`、`control_terminate_request()` 等 **core** API
