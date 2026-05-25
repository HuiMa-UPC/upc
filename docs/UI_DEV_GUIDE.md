# Rongsu UI 开发指南

> 目标分辨率：**物理 1080×1920（竖屏）/ 逻辑 1920×1080（横屏，EGL GPU 旋转 90°）**  
> LVGL 版本：v9.5  
> 显示后端：DRM + EGL/GBM + OpenGLES  
> 输入后端：自定义 evdev（带坐标旋转）

---

## 1. 目录结构与文件组织

```
lvgl-upc/
├── inc/                          # 全局头文件
│   ├── lvgl_wrap.hpp             # LVGL 统一包含（C/C++ 兼容）
│   ├── platform.hpp              # 平台初始化（DRM / evdev / 旋转与触摸宏）
│   ├── ui_shell.hpp              # 页面壳：导航栏 + 内容区切换
│   ├── ui_theme.hpp              # 主题常量、配色、尺寸、字体接口
│   ├── ui_feedback.hpp           # Toast / 提示消息
│   ├── gcode_parse_gains.hpp     # G-code 解析增益配置
│   └── pipeline_tune.hpp         # 流水线调参
│
├── src/                          # 全局源码
│   ├── main.cpp                  # 入口：platform_init → ui_shell_init → 主循环
│   ├── platform.cpp              # DRM 显示、触摸 evdev、旋转/校准、tick/主循环
│   ├── ui_shell.cpp              # 页面壳实现：左侧导航 + 右侧内容区
│   ├── ui_theme.cpp              # 主题样式初始化、字体映射
│   ├── ui_feedback.cpp           # Toast 实现
│   ├── protocol_mcu.cpp/hpp      # MCU 通信协议（串口/TCP 下发 G-code）
│   ├── ring_buffer.cpp/hpp       # 环形缓冲区（G-code 包队列）
│   └── job_status.cpp/hpp        # 作业状态全局结构（前后端共享）
│
├── ui/                           # 页面模块（每个子目录一个页面）
│   ├── common/                   # 公共页面组件
│   │   ├── page_common.cpp/hpp   # 占位模板
│   │   └── ui_keyboard.cpp/hpp   # 全局软键盘
│   ├── home/                     # Home 监控页
│   │   ├── page_home.cpp/hpp     # 进度条、坐标、激光状态
│   │   └── ...
│   ├── control/                  # Control 控制页
│   │   ├── page_control.cpp/hpp  # TCP/串口连接、作业启停、增益输入
│   │   └── ctrl_backend.cpp/hpp  # 控制页后端逻辑（连接管理）
│   ├── files/                    # Files 文件页
│   │   ├── page_files.cpp/hpp    # G-code 文件列表、预览
│   │   ├── files_open.cpp/hpp    # 文件打开逻辑
│   │   └── gcode_worker.cpp/hpp  # G-code 解析工作线程
│   ├── settings/                 # Settings 设置页
│   │   └── page_settings.cpp/hpp # 板参数（A_MAX 等）输入
│   ├── serial/                   # Serial 调试页
│   │   └── page_serial.cpp/hpp   # 串口原始收发终端
│   └── tcpclient/                # TCP 调试页
│       ├── page_tcpclient.cpp/hpp# TCP 原始收发终端
│       └── tcpclient_backend.cpp/hpp
│
├── 3rdparty/lv_port_linux-release-v9.5/
│   ├── configs/rongsu_ui.defaults    # LVGL 配置默认值（字体、功能开关）
│   ├── lvgl/                         # LVGL v9.5 子模块
│   └── CMakeLists.txt                # 端口构建（生成 lv_conf.h）
│
└── docs/
    └── UI_DEV_GUIDE.md             # 本文件
```

---

## 2. 如何修改 UI 整体大小（字体、控件、布局）

本项目从 **1024×600** 升级到 **1920×1080**，所有尺寸已统一放大约 **1.75×**。如果你需要进一步调整，修改以下 3 处即可：

### 2.1 字体（`configs/rongsu_ui.defaults` + `src/ui_theme.cpp`）

**第一步**：在 `configs/rongsu_ui.defaults` 中开启所需字体：

```text
LV_FONT_MONTSERRAT_24 1
LV_FONT_MONTSERRAT_28 1
LV_FONT_MONTSERRAT_32 1
LV_FONT_DEFAULT &lv_font_montserrat_24
```

> 修改后必须重新运行 `cmake -S . -B build`，让 `generate_lv_conf.py` 重新生成 `lv_conf.h`。

**第二步**：在 `src/ui_theme.cpp` 中修改字体映射：

```cpp
const lv_font_t *ui_font_body(void)  { return &lv_font_montserrat_24; }
const lv_font_t *ui_font_title(void) { return &lv_font_montserrat_32; }
const lv_font_t *ui_font_icon(void)  { return &lv_font_montserrat_28; }
```

### 2.2 全局尺寸常量（`inc/ui_theme.hpp`）

所有控件尺寸、间距、圆角都在这里定义：

```cpp
#define UI_SIDEBAR_W        320     /* 左侧导航栏宽度 */
#define UI_NAV_ITEM_H       72      /* 导航按钮高度 */
#define UI_PAD              20      /* 大间距 */
#define UI_PAD_SM           14      /* 小间距 */
#define UI_RADIUS           10      /* 圆角 */
#define UI_FORM_LABEL_W     96      /* 表单标签宽 */
#define UI_INPUT_H          56      /* 输入框/下拉框高度 */
#define UI_TA_IP_W          260     /* IP 地址输入框宽 */
#define UI_BAR_TITLE_W      120     /* 进度条标题宽 */
```

> 修改这里后**不需要**重新 `cmake`，直接 `cmake --build` 即可，因为头文件会被自动依赖追踪。

### 2.3 硬编码尺寸（各 `page_*.cpp` + `src/ui_shell.cpp`）

少量页面内部有未走宏定义的硬编码值，需要手动同步：

| 位置 | 旧值 | 新值（1.75×） | 说明 |
|------|------|---------------|------|
| `ui_shell.cpp` | `icon` 宽 40 | 64 | 导航图标 |
| `ui_shell.cpp` | `pad_column` 12 | 18 | 导航按钮内间距 |
| `page_home.cpp` | 进度条高 14 | 22 | 进度条 |
| `page_control.cpp` | `g_btn_start` 72×36 | 120×56 | 启动按钮 |
| `page_control.cpp` | `g_btn_pause/stop/reset` 64×36 | 108×56 | 控制按钮 |
| `page_files.cpp` | `g_btn_open` 110×36 | 192×56 | 打开文件按钮 |

> **原则**：所有新增页面应尽量使用 `ui_theme.hpp` 中的宏（`UI_PAD`、`UI_INPUT_H` 等），避免引入新的硬编码像素值。

---

## 3. 如何添加新界面

### 3.1 第一步：在 `ui_shell.hpp` 中注册页面 ID

```cpp
typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_CONTROL,
    UI_PAGE_FILES,
    UI_PAGE_SETTINGS,
    UI_PAGE_SERIAL,
    UI_PAGE_TCP,
    UI_PAGE_COUNT
} ui_page_id_t;
```

> `UI_PAGE_COUNT` 必须放在最后，它决定了导航栏按钮数量。

### 3.2 第二步：创建页面文件

新建目录 `ui/mypage/`，包含：

**`page_mypage.hpp`**
```cpp
#ifndef PAGE_MYPAGE_H
#define PAGE_MYPAGE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_mypage_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif
```

**`page_mypage.cpp`**
```cpp
#include "page_mypage.hpp"
#include "ui_theme.hpp"

lv_obj_t *page_mypage_create(lv_obj_t *parent)
{
    /* parent 是右侧内容区根容器，已经设为 100%×100% */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));

    /* 页面内边距 */
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);

    /* 示例：一个标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "My Page");
    lv_obj_add_style(title, ui_style_title(), 0);

    return page;
}
```

### 3.3 第三步：接入页面壳

修改 `src/ui_shell.cpp`：

1. **包含头文件**（顶部 `#include` 区）：
   ```cpp
   #include "page_mypage.hpp"
   ```

2. **注册导航项**（`k_nav_defs` 数组中，在 `UI_PAGE_COUNT` 之前插入）：
   ```cpp
   static const nav_item_def_t k_nav_defs[UI_PAGE_COUNT] = {
       { LV_SYMBOL_HOME,     "Home",     page_home_create },
       { LV_SYMBOL_PLAY,     "Control",  page_control_create },
       { LV_SYMBOL_FILE,     "Files",    page_files_create },
       { LV_SYMBOL_SETTINGS, "Settings", page_settings_create },
       { LV_SYMBOL_USB,      "Serial",   page_serial_create },
       { LV_SYMBOL_WIFI,     "TCP",      page_tcp_create },
       { LV_SYMBOL_DUMMY,    "MyPage",   page_mypage_create },  /* 新增 */
   };
   ```

3. **确保 `ui_shell.hpp` 中的 `UI_PAGE_COUNT` 已更新**（如果 enum 加了新项，它会自动扩展）。

> 图标可用 LVGL 内置符号（`LV_SYMBOL_HOME`、`LV_SYMBOL_PLAY` 等），或直接用字符串 `"\xEF\x80\x81"` 这种 UTF-8 字体图标。

### 3.4 页面切换（代码中主动跳转）

```cpp
#include "ui_shell.hpp"

/* 切换到新页面 */
ui_shell_show_page(UI_PAGE_MYPAGE);
```

---

## 4. 前后端代码如何接入

### 4.1 数据流总览

```
┌─────────────┐      G-code 文件      ┌─────────────┐
│  UI (Files) │ ────────────────────> │ gcode_worker│
│  page_files │                       │ (解析线程)  │
└─────────────┘                       └──────┬──────┘
                                             │ 解析后的包
                              ┌──────────────┼──────────────┐
                              ▼              ▼              ▼
                        ┌─────────┐    ┌──────────┐   ┌──────────┐
                        │protocol_│    │ ring_    │   │ job_     │
                        │mcu      │    │ buffer   │   │ status   │
                        │(下发)   │    │ (队列)   │   │ (状态)   │
                        └────┬────┘    └──────────┘   └──────────┘
                             │
                             ▼ 串口 / TCP
                        ┌─────────┐
                        │   MCU   │
                        └─────────┘
```

### 4.2 后端 → UI 更新（状态推送）

**最常用模式**：后端在接收/解析/发送过程中更新全局 `job_status`，UI 通过 `lv_timer` 定期读取。

**`src/job_status.hpp`**（全局状态结构）：
```cpp
struct job_status_t {
    char     current_file[256];
    int      total_lines, parsed_lines;
    int      total_packets, sent_packets;
    size_t   ring_used, ring_capacity;
    uint16_t cmd_cache_rate, data_cache_rate;
    uint16_t current_x, current_y, current_z;
    uint16_t laser_state;
    bool     is_sending, is_parsing;
    bool     tcp_connected;
    bool     ring_full;
    bool     mcu_cmd_tx_hold;
    bool     flash_saved;
};

extern job_status_t g_job_status;
```

**线程安全**：读/写前加锁（`JOB_LOCK()` / `JOB_UNLOCK()`）。

**UI 刷新示例**（见 `page_home.cpp:status_update_cb`）：
```cpp
static void status_update_cb(lv_timer_t *)
{
    int total_lines, parsed_lines;
    {
        JOB_LOCK();
        total_lines  = g_job_status.total_lines;
        parsed_lines = g_job_status.parsed_lines;
    }

    int pct = percent_rounded(parsed_lines, total_lines);
    lv_bar_set_value(g_bar_parse, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(g_lbl_parse_pct, "%d%%", pct);
}
```

### 4.3 UI → 后端（用户操作）

以 Control 页的"Start"按钮为例：

**`ui/control/page_control.cpp`**（事件回调）：
```cpp
static void on_start_click(lv_event_t *)
{
    ctrl_start_job();              /* 调用后端接口 */
}
```

**`ui/control/ctrl_backend.cpp`**（后端实现）：
```cpp
void ctrl_start_job(void)
{
    /* 设置状态机、启动 gcode_worker 线程等 */
    gcode_worker_start(...);
}
```

### 4.4 新增后端接口 checklist

1. **声明**：在 `ui/xxx/xxx_backend.hpp` 中写函数原型
2. **实现**：在 `ui/xxx/xxx_backend.cpp` 中写逻辑
3. **调用**：在 `page_xxx.cpp` 的事件回调中调用
4. **数据回显**：通过 `job_status` 或自定义全局变量 + `lv_timer` 刷新到 UI

---

## 5. 公共工具函数使用

### 5.1 主题/样式

```cpp
#include "ui_theme.hpp"

/* 颜色 */
ui_color_bg, ui_color_accent, ui_color_text, ui_color_danger, ...

/* 字体 */
ui_font_body();   /* 正文字体（Montserrat 24） */
ui_font_title();  /* 标题字体（Montserrat 32） */
ui_font_icon();   /* 图标字体（Montserrat 28） */

/* 预置样式 */
lv_obj_add_style(obj, ui_style_panel(), 0);          /* 卡片面板 */
lv_obj_add_style(obj, ui_style_title(), 0);          /* 标题文字 */
lv_obj_add_style(obj, ui_style_subtitle(), 0);       /* 副标题 */
lv_obj_add_style(obj, ui_style_btn_primary(), 0);    /* 主按钮 */
```

### 5.2 Toast 提示

```cpp
#include "ui_feedback.hpp"

/* 成功提示（绿色，2秒） */
ui_toast_show("Job started", ui_color_accent, 2000);

/* 错误提示（红色，3秒） */
ui_toast_show("Connect failed", ui_color_danger, 3000);
```

### 5.3 全局软键盘

```cpp
#include "ui_keyboard.hpp"

/* 绑定到某个 textarea */
ui_keyboard_bind_textarea(g_ip_ta);

/* 页面切换时自动隐藏 */
ui_keyboard_hide();
```

### 5.4 常用辅助宏

```cpp
/* 百分比计算（带四舍五入） */
percent_rounded(num, den);      /* e.g. percent_rounded(3, 4) == 75 */

/* RingBuffer 使用率 */
ring_usage_percent(used, capacity);
```

---

## 6. 构建与运行

### 6.1 修改配置后重建

```bash
# 修改了 configs/rongsu_ui.defaults（字体、功能开关）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 仅修改了源码/头文件
cmake --build build --config Release -j 20
```

### 6.2 板端运行

```bash
# 杀掉 weston（如果它在占用 DRM）
killall weston weston-keyboard weston-desktop-shell

# 运行
./demo
```

### 6.3 屏幕旋转与触摸方向

显示与触摸的初始化均在 **`src/platform.cpp`**；可调宏定义在 **`inc/platform.hpp`**。

#### 6.3.1 修改屏幕旋转

当前默认：**物理 1080×1920（竖屏）→ 逻辑 1920×1080（横屏）**，通过 DRM/EGL 旋转 90°。

在 `inc/platform.hpp` 中修改：

```cpp
#define PLATFORM_DISPLAY_ROTATION LV_DISPLAY_ROTATION_90
```

可选值：

| 宏值 | 效果 |
|------|------|
| `LV_DISPLAY_ROTATION_0` | 不旋转，逻辑分辨率与物理面板一致 |
| `LV_DISPLAY_ROTATION_90` | 顺时针 90°（**当前默认**，横屏 UI） |
| `LV_DISPLAY_ROTATION_180` | 旋转 180° |
| `LV_DISPLAY_ROTATION_270` | 顺时针 270°（等同逆时针 90°） |

`init_drm_display()` 内会调用：

```cpp
lv_display_set_rotation(disp, PLATFORM_DISPLAY_ROTATION);
```

修改后执行 `cmake --build build` 即可，**无需**重新 `cmake -S`（除非改了 LVGL 配置）。

也可在 CMake 中覆盖（示例）：

```cmake
target_compile_definitions(${APP_TARGET} PRIVATE
    PLATFORM_DISPLAY_ROTATION=LV_DISPLAY_ROTATION_270)
```

#### 6.3.2 触摸设备路径

| 方式 | 说明 |
|------|------|
| **`PLATFORM_TOUCH_DEVICE`**（`inc/platform.hpp`） | 编译期默认，当前为 `/dev/input/event2` |
| **环境变量 `LV_LINUX_EVDEV_POINTER_DEVICE`** | 运行时覆盖，优先级更高 |

板端可用 `evtest` 或 `ls -l /dev/input/event*` 确认触摸节点。

#### 6.3.3 触摸方向（与显示旋转对齐）

触摸由 LVGL evdev 驱动创建，并绑定到同一 `display`：

```cpp
lv_indev_t *indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, dev);
lv_indev_set_display(indev, disp);
```

绑定后，驱动会在 **`3rdparty/lv_port_linux-release-v9.5/lvgl/src/drivers/evdev/lv_evdev.c`** 的 `_evdev_process_pointer()` 中，根据 `lv_display_get_rotation(disp)` 将物理触摸坐标变换为逻辑 UI 坐标（项目内已含 90°/180°/270° 补丁）。

**因此：改 `PLATFORM_DISPLAY_ROTATION` 后，触摸一般会随显示一起对齐**，无需在 `platform.cpp` 里手写坐标公式。

若出现 **X/Y 轴对调**（例如横向滑动时指针纵向移动），在 `inc/platform.hpp` 启用：

```cpp
#define PLATFORM_EVDEV_SWAP_AXES 1
```

或在 `init_touchscreen()` 中调用 `lv_evdev_set_swap_axes(indev, true)`。

若 **边距不准**（能点但偏移），在 `lv_indev_set_display` 之后增加校准：

```cpp
lv_evdev_set_calibration(indev, min_x, min_y, max_x, max_y);
```

`min_x/max_x/min_y/max_y` 可用 `evtest /dev/input/eventX` 查看 `ABS_X` / `ABS_Y` 的最小/最大值。

若 **方向仍镜像或反了**（改旋转角后个别轴翻转不对），需编辑 `lv_evdev.c` 中对应 `LV_DISPLAY_ROTATION_xxx` 分支的坐标映射（约 157–170 行），然后重新编译。

#### 6.3.4 DRM 显示设备

| 环境变量 | 说明 |
|----------|------|
| `LV_LINUX_DRM_CARD` | 指定 DRM 设备路径；未设置时由 `lv_linux_drm_find_device_path()` 自动探测 |

启动日志示例：

```text
DRM OK 1920x1080 (rotation=1)
touchscreen OK (/dev/input/event2)
```

其中 `rotation=1` 对应 `LV_DISPLAY_ROTATION_90`（枚举从 0 起）。

---

## 7. 总结：新增一页的最小改动清单

| 文件 | 改动 |
|------|------|
| `inc/ui_shell.hpp` | enum 加 `UI_PAGE_MYPAGE` |
| `ui/mypage/page_mypage.hpp` | 新建 |
| `ui/mypage/page_mypage.cpp` | 新建 |
| `src/ui_shell.cpp` | `#include` + `k_nav_defs` 加一行 |
| `CMakeLists.txt`（顶层） | `APP_SOURCES` 加 `ui/mypage/page_mypage.cpp` |
