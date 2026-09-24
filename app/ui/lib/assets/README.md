# ui/assets — 静态资源

图片、字库等 **无业务逻辑** 的编译期资源。所有 `.c` 均为脚本生成，**不要手改**（头部注释里写了出处）。

## 当前文件

| 文件 | 说明 | 出处 |
|------|------|------|
| `home_assets.h` | 只剩 `home_device_*` 与 `home_card_bg_*` 的声明（旧的整屏背景 `home_bg_*` 已随浅色稿退场）。原先那三枚图标也已退场：`ic_temp` / `ic_o2` 重新描稿后收进 `gen_home_metric_icons.py`（现按档位出多份原生尺寸），`ic_pm` 随舱内 PM2.5 那行删除后已无调用点，`home_assets.c` 随之整个删掉 | 手维护（`tools/gen_home_assets.py` 早已不在 `tools/`） |
| `home_device.c` | 首页待机态的设备渲染图，**按屏分辨率两份**（RGB565A8 带 alpha，声明在 `home_assets.h`，挑图走 `page_user_home.cpp` 的 `home_device_dsc()`）：`home_device_1280` 525×791、`home_device_1024` 420×633（1920 档复用前者）。合计约 1.95MB。稿上**不是等比 cover**（源图先放大再左移、由框裁），框外还叠了一道底部淡入。**上一版（6201:85912）是 700 见方、带 darken + 四道淡入 + 两块料面矢量层，新稿全去掉了**，别拿旧参数套新图。源节点 6230:89954 | `tools/gen_home_bg.py`（输入 `tools/_bg_tmp/home_device_src_v2.png`） |
| `home_card_bg.c` | HOME 空气质量卡的背景，**按屏分辨率两份**（RGB565 不透明，挑图走 `home_card_bg_dsc()`）：`home_card_bg_1280` 240×300、`home_card_bg_1024` 192×240。把稿上的径向渐变 + 点阵烤成一张图，圆角交给 LVGL 的 radius。合计约 230KB。源节点 6201:85915 | `tools/gen_home_bg.py`（输入 `tools/_bg_tmp/home_card_dots.svg`） |
| `home_metric_icons.h` / `.c` | A8 单色图标。走 `make_dim_icon` 的那批**每档屏一份原生尺寸**（48/32/26，`switch` 另加 36/24/19），每枚附一张 `ic_<name>_set[]`：`ic_home_*` `ic_switch_*` `ic_cabin_*` `ic_temp_*` `ic_o2_*` `ic_humidity_*` `ic_pressure_*` `ic_nav_{home,monitor,history,settings,message}_{52,32,50}`（左侧标签栏五枚，路径取自 `tools/icon/Nav_*1`，96 画布的原坐标配 `"32 32 32 32"` 的裁窗 viewBox + 描边 2，尺寸源头是 `USER_NAV_ICON_SZ`）`ic_bell_{60,40,32}` 与 `ic_bell_dot_{60,40,32}`（右上通知铃铛的字形，同样是填充型；设计给的 SVG 是整枚按钮，只取那两条路径，底板/投影由 LVGL 画、红点另是独立对象。**分有无红点两版**：`_dot` 那版右上角咬掉一块给红点让位，两版外接框相同、只有轮廓差，选图见 `build_bell()`；尺寸跟 `USER_LIGHT_BTN_SZ * 5/9`）`ic_barrel_1_{30,20,16}` 与 `ic_barrel_2_{30,20,16}`（首页设备透图上那两颗余量胶囊里的料桶 + 桶号，稿 6201:86004 / 86016；**填充型**，稿的原生画布就是 20 见方、路径坐标已在 0..20，裁窗直接照搬。两枚只差桶号那一笔，但必须是两个独立图标——胶囊分别钉在透图里那两个料桶上，编号要跟桶对上。尺寸跟 `USER_METRIC_ICON_SZ * 5/8`）`ic_folder_{60,40,32}`（PRINT 钮的文件夹，轮廓是 Figma 的「内描边」导出法、靠 `<mask>` 裁，**mask 里那条和可见那条都得写 `stroke="none"`**，漏一条就会被外层 `<g>` 的描边撑大；原先多出的 45/27 两档是给历史页空态那枚 Start Print 按钮的，新稿去掉该按钮后已随尺寸表退回三档）`ic_hist_{clock,film,image}_{48,32,26}`（历史页三个 Tab 的空态图标：虚线圆时钟 / 场记板 / 相框，稿 `6122:80392`/`80601`/`80532`；三枚在稿里画在同一位置，共用裁窗 `"624.5 335 32 32"`，**`hist_image` 是描边型、描边宽 2 由路径自带**）`ic_hist_play_{96,64,51}`（卡片中央播放钮的三角，稿 `6122:80025`；**生成尺寸 = 它底下那个黑 50% 圆的直径**，裁窗取的就是那个 64 见方的圆，三角在里面自然居中，底由 LVGL 画。因此这枚画布绝大部分是透明像素、三档合计 16KB 换一个 15×20 的三角——**是有意的**：稿把三角摆在比几何中心偏右 1.5px 的位置（视觉居中），连画布一起取过来就不必在代码里写这个偏移量，也省掉一次缩放；32MB 的资源目录里这点体积不值得为此换成裁紧 + 手写偏移）`ic_hist_check_{36,24,19}`（多选选中态的白勾，稿 `6073:73736`；生成尺寸 = 底下那个 24 见方蓝块的边长，须与 `USER_HIST_CHECK_SZ` 一致；`stroke-miterlimit="16"` 要留着，拐角是尖的，默认的 4 会削圆）；监控页那批 `ic_plate_{top,level,bottom}_{48,32,26}` `ic_fullscreen_{48,32,26}` `ic_snapshot_{48,32,26}` `ic_purify_ring_{48,32,26}` `ic_door_panel_{48,32,26}`（后者**两处共用**：监控页 Manual 的 OPEN 与首页开门滑块的旋钮，稿上两条路径逐点相同、只差整体平移）`ic_light_{60,40,32}` `ic_chevron_{up,down}_{36,24,19}` `ic_minus_*` `ic_plus_*` `ic_reset_*` `ic_check_bold_*`（都是 36/24/19）`ic_power_{42,28,22}`——这批的路径全是整帧稿的原坐标、靠各自的裁窗 viewBox 取出，**裁窗按墨迹外接框的中心摆而不是按按钮中心**（稿上有几处图标在按钮里本来就不居中，按按钮心裁会把偏移烤进位图）；单档的 `ic_info`（40）`ic_clock` `ic_cube`（24）；手维护那批 `ic_warning` `ic_layers` `ic_expand` `ic_alert` `ic_printer` `ic_purify` `ic_play` `ic_door_solid` `ic_photo` `ic_badge_spin` `ic_badge_play` `ic_badge_check` `ic_badge_alert` `ic_toast_info` | `tools/gen_home_metric_icons.py`（保留式重写，见下方说明） |
| `notice_badge_icons.h` / `.c` | 通知等级徽标图标，每档屏一版：`ic_notice_alert_{14,18,27}` `ic_notice_info_{14,18,27}` | `tools/gen_notice_badge_icons.py` |

> **为什么要每档一份**：LVGL 的 `lv_draw_sw_transform` 是 2 抽头点采样、不做面积
> 平均，缩小时步长大于一个源像素就会整行整列跳过源数据。原先一张 40×40 喂三档，
> 实测横杠 alpha 在 32 档 225..251、26 档 188..220 之间起伏且峰值到不了实心，屏上
> 就是毛刺加断续。改成原生尺寸 1:1 之后横杠是平的 255。挑图统一走
> `ui_user_pick_icon()`（`ui_user_widgets`），尺寸源头是 `USER_METRIC_ICON_SZ`，
> 侧栏那五枚另跟 `USER_NAV_ICON_SZ`。
> **改主题那两个值就要同步改脚本的 `METRIC_SIZES` / `NAV_SIZES`**，否则又会退回缩放。
> 同一套做法见 `notice_badge_icons` 与 `ui_message_store.cpp`
> 的 `ui_message_severity_icon()`。
>
> **`home_metric_icons.c` 里那 14 枚手维护图标**（`ic_warning` `ic_layers`
> `ic_expand` `ic_alert` `ic_printer` `ic_purify` `ic_play` `ic_door_solid`
> `ic_photo` `ic_badge_*` `ic_toast_info`）**没有任何脚本能重新生成**——源矢量已
> 丢失。以前整跑 `gen_home_metric_icons.py` 会把它们连根抹掉、编译当场断；现在脚本
> 改成**保留式重写**（`read_foreign_blocks()` 把不归自己管的块原样读出再写回），
> 整跑已经安全。要改那 14 枚仍只能手改 `.c`。
>
> 归属判定（`is_owned()`）**按前缀认 `ic_<已登记的名字>_<数字>`，不是只认当前
> `ICON_SIZES` 里那几档**。从尺寸表删掉一档时，那一档的旧符号还躺在 `.c` 里，只按
> 精确名匹配会把它当成手维护块原样留下——既是死资源，又会让脚本末尾「保留手维护
> 图标 N 枚」那个计数虚高（踩过一次：`ic_folder` 从五档收回三档，45/27 两块就这么留下了）。
> 所以**那个计数是校验位**：重跑后它若不是 14，多半是有旧档没被回收。
> 通知徽标那两枚早先已摘出去放进 `notice_badge_icons`（源 SVG 在 `tools/_badge_svg/`，
> 可重跑）；其余 12 枚的处置方式待定。
| `settings_icons.h` / `.c` | 设置页分组图标 `settings_icon_*` | `tools/gen_settings_icons.py` |
| `popup_dialog_img.h` / `.c` | 打印流程弹窗插图：门三态 `img_door_opening` / `img_door_opened` / `img_door_closed`、转运盒三步 `img_box_install` / `img_box_moving` / `img_box_shutter`（均 496×321，满宽出血，缩放共用 `control.cpp` 的 `door_set_image()`）、放版两态 `img_plate_loaded` / `img_plate_empty`（320×272，圆角由容器 `clip_corner` 裁）。全部 **RGB565** 不透明，合计约 2.2MB。取代已退役的 `print_flow_img`（那版是 ARGB8888，21MB） | `tools/gen_popup_dialog_img.py`（**自带输入**：读 `tools/pages/popup&toast/image{2,3,4,11,14,15,16}.png`，那些 PNG 是同名稿 SVG 的 2 倍图；**不读 SVG**，因为蓝箭头与高亮描边是叠在位图之上的矢量图元，只抠 `<pattern>` 会把它们丢掉） |
| `powder_dialog_img.h` / `.c` | 打印中三个粉料阻塞弹窗的顶图：`img_powder_empty`（粉舱已空）`img_powder_recycle`（回粉盒满）`img_powder_waste`（废粉盒满）。均 496×321 **RGB565**（三张都是不透明照片，存 alpha 纯浪费），缩放共用 `control.cpp` 的 `door_set_image()`。合计约 933KB。源节点 6345:31129 / 6343:30738 / 6343:30854 | `tools/gen_powder_dialog_img.py`（**自带输入**：直接读 `tools/pages/monitor/printing/image{8,9,10}` 这三份稿 SVG，base64 图片与摆放矩阵都在里面） |
| `ctl_backdrop_img.h` / `.c` | 打印态底部复合控件的玻璃底衬 `img_ctl_bd_{run,collect}_{1920,1280,1024}`（ARGB8888，稿里的 Union 形状，含 1px 白 10% 内描边） | `tools/gen_ctl_backdrop.py` |
| `slide_icons.h` / `.c` | 开门滑块图标 `ic_door_open` | Figma SVG→A8（node 2513:19808） |
| `lv_font_cjk_data.c` | 思源黑体子集 TTF 字节数组（`lv_font_cjk_ttf`，`ui_theme.cpp` 的 `user_font_cjk()` 用 `lv_tiny_ttf_create_data` 运行时渲染）；新增中文需重跑脚本扩子集 | `tools/gen_cjk_font.py`（脚本已不在 `tools/`），源字体 `fonts/SourceHanSansCN-Regular.ttf` |

```bash
# 示例（以脚本内实际参数为准，均从仓库根执行）
python tools/gen_home_metric_icons.py
python tools/gen_popup_dialog_img.py
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

`home_metric_icons` / `slide_icons` 是 **A8 灰度**（只存 alpha），
显示时用 `lv_obj_set_style_image_recolor()` 上色，因此同一份资源可复用于亮态/灰态。
新增图标在 `gen_home_metric_icons.py` 的三处登记后重跑脚本即可：

- `ICONS`：SVG 片段（从 Figma 抠出）。也可以写成 `f(边长)` 的函数，用于需要随输出
  尺寸现算几何的图标，见 `switch_glyph()`（把横杠吸到像素网格）与 `temp_glyph()`
  （给刻度点一个「不小于 1 设备像素」的下限）
- `ORDER`：输出顺序
- `ICON_SIZES`：该枚要出哪几档边长。列进 `METRIC_ICONS` 的会额外生成一张
  `ic_<name>_set[]` 供 `ui_user_pick_icon()` 挑（那个名字里的 METRIC 是历史包袱，
  实际含义是「要出多档原生尺寸的」，侧栏那五枚也在里面）
- `ICON_CFG`：viewBox / 描边宽度。viewBox 可直接给稿的原生画布，不必把坐标折算到
  默认的 32——写成函数的 glyph 自己返回这两项，不用在这里登记。viewBox 还能当**裁窗**
  用：侧栏那五枚的路径是 96 画布的原坐标，给 `"32 32 32 32"` 就把中间那 32 见方的
  图标盒裁出来，坐标一个都不用改
- `ICON_LINECAP` / `ICON_LINEJOIN`（可选）：默认都是圆的；`ic_switch` 按稿用
  `butt` + `bevel`，圆头会让线两端各多伸出半个描边宽、箭头尖发钝。侧栏那几枚是逐枚
  与原 SVG 比对出来的：`nav_history` 稿上没写 linecap（即 SVG 默认的 `butt`），
  `nav_settings` 的齿轮尖要 `miter`，圆角会把齿磨钝
- 单个元素可以用自己的 `fill` / `stroke` 属性覆盖外层 `<g>`，混合填充与描边就靠
  这个（见 `pressure` 的指针轴心、`temp` 的八颗刻度点）。整枚都是填充型的也走这条，
  写 `fill="white" stroke="none"` 即可，此时 `ICON_CFG` 里的描边宽度只是占位（见
  `arrow_right`）
- 从 Figma 拷来的 `stroke-opacity` / `fill-opacity` **必须剥掉**：A8 只存 alpha，
  变淡是运行时 `image_recolor` 上灰色做的，留着会二次变暗

只改其中一枚而不想重跑整个脚本时，import 该模块复用 `render()` + `emit_map()`
单独栅格化，再把结果替换进 `.c` 里对应的 `ic_<symbol>_map` 块。

## 使用

单档图标直接引用；多档的走 `ui_user_pick_icon()`（`ui_user_widgets`）按目标边长挑。
首页留了一个同名短别名 `pick_icon()` 转调，调用点多、写起来短些。

```cpp
#include "home_metric_icons.h"

lv_image_set_src(img, &ic_layers);                       /* 单档 */
make_dim_icon(parent, ic_home_set, USER_METRIC_ICON_SZ); /* 多档，1:1 不缩放 */
ui_user_pick_icon(ic_nav_home_set, USER_NAV_ICON_SZ);    /* 侧栏那五枚 */
```

## 原则

- 仅 `.c` / `.h` 资源与生成脚本说明，不放页面布局代码
- 页面与 components 通过 `#include` 引用，assets 不反向依赖 pages
