# UI 多人开发说明

## 技术栈

- **语言**：C++17（`src/*.cpp`、`ui/**/*.cpp`）
- **UI 库**：LVGL v9.5
- **显示后端**：Linux DRM（纯 dumb buffer 模式，非 EGL）
- **输入后端**：evdev（触摸屏）
- **目标分辨率**：横屏 **1920×1080**（设计基准，小屏可缩放）

## 目录结构

```
inc/                          # 核心头文件
  lvgl_wrap.hpp               # LVGL C 库头文件封装（含 extern "C"）
  platform.hpp                # 平台初始化与主循环
  ui_shell.hpp                # 横屏壳：左导航 + 右内容
  ui_theme.hpp                # 配色、字号、公共样式（前端规范）

src/                          # 核心实现
  main.cpp                    # 程序入口
  platform.cpp                # DRM 显示 + evdev 触摸初始化
  ui_theme.cpp                # Klipper 风格主题、样式、字体
  ui_shell.cpp                # 导航与页面切换（勿改除非加 Tab）

ui/                           # 页面模块（每人负责一个目录）
  common/
    page_common.hpp           # 占位页模板头文件
    page_common.cpp           # 公共占位页实现
  home/
    page_home.hpp
    page_home.cpp
  control/
    page_control.hpp
    page_control.cpp
  files/
    page_files.hpp
    page_files.cpp
  settings/
    page_settings.hpp
    page_settings.cpp
```

## 横屏布局

```
+------------------+--------------------------------+
|  [icon] Home     |                                |
|  [icon] Control  |      当前 page 内容区           |
|  [icon] Files    |      (page_xxx_create)         |
|  [icon] Settings |                                |
|  (可纵向滑动)     |                                |
+------------------+--------------------------------+
     280px                   flex 剩余
```

- 左侧导航栏固定宽 `UI_SIDEBAR_W`（280px）
- 右侧内容区由 `ui_shell` 传入的 `parent` 提供，占满剩余空间
- 所有 page 的根对象必须 `lv_obj_set_size(page, LV_PCT(100), LV_PCT(100))`

## page_common 占位页模板

`ui/common/page_common` 提供了统一的**空状态/占位页面**，避免重复写布局。

**API**：
```cpp
lv_obj_t *page_common_create_placeholder(lv_obj_t *parent,
                                          const char *title,
                                          const char *hint);
```

**效果**：一个居中卡片，内含白色大标题 + 灰色提示文字。各页面在内容未定前可直接调用它，后续再替换为真实业务控件。

示例见 `ui/home/page_home.cpp`：
```cpp
lv_obj_t *page_home_create(lv_obj_t *parent)
{
    return page_common_create_placeholder(parent,
                                          "Home",
                                          "page_home.cpp\nAssignee: ___\nContent TBD");
}
```

## 新增页面步骤

1. 在 `inc/ui_shell.hpp` 的 `ui_page_id_t` 枚举中增加新页面项
2. 新建目录 `ui/xxx/`，添加 `page_xxx.hpp` + `page_xxx.cpp`，实现：
   ```cpp
   lv_obj_t *page_xxx_create(lv_obj_t *parent);
   ```
3. 在 `src/ui_shell.cpp` 的 `k_nav_defs[]` 中注册图标、标题、factory
4. 在 `CMakeLists.txt` 的 `APP_SOURCES` 中加入 `ui/xxx/page_xxx.cpp`
5. 在 `CMakeLists.txt` 的 `target_include_directories` 中加入 `${CMAKE_SOURCE_DIR}/ui/xxx`
6. 在 `ui/xxx/page_xxx.cpp` 的提示文本中标明负责人和功能描述

## 页面开发约定

- 只在 `ui/xxx/page_xxx.cpp` 内创建控件，父对象为 `parent`（即右侧内容区）
- 页面根对象请 `lv_obj_set_size(page, LV_PCT(100), LV_PCT(100))`
- 样式优先用 `ui_theme.hpp` 的 `ui_style_*()` 和配色常量，勿随意写死颜色
- 控件尽量大，适应触摸屏：
  - 正文：`ui_font_body()` 16px
  - 标题：`ui_font_title()` 20px
  - 图标：`ui_font_icon()` 18px

## 关于显示方向（重要）

当前项目使用 **纯 DRM 后端**（`LV_LINUX_DRM_USE_EGL 0`），该后端**不支持 `lv_display_set_rotation()` 软件旋转**。

- UI 按 **横屏 1920×1080** 设计，若你的屏幕物理分辨率本身就是横屏（如 HDMI 显示器），无需额外处理
- 若屏幕物理为竖屏（如 1080×1920 的 MIPI 屏），需要旋转输出，可选方案：
  1. **DRM Plane 硬件 Rotation**：在 DRM 驱动层给 plane 设置 `rotation` property（需内核驱动支持）
  2. **切换 EGL 后端**：将 `configs/rongsu_ui.defaults` 中的 `LV_LINUX_DRM_USE_EGL` 改为 `1`，并确保系统有 OpenGL ES + EGL 库，然后调用 `lv_display_set_rotation()`
  3. **改设备树/内核参数**：在 DTB 或 bootargs 中强制面板为横向时序（不在本项目代码范围内）
  4. **软件旋转**：性能开销大，高分辨率下不推荐

## 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## 板端运行

默认触摸设备 **`/dev/input/event2`**（见 `inc/platform.hpp` 的 `PLATFORM_TOUCH_DEVICE`）。

```bash
./demo
```

临时改用其它触摸节点：

```bash
export LV_LINUX_EVDEV_POINTER_DEVICE=/dev/input/event2
./demo
```
