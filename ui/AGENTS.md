# ui 模块 — Agent 使用指南

> 命名空间：`ui`  
> CMake 目标：`ui::port`（静态库 `ui`）  
> 可执行文件：`rongsu_ui`（`main.cpp`、`platform.cpp`）  
> 依赖：[`common/contract/README.md`](../common/contract/README.md)、[`common/middleware/AGENTS.md`](../common/middleware/AGENTS.md)、`upc::ipc`、`lvgl::port`  
> **禁止**链接 `glavo/lib`；后端经 IPC + `MsgPort` + `UiMsgProxy`

---

## 1. 模块概览

完整约定见 **[ui/README.md](README.md)**。

| 子目录 | 职责 |
|--------|------|
| **core** | 页面管理、切换逻辑、`UiMsgProxy`、`ui_app`、`control` 打印全流程 |
| **pages** | 各 Tab **页面布局与样式**（`page_user_*`） |
| **components** | 已封装的公用 LVGL 部件（主题、弹窗、滑动解锁等） |
| **assets** | 图片、字库等静态资源（如 `snowpod_device_img`） |

设计原则：

- **pages 只画本页 UI**；流程与切页在 **core**。
- **后端通信只走 `UiMsgProxy`**（core）；pages 禁止直接 `MsgBus::subscribe` / `publish`。
- **LVGL 只在 UI 线程操作**：后台通过 `publish_async` + 主循环 `process()` 间接更新界面。
- **页面注册在 `lib/core/ui_app.cpp`**：`ui/main.cpp` 只调用 `ui::app_init()`。

---

## 2. 目录结构

```
ui/
├── main.cpp                # 进程入口
├── platform.cpp/hpp        # DRM / LVGL 平台
├── CMakeLists.txt
├── AGENTS.md
└── lib/
    ├── core/               # PageManager、UiMsgProxy、control、ui_app
    ├── pages/              # page_user_* 布局
    ├── components/         # ui_* 公用控件
    └── assets/             # 图片/字库
```

**include 方式**（链接 `ui::port` 后可用）：

```cpp
#include "lvgl.h"
#include "page_manager.hpp"
#include "ui_msg_proxy.hpp"
#include "ui_app.hpp"
#include "page_demo.hpp"   // 一般只在 ui_app.cpp 或 pages 内部使用
```

---

## 3. CMake 集成

```cmake
add_subdirectory(ui)
target_link_libraries(rongsu_ui PRIVATE ui::port upc::ipc upc::process upc::debug)
```

`ui/CMakeLists.txt` 自动 GLOB（`CONFIGURE_DEPENDS`）：

- `lib/core/src/*.c*`
- `lib/pages/src/*.c*`
- `lib/components/src/*.c*`
- `lib/assets/src/*.c*`

**新增页面 `.cpp` 放入 `pages/src/` 即可**，一般无需改 CMake。

`ui::port` 的 `PUBLIC` 依赖仅为 `middleware::port`、`lvgl::port`。根 `CMakeLists.txt` 通过 `target_include_directories` 暴露 `ui/core/inc`、`ui/pages/inc` 等路径；`app/main.cpp` 只需 `#include "ui_app.hpp"`。

---

## 4. 应用入口（`app/src/main.cpp`）

```cpp
#include "ui_app.hpp"
#include "demo_service.hpp"

// middleware init → DemoService start → platform_init()
ui::app_init(lv_scr_act());

while (1) {
    const uint32_t delay = lv_timer_handler();
    middleware::MsgBus::instance().process();
    middleware::MsgBus::instance().process();  // 可选：同帧 Req+Rsp
    std::this_thread::sleep_for(
        std::chrono::milliseconds(delay > 0 ? delay : 1));
}
```

页面注册逻辑在 `ui/core/src/ui_app.cpp`，不在 `main.cpp`。

---

## 5. PageManager — 左侧菜单 + 右侧内容区

### 5.1 布局

```
┌──────────┬─────────────────────┐
│ menu     │  content_panel_     │
│ _panel_  │  （当前 Page 根对象） │
│ 72px     │                     │
│ 图标按钮  │                     │
└──────────┴─────────────────────┘
```

`init(screen)` 在 `screen` 下创建 `shell_`（横向 flex），左侧 `menu_panel_`（宽 72px、可滚动），右侧 `content_panel_`（`flex_grow` 占满剩余宽度）。

### 5.2 类名、`name()` 与 `switch_to` 的关系

三者**无编译器自动绑定**，靠约定手动对齐：

| 名称 | 示例 | 说明 |
|------|------|------|
| C++ 类名 | `PageDemo` | 页面实现类，命名习惯 `Page` + 功能名（PascalCase） |
| 页面 ID | `"demo"` | `Page::name()` 返回值，**全局唯一字符串** |
| 菜单图标 | `LV_SYMBOL_HOME` | 左侧按钮显示符号，与 `"demo"` 无查找关系 |

```cpp
class PageDemo : public Page {
public:
    std::string name() const override { return "demo"; }  // 页面 ID
    // ...
};

register_page(std::make_unique<PageDemo>(), LV_SYMBOL_HOME);
switch_to("demo");   // 必须与 name() 返回值完全一致
```

点击左侧菜单时，`PageManager` 内部调用 `switch_to(page->name())`，不硬编码字符串。当前页对应菜单按钮会置 `LV_STATE_CHECKED`。

### 5.3 `register_page` vs `switch_to`

| API | 作用 | 是否创建 LVGL 控件 |
|-----|------|-------------------|
| `register_page(page, icon)` | 登记页面 + 左侧加菜单按钮 | 否 |
| `switch_to(name)` | 激活页面：懒创建、显示、菜单高亮 | 首次调用时触发 `on_create` |

### 5.4 生命周期

| 回调 | 调用时机 |
|------|----------|
| `on_create(parent)` | **首次** `switch_to` 该页时，在 `content_panel_` 下建控件 |
| `on_enter()` | 每次成为当前可见页（`PageDemo` 中 `clear_flag(HIDDEN)`） |
| `on_exit()` | 切到其他页时（`PageDemo` 中 `add_flag(HIDDEN)`） |
| `on_destroy()` | 可选；当前实现不销毁对象，仅 hide |

`show_page` / `hide_page` 目前为空实现，可见性由各页 `on_enter` / `on_exit` 控制。

### 5.5 栈式 API（预留）

`push` / `pop` / `replace` / `home` 已在头文件声明；当前实现中 `push`/`replace` 等价于 `switch_to`，`pop` 为空操作，`home` 切到第一个注册页。侧边栏场景以 `switch_to` 为主。

---

## 6. 如何新增 UI 页面（标准流程）

以新增「设置页」为例。

### 步骤 1 — 新建头文件 `pages/inc/page_settings.hpp`

```cpp
#ifndef PAGE_SETTINGS_HPP
#define PAGE_SETTINGS_HPP

#include "page_manager.hpp"

namespace ui {

class PageSettings : public Page {
public:
    std::string name() const override { return "settings"; }  // 页面 ID
    void on_create(lv_obj_t* parent) override;
    void on_enter() override;
    void on_exit() override;

private:
    lv_obj_t* screen_ = nullptr;
};

} // namespace ui

#endif
```

### 步骤 2 — 新建实现 `pages/src/page_settings.cpp`

```cpp
#include "page_settings.hpp"

namespace ui {

void PageSettings::on_create(lv_obj_t* parent) {
    screen_ = lv_obj_create(parent);
    lv_obj_set_size(screen_, lv_pct(100), lv_pct(100));
    // ... 创建控件 ...
}

void PageSettings::on_enter() {
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_HIDDEN);
}

void PageSettings::on_exit() {
    lv_obj_add_flag(screen_, LV_OBJ_FLAG_HIDDEN);
}

} // namespace ui
```

### 步骤 3 — 在 `ui/core/src/ui_app.cpp` 注册

```cpp
#include "page_settings.hpp"

void app_init(lv_obj_t* screen) {
    PageManager::instance().init(screen);
    UiMsgProxy::instance().init();

    PageManager::instance().register_page(
        std::make_unique<PageDemo>(), LV_SYMBOL_HOME);
    PageManager::instance().register_page(
        std::make_unique<PageSettings>(), LV_SYMBOL_SETTINGS);

    PageManager::instance().switch_to("demo");  // 启动默认页
}
```

### 步骤 4 — 页面内跳转（可选）

在按钮回调中：

```cpp
ui::PageManager::instance().switch_to("settings");
```

### 步骤 5 — 若需后端通信

1. 在 `middleware/message_bus/msg_types.hpp` 增加 `MsgType`（若尚无）。
2. 在 `services/` 实现对应 Service（由 `app` 或上层链接，不在 `ui` 库内）。
3. 在 `UiMsgProxy` 增加 `send_*` / `register_*`。
4. 在页面 `on_create` 中 `register_*`，在 `on_exit` 中可传 `nullptr` 清空回调。

### 步骤 6 — 验证

```bash
rg "PageSettings|switch_to\(\"settings\"\)" --glob '*.{cpp,hpp}'
```

**无需改** `ui/CMakeLists.txt`（GLOB 自动收录 `pages/src/page_settings.cpp`）。

---

## 7. 如何删除 UI 页面

以删除 `PageSettings` 为例。

### 步骤 1 — 删除源文件

- `pages/inc/page_settings.hpp`
- `pages/src/page_settings.cpp`

### 步骤 2 — 清理 `ui_app.cpp`

移除 `#include "page_settings.hpp"`、`register_page(...PageSettings...)` 及任何 `switch_to("settings")`。

### 步骤 3 — 全仓搜索残留

```bash
rg "PageSettings|page_settings|switch_to\(\"settings\"\)" --glob '*.{cpp,hpp,md}'
```

### 步骤 4 — 清理关联后端（若有）

- 删除 `UiMsgProxy` 中该页专用的 `send_*` / `register_*`。
- 删除 `services/` 中仅该页使用的 Service。
- 评估是否从 `msg_types.hpp` 移除对应枚举。

### 步骤 5 — 重新编译

```bash
cmake --build build -j
```

---

## 8. UiMsgProxy — Demo 前后端（当前唯一通道）

### 8.1 消息流

```
PageDemo 按钮
  → send_demo_hello()
  → DemoHelloReq (UI → APP, publish_async)
  → DemoService
  → DemoHelloRsp (APP → UI)
  → register_demo_hello 回调
  → lv_label_set_text
```

### 8.2 API

| 方法 | 方向 | 说明 |
|------|------|------|
| `init()` | — | 订阅 `DemoHelloRsp`，模块 ID 为 `ModuleId::UI` |
| `send_demo_hello()` | UI → APP | 发布 `DemoHelloReq`（async） |
| `register_demo_hello(cb)` | APP → UI | 设置 Rsp 回调；payload 为 Inline 字符串 |

### 8.3 PageDemo 示例

```cpp
// on_create 中注册
UiMsgProxy::instance().register_demo_hello(
    [this](const std::string& text) {
        lv_label_set_text(label_msg_, text.c_str());
    });

// 按钮点击
UiMsgProxy::instance().send_demo_hello();
```

---

## 9. 命名与常见错误

| 问题 | 原因 | 处理 |
|------|------|------|
| `switch_to("demo")` 后页面空白 | `name()` 与字符串不一致 | 保证 `name()` 与 `switch_to` 完全相同 |
| 编译找不到 `page_xxx.hpp` | `main.cpp` 直接 include 页面头 | 页面注册应放在 `ui_app.cpp` |
| 切页后仍收到旧页回调 | 未在 `on_exit` 清空 `register_*` | `register_xxx(nullptr)` |
| 左侧菜单无反应 | 未先 `PageManager::init` | `init` 必须在 `register_page` 之前 |
| UI 无响应 | 主循环未 `MsgBus::process()` | 每帧至少调用一次 `process()` |

---

## 10. Agent 快速检查清单

- [ ] 新页面：`pages/inc` + `pages/src`，`name()` 唯一且与 `switch_to` 一致
- [ ] 在 `ui_app.cpp` 中 `register_page` + 启动时 `switch_to`
- [ ] 不在 `main.cpp` 直接 include 各页面头文件
- [ ] 主循环每帧 `lv_timer_handler()` + `MsgBus::process()`
- [ ] 页面不直接订阅 `MsgBus`；通信用 `UiMsgProxy`
- [ ] LVGL 头使用 `#include "lvgl.h"`
- [ ] 删页后 `rg` 无残留引用

---

## 11. 相关文档

| 文档 | 内容 |
|------|------|
| [middleware/AGENTS.md](../middleware/AGENTS.md) | MsgBus、MsgType |
| [services/AGENTS.md](../services/AGENTS.md) | DemoService |
| `ui/core/src/ui_app.cpp` | 页面注册入口（当前：`PageDemo` → `"demo"`） |
| `app/src/main.cpp` | 应用主循环与 Service 启动 |
