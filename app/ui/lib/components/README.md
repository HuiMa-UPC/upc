# ui/components — 公用 UI 部件

已封装、可在多个页面或流程中复用的 LVGL 控件与主题，**不写具体业务页面布局**。

## 模块

| 模块 | 用途 |
|------|------|
| `ui_theme` | 1920/1280 profile、颜色、字体、按钮/输入框样式宏 |
| `ui_user_widgets` | 用户风通用块：磨砂卡片、数值块、耗材行、分段控件、文件 meta 格式化等 |
| `ui_user_skeleton` | Tab 占位页（标题 + 卡片 + 提示文案） |
| `ui_modal_dialog` | 标准弹窗壳（标题、body、footer） |
| `ui_slide_unlock` | 滑动解锁条 |
| `ui_board_anim` | 打印板插入动画 |
| `ui_arc_spinner` | 圆弧加载动画 |
| `ui_keyboard` | 全局软键盘 |
| `ui_feedback` | Toast、二次确认框 |

## 使用方式

```cpp
#include "ui_user_widgets.hpp"
#include "ui_theme.hpp"

lv_obj_t *card = ui_user_make_frosted_card(parent, true);
```

分段控件（`ui_user_make_segmented`）支持「无选中」：初值/入参传 `UI_USER_SEG_NONE`
则全段置灰，读回也是该哨兵。用于必须由用户显式选择的场景（如打印板厚度）。

## 原则

- 接口以 `ui_*` 命名，参数只依赖 `lv_obj_t*` 与主题宏
- 不 `#include` 任何 `page_user_*` 头文件
- 新控件若 ≥2 处使用，应放在本目录而非复制到 pages
