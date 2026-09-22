# ui/assets — 静态资源

图片、字库等 **无业务逻辑** 的编译期资源。所有 `.c` 均为脚本生成，**不要手改**（头部注释里写了出处）。

## 当前文件

| 文件 | 说明 | 出处 |
|------|------|------|
| `home_assets.h` | 只剩两份 `home_bg_*` 的声明。原先那三枚图标已退场：`ic_temp` / `ic_o2` 重新描稿后收进 `gen_home_metric_icons.py`（现按档位出多份原生尺寸），`ic_pm` 随舱内 PM2.5 那行删除后已无调用点，`home_assets.c` 随之整个删掉 | 手维护（`tools/gen_home_assets.py` 早已不在 `tools/`） |
| `home_bg.c` | 首页背景图，**按屏分辨率两份**（RGB565，声明在 `home_assets.h`，挑图走 `page_user_home.cpp` 的 `home_bg_dsc()`）：`home_bg_1280` 1280×800 源节点 3752:8895「背景图1」、`home_bg_1024` 1024×640 源节点 3752:9081「背景图2」（1920 档也复用后者）。合计约 3.36MB。哪档配哪个节点、以及右侧镜像补边的来由见 `app/ui/UI.md`「首页待机布局」 | `tools/gen_home_bg.py`（输入 `tools/_bg_tmp/home_bg_1280_src.png` 与 `home_bg_src.png`） |
| `home_metric_icons.h` / `.c` | A8 单色图标。走 `make_dim_icon` 的那批**每档屏一份原生尺寸**（48/32/26，`switch` 另加 36/24/19），每枚附一张 `ic_<name>_set[]`：`ic_home_*` `ic_switch_*` `ic_cabin_*` `ic_temp_*` `ic_o2_*` `ic_humidity_*` `ic_pressure_*` `ic_bolt_*` `ic_bulb_*`（控制页 Laser Power / LIGHT 的卡头）`ic_chevron_left_{20,15}`（控制页更多控制的 Back 箭头，路径直接取自稿的 SVG 导出，20 画布 + 描边 2，**不走本套默认的 32/1.6**）；单档的 `ic_info`（40）`ic_clock` `ic_cube`（24）；手维护那批 `ic_warning` `ic_layers` `ic_expand` `ic_alert` `ic_printer` `ic_purify` `ic_play` `ic_door_solid` `ic_photo` `ic_badge_spin` `ic_badge_play` `ic_badge_check` `ic_badge_alert` `ic_toast_info` | `tools/gen_home_metric_icons.py`（保留式重写，见下方说明） |
| `notice_badge_icons.h` / `.c` | 通知等级徽标图标，每档屏一版：`ic_notice_alert_{14,18,27}` `ic_notice_info_{14,18,27}` | `tools/gen_notice_badge_icons.py` |

> **为什么要每档一份**：LVGL 的 `lv_draw_sw_transform` 是 2 抽头点采样、不做面积
> 平均，缩小时步长大于一个源像素就会整行整列跳过源数据。原先一张 40×40 喂三档，
> 实测横杠 alpha 在 32 档 225..251、26 档 188..220 之间起伏且峰值到不了实心，屏上
> 就是毛刺加断续。改成原生尺寸 1:1 之后横杠是平的 255。挑图在
> `page_user_home.cpp` 的 `pick_icon()`，尺寸源头是 `USER_METRIC_ICON_SZ`。
> **改主题那个值就要同步改脚本的 `METRIC_SIZES`**，否则又会退回缩放。
> 同一套做法见 `notice_badge_icons` 与 `ui_message_store.cpp`
> 的 `ui_message_severity_icon()`。
>
> **`home_metric_icons.c` 里那 14 枚手维护图标**（`ic_warning` `ic_layers`
> `ic_expand` `ic_alert` `ic_printer` `ic_purify` `ic_play` `ic_door_solid`
> `ic_photo` `ic_badge_*` `ic_toast_info`）**没有任何脚本能重新生成**——源矢量已
> 丢失。以前整跑 `gen_home_metric_icons.py` 会把它们连根抹掉、编译当场断；现在脚本
> 改成**保留式重写**（`read_foreign_blocks()` 把不归自己管的块原样读出再写回），
> 整跑已经安全。要改那 14 枚仍只能手改 `.c`。
> 通知徽标那两枚早先已摘出去放进 `notice_badge_icons`（源 SVG 在 `tools/_badge_svg/`，
> 可重跑）；其余 12 枚的处置方式待定。
| `home_btn_icons.h` / `.c` | 首页按钮图标 `ic_camera` / `ic_folder` | Figma SVG→A8（node 3122:13207 / 2513:19774） |
| `nav_icons.h` / `.c` | 左侧标签栏五个图标 `nav_icon_*` | `tools/png_to_lvgl_icon.py`（脚本已不在 `tools/`） |
| `settings_icons.h` / `.c` | 设置页分组图标 `settings_icon_*` | `tools/gen_settings_icons.py` |
| `print_flow_img.h` / `.c` | 打印流程弹窗顶图：打印板 `img_plate_single` / `img_plate_double`、门态三图、`img_mask`；转运盒那批 `img_box_install` `img_box_moving` `img_box_shutter` `img_door_close_hint` | `tools/gen_print_flow_assets.py` |
| `ctl_backdrop_img.h` / `.c` | 打印态底部复合控件的玻璃底衬 `img_ctl_bd_{run,collect}_{1920,1280,1024}`（ARGB8888，稿里的 Union 形状，含 1px 白 10% 内描边） | `tools/gen_ctl_backdrop.py` |
| `slide_icons.h` / `.c` | 开门滑块图标 `ic_door_open` | Figma SVG→A8（node 2513:19808） |
| `lv_font_cjk_data.c` | 思源黑体子集 TTF 字节数组（`lv_font_cjk_ttf`，`ui_theme.cpp` 的 `user_font_cjk()` 用 `lv_tiny_ttf_create_data` 运行时渲染）；新增中文需重跑脚本扩子集 | `tools/gen_cjk_font.py`（脚本已不在 `tools/`），源字体 `fonts/SourceHanSansCN-Regular.ttf` |

```bash
# 示例（以脚本内实际参数为准，均从仓库根执行）
python tools/gen_home_metric_icons.py
python tools/gen_print_flow_assets.py
python tools/gen_ctl_backdrop.py
```

`ic_camera` 与 `ic_photo` 是两枚不同的图标，别混用：前者是**摄像机**造型，用于首页
顶栏的「查看监控」入口；后者是**照相机**，用于打印态/监控态右侧中部的拍照钮（稿
3847:9440）。

`ctl_backdrop_img` 与主题常量强耦合：图按 `USER_PRINT_CTL_BTN` / `USER_PRINT_PILL_W`
三档预生成，尺寸与按钮坐标严格对齐、运行时不缩放。改了这两个常量必须同步改
`gen_ctl_backdrop.py` 的 `PROFILES` 并重跑，否则底衬会和按钮错位。脚本里的
`blob_of()` / `gap_of()` 也要与 `page_user_home.cpp` 的 `ctl_blob()` / `ctl_gap()`
保持一致。

## A8 图标的上色约定

`home_metric_icons` / `home_btn_icons` / `slide_icons` 是 **A8 灰度**（只存 alpha），
显示时用 `lv_obj_set_style_image_recolor()` 上色，因此同一份资源可复用于亮态/灰态。
新增图标在 `gen_home_metric_icons.py` 的三处登记后重跑脚本即可：

- `ICONS`：SVG 片段（从 Figma 抠出）。也可以写成 `f(边长)` 的函数，用于需要随输出
  尺寸现算几何的图标，见 `switch_glyph()`（把横杠吸到像素网格）与 `temp_glyph()`
  （给刻度点一个「不小于 1 设备像素」的下限）
- `ORDER`：输出顺序
- `ICON_SIZES`：该枚要出哪几档边长。列进 `METRIC_ICONS` 的会额外生成一张
  `ic_<name>_set[]` 供 `pick_icon()` 挑
- `ICON_CFG`：viewBox / 描边宽度。viewBox 可直接给稿的原生画布，不必把坐标折算到
  默认的 32——写成函数的 glyph 自己返回这两项，不用在这里登记
- `ICON_LINECAP` / `ICON_LINEJOIN`（可选）：默认都是圆的；`ic_switch` 按稿用
  `butt` + `bevel`，圆头会让线两端各多伸出半个描边宽、箭头尖发钝
- 单个元素可以用自己的 `fill` / `stroke` 属性覆盖外层 `<g>`，混合填充与描边就靠
  这个（见 `pressure` 的指针轴心、`temp` 的八颗刻度点）
- 从 Figma 拷来的 `stroke-opacity` / `fill-opacity` **必须剥掉**：A8 只存 alpha，
  变淡是运行时 `image_recolor` 上灰色做的，留着会二次变暗

只改其中一枚而不想重跑整个脚本时，import 该模块复用 `render()` + `emit_map()`
单独栅格化，再把结果替换进 `.c` 里对应的 `ic_<symbol>_map` 块。

## 使用

单档图标直接引用；多档的走 `page_user_home.cpp` 的 `pick_icon()` 按目标边长挑：

```cpp
#include "home_metric_icons.h"

lv_image_set_src(img, &ic_layers);                       /* 单档 */
make_dim_icon(parent, ic_home_set, USER_METRIC_ICON_SZ); /* 多档，1:1 不缩放 */
```

## 原则

- 仅 `.c` / `.h` 资源与生成脚本说明，不放页面布局代码
- 页面与 components 通过 `#include` 引用，assets 不反向依赖 pages
