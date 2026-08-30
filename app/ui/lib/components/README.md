# ui/components — 公用 UI 部件

已封装、可在多个页面或流程中复用的 LVGL 控件与主题，**不写具体业务页面布局**。

## 模块

| 模块 | 用途 |
|------|------|
| `ui_theme` | 1920/1280 profile、颜色、字体、按钮/输入框样式宏 |
| `ui_user_widgets` | 用户风通用块：磨砂卡片、数值块、耗材行、分段控件、文件 meta 格式化等 |
| `ui_user_skeleton` | Tab 占位页（标题 + 卡片 + 提示文案） |
| `ui_modal_dialog` | 标准弹窗壳（标题、body、footer） |
| `ui_slide_unlock` | 滑动解锁条（可配 knob 图标 / 透明轨道；`ui_slide_unlock_set_hint()` 换提示文案） |
| `ui_board_anim` | 打印板插入动画 |
| `ui_arc_spinner` | 圆弧加载动画 |
| `ui_keyboard` | 全局软键盘 |
| `ui_feedback` | Toast（`ui_toast_show_icon()` 可带前置图标）、二次确认框 |

## 使用方式

```cpp
#include "ui_user_widgets.hpp"
#include "ui_theme.hpp"

lv_obj_t *card = ui_user_make_frosted_card(parent, true);
```

分段控件（`ui_user_make_segmented`）支持「无选中」：初值/入参传 `UI_USER_SEG_NONE`
则全段置灰，读回也是该哨兵。用于必须由用户显式选择、没有合理默认值的场景。
**当前无调用方**（唯一的用例「选文件页打印板厚度」已随该功能整块删除），
留着是因为这个哨兵不影响传合法索引的老调用方。

跨页共用的三枚（都因为「≥2 处使用」才从页面里提上来）：

| 构件 | 稿 | 共用方 |
|------|----|--------|
| `ui_user_make_icon_img` | — | A8 图标盒：固定占位 + recolor + 等比缩放。首页按钮图标、历史页卡片指标/角标/空态插画 |
| `ui_user_make_primary_pill_btn` | 「Button / On Light」 | 蓝色对角渐变胶囊：历史页空态 `Start Print`（内容撑宽、内容居中）+ 首页左下 `Start Print`（稿 5339:27284，传 `width`/`spread` 换成定宽 + 图标贴左文字贴右） |
| `ui_user_make_confirm_footer_btn` | `2567:5555` | 确认弹窗页脚按钮（`USER_BTN_RADIUS` 圆角矩形，非胶囊）：删除文件 / 终止打印 / 历史页批量删除 / 开始打印确认 |

按钮样式基线（几何、主/副色值、哪些地方仍保留胶囊形）见 `app/ui/UI.md`「按钮样式基线」。
`ui_user_make_footer_ghost_btn` 是定宽版副按钮，色值与 `ui_user_make_confirm_footer_btn`
的 `filled=false` 一致，不再套 `user_style_btn_ghost()`（那套是 panel 实心 + 1px 描边）。

## 原则

- 接口以 `ui_*` 命名，参数只依赖 `lv_obj_t*` 与主题宏
- 不 `#include` 任何 `page_user_*` 头文件
- 新控件若 ≥2 处使用，应放在本目录而非复制到 pages
