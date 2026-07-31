# ui/assets — 静态资源

图片、字库等 **无业务逻辑** 的编译期资源。所有 `.c` 均为脚本生成，**不要手改**（头部注释里写了出处）。

## 当前文件

| 文件 | 说明 | 出处 |
|------|------|------|
| `home_assets.h` + `home_assets.c` | 首页环境指标图标 `ic_pm` / `ic_temp` / `ic_o2`，并声明 `home_bg` | `tools/gen_home_assets.py`（脚本已不在 `tools/`） |
| `home_bg.c` | 首页背景图（1024×640 RGB565，符号 `home_bg`，声明在 `home_assets.h`） | `tools/gen_home_bg.py` |
| `home_metric_icons.h` / `.c` | A8 单色图标：`ic_home` `ic_switch` `ic_cabin` `ic_aqi` `ic_humidity` `ic_pressure` `ic_purify` `ic_info` `ic_clock` `ic_cube` `ic_warning` `ic_layers` `ic_expand` `ic_alert` `ic_printer`；打印态那批 `ic_play` `ic_door_solid` `ic_badge_spin` `ic_badge_play` `ic_badge_check` `ic_badge_alert` `ic_toast_info` | `tools/gen_home_metric_icons.py` |
| `home_btn_icons.h` / `.c` | 首页按钮图标 `ic_camera` / `ic_folder` | Figma SVG→A8（node 3122:13207 / 2513:19774） |
| `nav_icons.h` / `.c` | 左侧标签栏五个图标 `nav_icon_*` | `tools/png_to_lvgl_icon.py`（脚本已不在 `tools/`） |
| `settings_icons.h` / `.c` | 设置页分组图标 `settings_icon_*` | `tools/gen_settings_icons.py` |
| `print_flow_img.h` / `.c` | 打印流程弹窗顶图：打印板 `img_plate_single` / `img_plate_double`、门态三图、`img_mask` | `tools/gen_print_flow_assets.py` |
| `slide_icons.h` / `.c` | 开门滑块图标 `ic_door_open` | Figma SVG→A8（node 2513:19808） |
| `lv_font_cjk_data.c` | 思源黑体子集 TTF 字节数组（`lv_font_cjk_ttf`，`ui_theme.cpp` 的 `user_font_cjk()` 用 `lv_tiny_ttf_create_data` 运行时渲染）；新增中文需重跑脚本扩子集 | `tools/gen_cjk_font.py`（脚本已不在 `tools/`），源字体 `fonts/SourceHanSansCN-Regular.ttf` |

```bash
# 示例（以脚本内实际参数为准，均从仓库根执行）
python tools/gen_home_metric_icons.py
python tools/gen_print_flow_assets.py
```

## A8 图标的上色约定

`home_metric_icons` / `home_btn_icons` / `slide_icons` 是 **A8 灰度**（只存 alpha），
显示时用 `lv_obj_set_style_image_recolor()` 上色，因此同一份资源可复用于亮态/灰态。
新增图标在 `gen_home_metric_icons.py` 的三处登记后重跑脚本即可：

- `ICONS`：SVG path（从 Figma 抠出）
- `ORDER`：输出顺序
- `ICON_CFG`：栅格化尺寸 / viewBox / 描边宽度（小图标按 2× 栅格化，缩放后更清晰）
- `ICON_LINECAP`（可选）：默认圆头；`ic_badge_*` 那批按稿用 `butt`，否则短笔画会在
  拐角互相盖出更粗的一块

## 使用

```cpp
#include "home_metric_icons.h"

lv_image_set_src(img, &ic_layers);
```

## 原则

- 仅 `.c` / `.h` 资源与生成脚本说明，不放页面布局代码
- 页面与 components 通过 `#include` 引用，assets 不反向依赖 pages
