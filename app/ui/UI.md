# app/ui/ — LVGL 界面进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_ui`** · 库 **`ui::port`**

## 职责

| 子目录 | 说明 |
|--------|------|
| `lib/core/` | PageManager、`UiMsgProxy`、`control`、摄像头预览、常驻设备信息条 + 共享设备状态、LCD 背光（`ui_backlight.*`，sysfs，不看 MCU 版本） |
| `lib/pages/` | 各 Tab 布局（`page_user_*`） |
| `lib/components/` | 公用 LVGL 控件 |
| `lib/assets/` | 图片、字库 |

## 红线

- **禁止**链接 `app/glavo/lib`、`app/parser/`
- **禁止** pages 直接 `MsgPort::subscribe` / `publish` → 走 **Proxy**
- **LVGL 仅 UI 线程**；后端回调经 `publish_async` + 主循环 `process()`
- 页面注册只在 **`ui_app.cpp`**，不在 `main.cpp` include 各页面头

## 主循环（`main.cpp`）

IPC 收包、`deliver_local`（Proxy 回调）与 LVGL 渲染**同一线程**；渲染前先泵 IPC，避免大画面刷新拖慢 Glavo 控制消息。

```cpp
connect_ipc_with_retry();  // hub + add_ipc_peer
platform_init();
ui::app_init(lv_scr_act());
ui::perf_attach_display(lv_display_get_default());   // 主线程性能探针

while (!g_shutdown_requested) {
    const uint64_t t0 = ui::perf_now_us();
    port.process(8);                        // 带超时的 poll，同时限制一圈的下限
    const uint64_t t1 = ui::perf_now_us();
    const uint32_t delay = lv_timer_handler();
    (void)delay;
    const uint64_t t2 = ui::perf_now_us();
    port.process(0);                        // 同帧 Req/Rsp
    const uint64_t t3 = ui::perf_now_us();
    upc_sys_monitor_tick();
    const uint64_t t4 = ui::perf_now_us();
    ui::perf_loop_sample(t1 - t0, t2 - t1, t3 - t2, t4 - t3);
}
```

后端→后端（如 Glavo→Media 打印录像）由 Hub 在 `process()` 内 **`IPC_FWD` 同步转发**，不经过 UiMsgProxy。预览像素走 dma fd 旁路 + SHM 元数据环，不走 MsgPort payload。

**一圈的时间就是所有 `lv_timer` 的真实周期上限**：LVGL 一次 `lv_timer_handler()` 里每个到期定时器最多只跑一次，不管已经过了几个周期。所以摄像头预览换帧（`ui_timer_cb`，周期写 16ms）实际多久跑一次，取决于这一圈有多长，而不是取决于 16。分段耗时由 `ui::perf_loop_sample` 每 5s 聚合输出，见 [§ 主线程性能探针](#主线程性能探针ui_perf_probe)。

## PageManager 约定

- `Page::name()` 返回唯一 ID（如 `"demo"`），与 `switch_to("demo")` **完全一致**
- `register_page` 只登记；`switch_to` 首次触发 `on_create`
- 离页时在 `on_exit` 里 `register_*(nullptr)` 取消 Proxy 回调

### 外壳布局：侧栏是浮层

`shell_` 仍是横向 flex，但 `menu_panel_`（左侧导航栏）带 `LV_OBJ_FLAG_FLOATING`，
不占 flex 格位，且在 `init()` 末尾 `move_foreground()` 画在最后——所以它**浮在页面之上**。
`content_panel_` 因此铺满整屏，再用 `pad_left = USER_SIDEBAR_W` 让出侧栏，页面按
`LV_PCT(100)` 建出来的内容区与改造前逐像素一致。

两条约束：

- **顺序不能反**。必须先 `FLOATING` 再改 z 序；直接对 flex 子节点
  `move_foreground()` 会改变布局顺序，把侧栏挪到屏幕右侧。
- **想整屏出血的页面自己跨过那段 padding**，用负偏移 + 宽度取 `LV_HOR_RES`
  （`LV_PCT(100)` 算的是父内容区，不含 padding）。目前只有首页的摄像头层这么做，
  见「摄像头整屏预览」。侧栏可点，压住的区域连触摸都收不到，所以左对齐的控件
  要补 `USER_SIDEBAR_W`。

`menu_panel_` 的 `bg_opa` 是 `LV_OPA_90` 而不是原来的 `LV_OPA_40`：整屏预览时侧栏
背后是视频，未选中图标 `user_color_icon_off`（`#555555`）的底色由当前帧决定，亮帧
下会被盖过去。其余页面背后是 `user_color_bg`（`#0a0909`，近纯黑），提高不透明度肉眼
无差别。调色解决不了这个问题——背景不可控，任何固定灰度都会在某些帧上失效。

#### 整屏底色是**按页**切的

底色只画在 `shell_` 一层（`content_panel_` 与各页根节点都是 `LV_OPA_TRANSP`），
两档由 `set_shell_light()` / `apply_shell_style()` 切，与侧栏那套
`set_sidebar_video_mode()` / `apply_sidebar_style()` 同构：

| 档 | 色 | 用在 |
|---|---|---|
| 浅 | `user_light_bg` `#fafafa` | **已按浅色稿改过的页面**：首页、历史页、消息页、设置页 |
| 深 | `user_color_bg` `#0a0909` | 控制页 + 所有隐藏页 |

**为什么是开关而不是直接全局翻浅**：浅色稿是一页一页改过来的。控制页还是深色内容
（白字、半透明白卡），底一翻成浅色那页当场就读不出字了。它改稿时把页名加进
`apply_device_bar()` 末尾那句即可，改完就可以把开关连同这一节一起删掉。

底色值本身也跟着新稿走过一次：首版浅色稿（6039 那批）写的是 `#f5f5f5`，新稿三处页面帧
（历史 6251:94118 / 消息 6230:89616 / 6091:13847）一致改成了 `#fafafa`。两者只差 5/255。

置位统一在 `apply_device_bar()` 里做（每次 `switch_to()` 都会走），页面自己不用调。
`switch_to()` 的顺序是 `on_exit()` → `apply_device_bar()` → `on_enter()`，所以新页的
`on_enter()` 跑之前底色已经就位，不会闪一帧旧色。

两个连带事实：

- **页面不必自己铺底**。历史页根节点是透明的 `LV_PCT(100)`，全靠这一层。早先想过让
  它自己盖一层浅色，但页根挂在 `content_panel_` 里、只覆盖内容区，侧栏那 128 和顶上
  `USER_CONTENT_TOP` 那条会留一个黑色的 L 形边；画在 `shell_` 上就没这问题。
- **首页那层 `build_main_area()` 的浅色底不能删**，虽然现在底色重复了：它还兼着裁掉
  出血的设备图（`663 + 700 = 1363 > 1280`），删了设备图会溢出屏幕。
- 侧栏不用跟着改。它已经是浅色档（白 4% + 投影），之前显得暗只是因为压在黑底上。

### 间距梯度（1280 档已对齐设计稿）

稿上的间距落在 8 / 16 / 20 / 24 / 32 / 40 这套网格上，而工程只有三档宏。1280 档现在取：

| 宏 | 1280 | 稿上对应的地方 |
|---|---|---|
| `USER_PAD_SM` | 16 | 侧栏项距、首页左下两枚入口的间距、HOME 卡与四宫格的间距、toast 内边距与图文间距、历史页卡片间距 |
| `USER_PAD` | 24 | 四宫格单卡内边距、消息卡内边距与列间距、首页说明小字与卡的距离 |
| `USER_PAD_LG` | 32 | 首页左边距、各浮层退的屏幕右/下边距、历史页 Tab 间距 |

派生出来的两档也常用：`USER_PAD_SM / 2` = 8（四宫格格间距、toast 卡间距、问候语两行间距）、
`USER_PAD_SM * 3 / 4` = 12（面板内边距、面板条目的图文间距）。

**1024/1920 两档没动**（仍是 11/22/36 与 20/40/64），所以出货的 1024 版本不受影响。副作用是
1280 档的 `USER_PAD_LG`(32) 比 1024 档的 36 还小——非单调，说明 1024 档那套本身也偏大
（稿 32 × 0.8 = 25.6），等以后单独对 1024 那一轮再收拾。

浅色稿里到处用的那个 **24 圆角**落不回这套梯度（`USER_RADIUS` 本档才 14），单开了
`USER_LIGHT_RADIUS`；toast 的圆角 8 走 `USER_BTN_RADIUS`，面板条目的 12 取
`USER_LIGHT_RADIUS` 的一半。

### 侧栏几何（稿 5339:27264 的「Home bar」）

稿上侧栏是「竖向居中 + 固定间距」的一列方块，1280 档四个数：

| 项 | 稿值@1280 | 主题字段 |
|---|---|---|
| 侧栏宽 | 128 | `USER_SIDEBAR_W` |
| 导航项（可点方块）边长 | 96 | `USER_NAV_ITEM_SZ` |
| 项间距 | 16 | `USER_NAV_GAP` |
| 图标盒边长 | 32 | `USER_NAV_ICON_SZ`（字形在盒里占 30，见下） |

128 = 16 + 96 + 16，故侧栏左右内边距不再借用 `USER_PAD_SM`，改成按
`(USER_SIDEBAR_W - USER_NAV_ITEM_SZ) / 2` 反推——`USER_PAD_SM` 是通用小间距、跟侧栏
几何无关，两者绑在一起时改哪边都会牵连另一边。

`menu_panel_` 因此用 `LV_FLEX_ALIGN_CENTER` + `pad_row = USER_NAV_GAP`，不再是
`SPACE_EVENLY`：稿上 5 枚 96 的项加 4 道 16 的缝共 544，居中落在 800 高的屏上，而
`SPACE_EVENLY` 会把间距按屏高摊开。`pad_top`/`pad_bottom` 仍取 `USER_PAD` 且两者相等，
所以在内容区里居中等价于在整条侧栏里居中，它们只作项撑满时的最小留白。1024/1920 两档的
`USER_NAV_GAP` 取的是能复现各自原 `SPACE_EVENLY` 观感的值（600 高下 44、1080 高下 100），
换档观感不变。

**`USER_NAV_ICON_SZ` 就是图标画布边长本身**，资源按这三个数（52 / 32 / 50）各出一份原生
尺寸、上屏 1:1 不缩放。所以**改它必须同步改 `tools/gen_home_metric_icons.py` 的
`NAV_SIZES`**，否则 `ui_user_pick_icon()` 挑不到等宽的就退回缩放，2px 的细描边会被 LVGL
的 2 抽头点采样采成毛刺（同一个坑的完整交代见 `lib/assets/README.md`）。

三个数的来历：新资源的「字形 / 画布」= 30/32 = 0.9375。1280 档取稿值 32，字形上屏就是稿上
的 30；1024/1920 两档这一轮不对稿、只保持换图标前的上屏字形大小（≈47 / 49），除以 0.9375
反推得 50 / 52。代价是 1024 档的描边会从约 1.7px 变成 `2 × 50/32 ≈ 3.1px`——这是新稿
「描边/尺寸 = 2/32」这个比例带来的，不是 bug，但**上板观感需实机核**。

早先这个字段是另一个意思：那时只有一份 64×64 的资源、字形在里面占 52，它是「把整张 64
缩到多大」的目标值（1280 档的 59 是按旧稿字形 48 反推的 48 ÷ (52/64)）。那套反推已随
「按档位出原生尺寸」作废，**别再按「字形 ÷ 占比」去算**。

### 侧栏配色与图标两态（浅色稿 6073:72801）

侧栏本身几乎是透明的：底 `white @ 4%`，与页面的分界全靠右侧那道
`#0C1C61 @ 4%` / offset(2,0) / blur 6 的**外投影**划出来，所以投影不能省。

图标两态只差颜色，都是同一枚 A8 靠 `image_recolor` 上色：

| | 稿 | 实现 |
|---|---|---|
| 选中 | 白字形 + `#333145` 实色底板、圆角 32 | 同稿。底板由按钮自身的 `LV_STATE_CHECKED` 绘制 |
| 未选中 | `#9E9E9E` | 同稿（整屏视频档除外，见下） |

底板圆角取 `USER_NAV_ITEM_SZ / 3`（1280 档 96/3 = 32，正是稿值）——跟着项边长走，
不借用 `USER_RADIUS`，后者是通用卡片圆角（1280 档才 14）、和导航项没关系。

底板还带一道外投影 `#333145 @ 20%` / offset(0,4) / `shadow_width = 12`。那个 12 是从稿导出的
`feGaussianBlur stdDeviation="6"` 换来的——**SVG 的 stdDeviation 是标准差，视觉模糊约是它的
两倍**，直接照抄 6 会细一半。

**这道投影必须只挂 `LV_STATE_CHECKED`。** LVGL 决定画不画影子只看 `shadow_width` /
`shadow_opa`，**压根不查 `bg_opa`**（见 `lv_draw_rect.c` 里 `has_shadow` 那段，`bg_opa` 走的
是另一条独立的 `has_fill`）。挂在默认态上五枚就全带影子；更糟的是未选中那四枚底是透明的、
`bg_cover` 为 false，`lv_draw_sw_box_shadow.c` 里那句 `simple = dsc->bg_cover` 不成立，影子
连按钮正中间也画出来——屏上是每枚未选中项底下压着一团完整的深色柔光，看着像浮起来。
（这条踩过：早先按「透明底不会有影子」写在默认态上，实机上五枚全浮着。）

**字形来源**：`tools/icon/Nav_*1` 那五个 SVG（未选中那一版，只有一条 path、没有底板），路径
坐标原样收进 `gen_home_metric_icons.py`，靠 `"32 32 32 32"` 这个裁窗 viewBox 从 96 画布里取出
中间的图标盒。两处容易踩：

- home 要取 `Nav_Home_Active1`。`Nav_Home_Active2` 是 120 画布、底板摆在 (12,8)，路径整体偏移
  了 +12/+8。
- `Nav_Home_Active1` 的 path 外面裹着一层 `<g filter=...>`（就是上面那道投影），**必须剥掉**：
  A8 只存 alpha，投影混进去会把字形整体往下拖（实测 bbox 从 (33,32,63,63) 变成 (33,36,63,67)）。
  投影归 LVGL 画。另外五个导出里只有 Home 带这层 filter，那是导出差异不是设计意图——稿上投影
  挂在「选中」这个变体上，而任一时刻只有一项选中。

**整屏视频档**（首页打印中 / 监控中）走 `PageManager::set_sidebar_video_mode(true)`：
底退回 `user_color_panel @ 90%`、未选中图标退回 `user_color_icon_off`（`#555555`）。
理由是侧栏背后那时是视频帧，4% 的白压不住亮帧，`#9E9E9E` 会被盖过去——这正是当年把
`bg_opa` 从 40% 提到 90% 的原因。换页时 `apply_device_bar()` 一律复位成浅色档，首页
在 `on_enter` 里按当前态再置一次。

**改版前那套渐变蒙版已整体退役**。那时稿上选中态没有底板、只有一枚蓝渐变字形，而 A8
只存 alpha、`image_recolor` 只能上单色，于是每个按钮里建了两层互斥图标（`menu_icon_img`
/ `menu_icon_grad`），选中那层是一块铺了渐变的方块、用同一份 A8 当 `bitmap_mask_src`
裁成字形。连带三个 LVGL 的坑（蒙版按原尺寸居中贴不缩放、`transform_pivot` 默认在左上角、
窄侧栏档位要放开 `LV_OBJ_FLAG_OVERFLOW_VISIBLE`）也一并没了。**要做渐变字形再回来翻
git 历史**，别凭印象重写。

**按压反馈分两档**（稿里没有这个元素，是工程自己加的交互反馈）：按未选中的标签是
白 20%，按已选中的标签是主色蓝，靠 `LV_STATE_CHECKED | LV_STATE_PRESSED` 这个组合
选择器区分。LVGL 取「状态位是当前状态子集」里状态值最大的那条，`CHECKED`(1<<2) +
`PRESSED`(1<<7) = 132 > 单独 `PRESSED` 的 128，所以组合态一定胜出。

**`LV_STATE_CHECKED` 那档必须显式给 `bg_color` + `bg_opa`，不能只是「不设」**。这是实机
踩过的坑：`lv_btn_create` 会带上默认主题，而主题给每个普通按钮的 checked 态都挂了
`bg_color_secondary`（副色 + `LV_OPA_COVER`，见 `lv_theme_default.c` 的 apply 分支），
副色是 `lv_display.c` 自动初始化主题时传的 `LV_PALETTE_RED`——不压住的话选中态是一块
不透明红底。早先稿上没有底板，是靠把 `bg_opa` 写成 `LV_OPA_TRANSP` 来压制的；现在稿
自带底板，局部给一个不透明的 `#333145` 同样盖得住，那个绕法已经退掉。

这类「清理样式反而让默认主题从背后冒出来」的问题，根治办法是菜单按钮改用
`lv_obj_create` 或建完就 `lv_obj_remove_style_all()`，代价是按压反馈等观感要重新核一遍。

### 常驻设备信息条（`ui_device_bar` + `ui_device_state`）

顶部那一条不属于任何页面，由 `PageManager::init()` 建在 `shell_` 上（`content_panel_`
之后 → 恒定压在所有页面之上，含首页的整屏摄像头层；`menu_panel_` 仍在它之上）。它同样是
`FLOATING` + `pos(0,0)`，否则会占掉 `shell_` 横向 flex 的一个格位把内容区挤走。条自身
`remove_flag(CLICKABLE)`，不然整屏宽的带子会吃掉底下控件的触摸。

**浅色稿 6201:85956 把它改成了两组设备**：`WiFi 图标 + [SnowPod · Stand-by] + 竖分隔线
+ [NitroPod · Connected]`。几何按稿（1280 档）：

| 项 | 稿值 | 实现 |
|---|---|---|
| 左缘 | 160 | `USER_SIDEBAR_W + USER_PAD_LG`（**不是 `USER_TOPBAR_PAD_HOR`**，那个算出来是 156；它还被首页那条顶部占位带用着，不动） |
| 图标/组/分隔线之间 | 10.667 | `USER_PAD_SM * 2 / 3` = 10 |
| 组内：名字 ↔ 状态 | 16 | `USER_PAD_SM` |
| 状态点 ↔ 状态字 | 8 | `USER_PAD_SM / 2` |
| 状态点直径 | 8 | `USER_STATUS_DOT_SZ * 2 / 3`（三档 12/8/6）。**不动那个共享字段**：调试页、`ui_user_widgets`、侧栏红点、通知徽标都在用 |
| 分隔线 | 1×20 `#d9d9d9` | `USER_STATUS_DOT_SZ * 5 / 3`（三档 30/20/16）+ `user_light_divider` |
| 名字 / 状态字号 | 22 | `user_font_body()`（1280 档 24，最近的一档） |

**本条一律走浅色配色**，不按页面分档。这一点要留意：目前浅底的是首页待机态、历史页、
消息页与设置页（见「整屏底色是按页切的」），控制页仍是 `user_color_bg` 的 `#0a0909` 深底、
首页打印/监控态底下是整屏摄像头，那几处的观感要等它们也翻成浅色才正。
不过历史页与消息页都**不显示这条**（稿上顶部直接是 Tab 行），实际只有首页在看它。

配色上有三处是从深色档换过来的，**换的原因都是「白/亮色压在浅底上看不见」**：

- 设备名 `user_light_dev_name`（`#1d1d24`）。改版前用的是 `user_color_text`，那是纯白，
  实机上整个设备名是隐身的（看着像「WiFi 图标 + 一大段空白 + 状态字」）。
- WiFi 图标连上那档也从 `user_color_text` 换成 `user_light_text`。实机上它能看见纯属当时
  没连网走的是灰色那档，**一连上就会跟着消失**。
- `ui_device_status_color()` 整个函数换成浅色档的值，绿那档尤其关键：`user_color_status_ok`
  的 `#52ff92` 是给深底调的亮薄荷绿，浅底上几乎看不见，换成 `user_light_ok`（`#00c74e`）。
  那个函数**只有本条在用**（状态点与状态字两处），所以整体换掉没有外溢。

**第二组的数据源还没接通**：`ui_device_bar_create()` 里先按稿写死 `NitroPod / Connected`，
接口是 `ui_device_bar_set_peer(name, status, level)`，接上之后把那一行挪到数据更新处即可。
`level` 只决定配色、故意只有 OK / ERROR / OFFLINE 三档——稿上只画了 `Connected` 一态，
多余的等级等知道那台设备到底有哪些状态了再加。名字传空则整组连同分隔线一起收起来。
**不复用 `ui_device_status_t`**：那是本机打印机的状态机（Idle / Printing / Paused ...），
`Connected` 不在里面，第二台设备的状态集大概率也不是同一套。

- **让位**：`content_panel_` 的 `pad_top = USER_CONTENT_TOP`，页面按 `LV_PCT(100)` 建出来
  的内容区自然落在条下面。注意**不是** `USER_TOPBAR_H`，两者已拆开，见下。
- **显隐规则**：`apply_device_bar()` 按「侧栏有菜单按钮 = 显示」判定，即五个标签页显示，
  隐藏页（选文件 / 调试 / 延时摄影）不显示且 `pad_top` 收回 0。以后新增隐藏页默认不显示。
- **历史页是唯一的例外**：浅色稿 6122 一族五帧的顶部都没画这条，所以那一页按页名排除掉，
  `pad_top` 跟着收回 0、整屏高度还给页面，Tab 行到屏顶那段留白由页面自己的 `pad_top`
  （`USER_HIST_PAD_L`，稿上左右上三边同为 48）给。代价是在历史页看不到设备名与联机状态，
  这是照稿的取舍。别的页要照做的话记得两件事一起做：排除 + 自己补顶内距。
- **弹窗**：条在 `shell_` 内，打印流程/传输弹窗的全屏遮罩建在 `screen_` / `lv_layer_top()`
  上，会连它一起压暗，与页面表现一致。
- **首页例外**：首页要整屏出血，根节点用 `pos(-USER_SIDEBAR_W, -USER_CONTENT_TOP)` 跨过两个
  方向的 padding（纵向抵消的是 `pad_top`，所以跟的是 `USER_CONTENT_TOP` 而非条高，用错会
  整页竖直错开二者之差），尺寸取 `LV_HOR_RES` × `LV_VER_RES`（`LV_PCT(100)` 算的是父内容区，
  会被 `pad_top` 扣掉一条）。首页自己那条 `USER_TOPBAR_H` 高的透明行现在是**空占位**（稿
  5277:11571 起右侧那两颗按钮已移到左下的方块上），但不能删——首页根节点是纵向 flex，
  其余几层都是 `FLOATING` 不占格位，它是唯一撑起那一格的子节点。
- **`USER_TOPBAR_H` = 屏高 16%**（1024×600 档 96、1280×800 档 128）。调试页那条传感器卡
  原先借用该字段，现在用 `USER_SENSOR_STRIP_H`（值即旧的 `USER_TOPBAR_H`），不随带子变高。
- **`USER_TOPBAR_H` 与 `USER_CONTENT_TOP` 是两回事**（1280 档 128 / 112）。前者是条自身的高，
  条内控件竖直居中，所以它决定设备名那行的行心（稿 4366:5400 / 3033:5247 / 3238:14296 三张
  稿里设备行都是 48..80、心 64，对应条高 128）；后者是页面内容的起点，稿上历史页与消息页的
  Tab 文字都顶在 y=112、分隔线 152。稿里设备行底（80）到内容顶只隔 32，两者本就不等，
  原先共用一个字段导致内容被压低 16。**只改一个字段解决不了**：把它整体降到 112，设备名那行
  会跟着上移 8。故拆成两个，各自按稿取值。
  - **设置页是例外**：它是两栏导航、没有 Tab 行，两栏各自的顶内距由页面自己给
    （左栏 48、右栏 72，见「设置页布局」），全局字段一个都不用。

数据侧：`ui_device_state` 是设备名/联网/运行状态/环境指标的唯一来源。
**页面不要自己 `register_env_update`** ——它是单槽的（`env_update_cb_ = std::move(cb)`），
第二个订阅者会静默顶掉第一个。订阅在 `ui_app.cpp` 的 `ui_device_state_init()` 做一次，
再扇出给观察者（当前是常驻条 + 首页）。打印中/结果页的状态由前端用
`ui_device_state_set_status()` 置位并**锁住**，不让环境推送覆盖。
副产品：设置页/历史页原先各自画一条写死 "Ready" 的 header，现已删除，状态跟着后端走了。

被锁住（前端置位、环境推送不得覆盖）的状态：`Printing` / `Paused` /
`Print Complete` / `Print Terminated` / `Air Purifying` / `Purify Complete`。
后三个由打印态浮层的 `apply_print_phase()` 按 `g_print_phase` + `g_purify_stage`
推过来，取件退出打印态时统一还原成 `Ready`。

### 消息中心与通知三态（`ui_message_store` + `ui_notice_stack`）

设备事件/告警的显示通道是首页右上角那块地方，按浅色稿做成 **toast / 铃铛 / 面板三态
互斥**。它和 toast 组件（`ui_feedback` 的底部提示）分工不同——**那个是操作反馈，看完
就走；这里是设备发生的事，要能回头翻**。两者互不替代。

数据在 `core/ui_message_store`（同 `ui_device_state` 的形态：单例 + 观察者），
三个来源：

1. 后端 `GLAVO_Device_Event_Ntf`（`DeviceEventPayload`：码 + 等级 + active + 触发时刻），
   码表与 [`doc/设备通信协议-数据结构定义（v0.9）.md`](../../doc/设备通信协议-数据结构定义（v0.9）.md)
   第五节那张表一一对应。`active=true` 走 `ui_message_emit()`、`active=false` 走
   `ui_message_retract()`；
2. 前端本地消息，码段从 `DeviceEventCode::UiWearMask`(512) 起，后端不发这一段。
   目前是打印结束那两条（催吹粉 / 提醒戴口罩），由 `apply_print_phase()` 推和撤；
3. MCU 报警（`core/ui_alarm_state`，见下节「MCU 报警」）：文案在 UI 侧由字典生成，
   走 `*_ex` 那一支（外部文案/等级版本，文本指针须指向调用方的稳定存储，
   `ui_alarm_state` 用自有定长缓冲保证这一点）。

**文案不进线协议**：总线上只跑数值码，人话由 `ui_message_store.cpp` 的 `kCatalog` 出。
等级也由码表定，不用 payload 里那个 `severity` 字段——那个是留给云端的，同一个码在
UI 上必须恒定同一种观感，否则后端改个字段卡片就换色了。当前文案是按稿意写的英文占位。

后端那条回调跑在总线线程上，而每一次变更都会重建 LVGL 对象，所以 `on_device_event`
先 `lv_async_call` 切回 UI 线程再落库（payload 里有个 64 位时间戳，塞不进
`lv_async_call` 那个指针，只能堆上过一手）。**界面上那几颗按钮同理要异步**：关闭钮、
铃铛、面板里的按钮，动作最后都会走到 `rebuild()`，而它第一件事就是 `lv_obj_clean`，
把正在派发事件的自己一起删掉。

#### 写入分两种语义：sync 与 emit

这是本模块最容易改错的地方。调用方本来就分两类，对「又发生了一次」的期待正好相反：

| | 语义 | 调用方 |
|---|---|---|
| `ui_message_sync()` / `_sync_ex()` | upsert：本地没有就新建并弹一次，**已有就静默更新**——不碰 visual、不动已读与排序、不重弹 | `apply_print_phase()` 那两条本地提示、30s 报警快照对齐、CONDITION 同一轮内的状态迁移 |
| `ui_message_emit()` / `_emit_ex()` | 又发生了一次：无条件拉回 `SHOWN`、回未读、序号顶新、重弹 | MCU 的 EVENT 每帧、CONDITION 从 `RTN_LATCHED` 回到 `ACTIVE` 那个上升沿、后端设备事件 `active=true` |

**sync 必须不碰 visual**。`apply_print_phase()` 打印期间每秒调好几次、快照轮询 30s 一轮，
要是它们能把已经收起的消息顶回 toast，屏上就是周期性刷屏。改版前这两支合在一个
`push_ex(renew)` 里，而 `renew=false` 仍然无条件执行 `dismissed = false` 和
`notify_observers()`，等于那个参数没真正分开——**别再合回去**。

排序用内部 `seq` 而不是 `ts_ms`：后端给的是 Unix 时间、本地消息压根没有时间，
两种纪元混着比大小会把本地消息全排到最旧。

#### 自动收起（`visual` 三态）

`ui_message_t::visual` 分三档，**两种离开 toast 的原因必须分开记**：

- `UI_MSG_SHOWN`：正挂在 toast 上；
- `UI_MSG_CLOSED_BY_USER`：用户点了关闭钮（只有 INFO 有这颗钮），走 `ui_message_dismiss()`；
- `UI_MSG_AUTO_COLLAPSED`：停留到时限自动收进铃铛，走 `ui_message_collapse()`，不看等级。

只有 `CLOSED_BY_USER` 会让 `sync` 把消息重新顶出来——自动收起是系统行为、不代表用户
处理过。合成一个 bool 就是上面说的刷屏。

计时在 store 一侧：每条进入 `SHOWN` 时记 `shown_tick`，一支 500ms 的扫描定时器按等级
到点收起（INFO 5s、告警级 15s，见 `kCollapseMs*`）。**放在 store 而不是显示层**，是因为
待机态的新三态与打印态的旧浮层共用同一份数据，时序必须一致。用一支全局扫描而不是每条
挂一个 one-shot，是因为条目会被 evict/retract 删掉，那样会留下一堆指向空槽的定时器。

`ui_alarm_state` 里原先那支 EVENT 专用的 5s 定时器已经撤了：留着会和这边打架，同一条
EVENT 被两个时长各撤一次。而且它一直只对 INFO 生效——撤下走的是 `ui_message_dismiss()`，
那个函数第一道门就把非 INFO 挡掉了。

#### 三态（`core/ui_notice_stack`）

| 态 | 何时 | 稿 |
|---|---|---|
| TOAST | store 里有 `visual == SHOWN` 的消息 | 6201:86044：白卡 400 宽、圆角 8、内距 16、32 徽标 + 18 白图标；右侧 24 见方、圆角 8、底 `#333145` @ 5% 的关闭方块（**只有底板是稿上的，里面那个叉借的是 `LV_SYMBOL_CLOSE` 字形**）；最多两张 |
| BELL | 没有 toast 时的常态 | 72 见方、圆角 24、白底 + 40 的实心铃铛（按钮 72 配图标 40，故按 `USER_LIGHT_BTN_SZ * 5/9` 挑档得 60/40/32）；有「还需留意的事」就带红点，见下 |
| PANEL | 点铃铛展开 | 6201:86200：320 宽、圆角 24、内距 12；条目 48 高、圆角 12、左侧 4×32 等级色竖条；底部「查看剩余 N 条」 |

三者占同一块地方（稿上它们的右缘都在 1232），任一时刻只有一个可见——**稿里那几层图层
互相重叠不是画错了**，就是这个意思。新消息到达时把展开的面板顶掉，回到 TOAST。

| 项 | 取值 |
|---|---|
| 位置 | `shell_` 上的 `FLOATING`，与常驻设备信息条同级 → 压在页面之上、弹窗层之下 |
| 对齐 | 贴右上角，退一个 `USER_HOME_EDGE_GAP`（稿 48）→ 铃铛落在稿的 (1160, 48)。**这个值同时管三处**：铃铛/toast/面板的右缘、右下开门滑块的右缘（稿上都是 1232）、以及首页打印态右上那块环境区（`build_env_wrap`，本栈就叠在它下方）。早先三处退的是 `USER_PAD_LG`（32），实机上铃铛比滑块往外多出 16、顶也高 16。纵向让位量按态给，见 `sync_notice_offset()` |
| 宽度 | `USER_NOTICE_CARD_W` = **400@1280**（toast 那张卡的宽）。铃铛 72、面板 320 都比它窄，靠容器的交叉轴 `FLEX_ALIGN_END` 右对齐，所以一个宽度够三态共用。**稿自己在两个 frame 里不一致**：待机 5339:27299/27310 是 400，打印态 4972:27560 的 5281:11797 是 416（与 stats-card 同宽）；本块是全局单例，只能取一个值，以待机 frame 为准 |
| 标题 + 副标题字号 | toast 两行都取 `user_font_text_sm()`（1280 档 16）、面板两行都取 `user_font_caption_sm()`（12）。稿上每组二者**同号**、只靠字重（SemiBold / Regular）和 60% 透明度分层；Montserrat 只有一个字重，字重那半做不出来，只落地「同号 + 透明度」。面板正文稿上是 10，字体表最小到 12 |
| 条数 | toast 最多两张；面板最多三条，多出来的收进「View remaining N messages」，点进消息页 |
| 徽标 | 四档等级配色 + 图标，见下「通知徽标配色」；与消息页列表共用一份映射 |
| 关闭钮 | **只有 INFO 有**（稿上只有信息卡带）。告警级靠自动收起离场 |
| 铃铛红点 | 稿里它是铃铛那枚 SVG 的一部分：`circle(50.5, 34.5) r=4.5` 落在 72 的按钮里，即直径按钮的 1/8、圆心在按钮坐标 (44.5, 30.5)——**挂在字形右上方、在按钮内部**，不是骑在圆角外面。色 `#FE6868`。它没烤进 A8：一来颜色与字形不同，二来只在有未读时出现，所以仍是独立对象 |
| 铃铛字形两版 | **稿上铃铛分有无红点两个变体**，`build_bell()` 按 `g_badge_on` 选：`ic_bell`（稿 6212:87571，完整轮廓）与 `ic_bell_dot`（稿 6201:85994，右上角咬掉一块给红点让出一圈空隙）。差异区落在 84 画布的 x 46.6..53.5 / y 29.2..40.0，正压在红点上。**两版外接框完全一样**，拿错了不会错位、也不会从尺寸上看出来，但常态下用带缺口那版，铃铛右上就是个没来由的豁口 |
| 显隐 | **只在首页**（见 `apply_device_bar()`） |

**面板的取数与两颗红点共用 `ui_message_needs_attention()`**（「没看过 **或** 是还没解除的
告警级」），所以**红点亮与面板里有东西恒等价**，不会出现点开是空的。没红点时点铃铛
直接跳消息中心，不弹空面板。后半句不用另外判断解除与否：CONDITION 解除时
`ui_alarm_state` 会 `ui_message_retract()` 把条目删掉，还在表里就等于还没解除。

**捕获层**（点面板外收起）：全屏、全透明、可点，**建在 `shell_` 上而不是
`lv_layer_top()`**。弹窗都在 top 层，捕获层放上去会把「面板 vs 弹窗」的层级关系变成按
创建时序碰运气——面板展开时突然冒出来的 WiFi 配对弹窗要么点不到、要么被当成「面板外」
一点就把面板收了。两个顺序约束：`FLOATING` 要在 `move_foreground` 之前加（`shell_` 是
横向 flex，直接对 flex 子节点改 z 序会连布局顺序一起改）；捕获层先提到最前、面板再提
一次，两者都要盖过侧栏，否则点侧栏收不起面板、会直接切页把面板留在那儿飘着。收起后
调 `PageManager::raise_sidebar()` 还原次序。

**右上角是有人抢的**：首页打印态 / 待机监控态那个位置是环境区（`build_env_wrap()`），
整块用 `ui_notice_stack_set_top_offset()` 压到它下面去。让位量在 `page_user_home.cpp` 的
`sync_notice_offset()` 里一处算完（取 `env_block_t::root` 的高度，`LV_SIZE_CONTENT`
算出来的，取值前必须先 `lv_obj_update_layout()`，否则首次进打印态拿到 0 会压在环境区
上）。`apply_device_bar()` 换页时一律清零，首页在 `on_enter`（即 `page_user_home_refresh`，
进出打印态/监控态也都会走到）里按当前态再让一次。

**打印态 / 监控态仍是改版前那摞黑玻璃卡**（`ui_notice_stack_set_dark(true)`）：那两个态
背后是整屏摄像头画面、页面本身还是暗色的，浅色稿那套白 toast 与铃铛都不合适，所以没有
铃铛也没有面板，消息一直摆着直到被收起或撤回。等打印态的新稿到位再统一。

作用域收窄成「只有首页」之后，控制页不再显示浮层了（改版前它是显示的），新消息只靠
侧栏红点。历史页与设置页本来就排除——那个位置是 Storage 容量条和 Wi-Fi 的 On/Off 开关，
**开关可点，被浮层压住连触摸一起吃掉**。曾试过降级成单行细条，但细条底是半透明的，
与 Storage 条叠印两层字都读不清；往下移又会压到 `Manage` 按钮和那个开关，故放弃。

**两颗红点**（侧栏消息图标 + 首页铃铛）由 `ui_app.cpp` 的 `sync_message_badge()` 一处
驱动，判据就是上面那个 `ui_message_needs_attention()`：进过一次消息页只消未读，
**告警要处理完才灭**。它比改版前的「未读 || `ui_alarm_current_count() > 0`」收紧了两个边：
旧判据只看 MCU 报警、漏掉后端推来的告警级事件，而且不分等级、一条 INFO 级的 CONDITION
也会让红点常亮。红点是叠在菜单按钮上的一个小圆 `lv_obj`，不是重出一版图标资源——稿里是
把红点烤进 SVG 的，那样没法按未读数开关。

**两颗红点的稿值不同，别强行拉齐**：铃铛那颗是 9/72（按钮 72 配 r=4.5），侧栏那颗是
8/96（导航项 96 里 `circle(60,36) r=4`，稿 6255:95797），比例本来就不一样，各按各的稿。
**统一的只有颜色** `#FE6868`（`user_light_notice_dot`）——侧栏那颗原先填的是深色档的
`ui_color_danger` `#F85149`，偏暗偏红，并排能看出来。

**侧栏消息图标也分有无红点两个变体**，与铃铛同一套做法：`ic_nav_message_set`（完整轮廓）
与 `ic_nav_message_dot_set`（稿 6255:95805，右上角咬掉一块给红点让位）。两版由
`register_page()` 的第二个 set 参数登记、`set_menu_badge()` 按未读切换；两版墨迹外接框
逐像素相同（headless 量过 x 1..30 / y 2..30），切换时不会跳。没登记变体的图标只叠红点，
红点会压在原轮廓上。

**后端还没实现 `GLAVO_Device_Event_Ntf` 的发布**，接好的回调目前收不到任何消息。
要验「超过两条 + 查看剩余 N 条」这条路径，用调试页 Other → Inject → `Msg Inject`
（造 4 条假消息：1 条 critical + 3 条 info）。

### 通知徽标配色（消息页列表 + 通知三态共用）

唯一来源是 `ui_message_severity_color()` / `ui_message_severity_icon()`
（`core/ui_message_store`）。**两处必须共用**：同一条消息在 toast / 面板竖条和消息页
列表里不能是两种颜色。报文 severity（1..4）先经 `ui_alarm_severity_to_msg()` 换成
`ui_msg_severity_t` 再取色，报警与设备事件因此落在同一套阶梯上。

| 等级 | 语义（协议第 7 节） | 色 | 图标 |
|---|---|---|---|
| `UI_MSG_SEV_INFO` | 1 information 信息提示 | 蓝 `#485DFF` | 对话气泡 |
| `UI_MSG_SEV_ERROR` | 2 warning 警告 | 黄 `#FFC942` | 警告三角 |
| `UI_MSG_SEV_CRITICAL` | 3 error 故障 | 橙 `#FF7843` | 警告三角 |
| `UI_MSG_SEV_EMERGENCY` | 4 critical **安全停机** | 红 `#FF4242` | 警告三角 |

中间两档取的是浅色稿 6064:71936 变量表里的 `Color 3` / `Color 2`，与改版前自己填的
`#FFD426` / `#FF8A3D` 只差一点，色相阶梯不变。第四档红稿上仍没有样例，沿用自填值。

图标资产是 `notice_badge_icons`（源 SVG 从稿 3058:7234 / 3058:7059 导出，存在
`tools/_badge_svg/`，`tools/gen_notice_badge_icons.py` 可重跑）。**每档屏各出一版**
（14 / 18 / 27 px，对应徽标边长 26 / 32 / 48 的 9/16），`ui_message_severity_icon()`
按 `g_ui->profile_id` 选。这样 `ui_user_make_icon_img()` 里 `target == src_sz`、
缩放整段被跳过——原先共用一张 36px 位图，1024 档要缩到 0.39 倍，细描边糊成一团。
**改尺寸表要两边同步**（生成脚本的 `SIZES` 与那个 switch）。

两条不要改回去的理由：

- **相邻两档不能同色相靠明度区分**。原先 3/4 是红 `#FF4242` + 深红 `#D32029`，
  实机上分不出来——暗背景下暗色视觉后退，最严重那档反而最不显眼。现在是蓝→黄→
  橙→红的色相阶梯，两两拉开。
- **critical 不是"更严重的 error"**，是保护动作已经发生、设备已进入安全态（现表里
  只有 1001 急停和 1003 打印中开门两条，都是激光立即断电）。排序上
  `ui_alarm_state::rebuild_order()` 也按 severity 降序，critical 永远排在当前报警
  最上面。若日后觉得颜色仍不够，加文字胶囊比再调色有效，但标签要写结果
  （"STOPPED"）而不是协议术语。（旧文档把 3/4 叫 fault/trip，只是改名，语义同此。）

生命周期胶囊（Active / Recovered - awaiting reset / Event / Resolved）是**另一套
语义**，配色在 `page_user_message.cpp` 的 `kPill*`，不要和等级配色混用。

### MCU 报警（`ui_alarm_state` + `ui_alarm_catalog`）

MCU 经串口上报的报警（`GLAVO_Alarm_Report_Ntf`，0xA3/0x20 原样透传）在 UI 侧有唯一
数据源 `core/ui_alarm_state`，按 `state_mask` 推导生命周期：

- **EVENT**（一次性）：记历史 + 走 `ui_message_emit_ex()` 弹 toast。**每一帧都是一次独立
  发生**，哪怕同一个码上次被关掉或收起过也重新弹；停留多久由 store 按等级计时，这里不管
  （早先本模块自带一支 5s 的 `event_dismiss_cb`，已撤，理由见「自动收起」）；
- **CONDITION**（状态型）：ACTIVE / RTN_LATCHED 期间挂「当前报警」表并同步到
  `ui_message_store`；回 NORMAL 撤卡、转历史。写入按 stage 分流：**首帧**（本地还没有这条）
  和 **RTN_LATCHED → ACTIVE 上升沿**（门关了又开、传感器反复越限）走 `emit_ex` 重弹一次，
  其余状态迁移走 `sync_ex` 静默更新。记录粒度仍是「一轮一条」——上升沿不新建条目，只是让
  toast 再提醒一次。

报警快照（30s 兜底轮询那条）一律走 `sync_ex`：它是对齐、不是事件。只有「本地压根没有这条」
时才会弹一次——开机或串口重连后补齐 MCU 那边一直亮着的报警，正该主动告知；本地已有的条目
走不到那个分支，所以轮询不会刷屏。

**文档权威顺序**（几份文件描述同一套协议，冲突时按此取舍）：`doc/主控板串口通信协议
- *.csv` 四份 → [`快照开发指导`](../../doc/SOC_TLV_ALARM_EVT_SNAPSHOT报警快照开发指导.md)
→ [`单条上报开发指导`](../../doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md)。
最后那份**没随 CSV 更新**，它 §7 还把 severity 3/4 叫 fault/trip（现为 error/critical，
只是改名，语义没变——CRITICAL 仍是「保护已动作、已进入安全态」，现表里只有 1001 急停和
1003 打印中开门两条），§5 那条 `show_reset = latched && clear_mode==RESET` 伪代码也已
作废（见下「复位」）。别照那份改代码。

文案由 `ui_alarm_catalog`（`tools/gen_alarm_catalog.py` 从 doc/ 下三份 CSV 生成
`ui_alarm_catalog_gen.cpp`，当前 105 条报警 = 95 CONDITION + 10 EVENT、78 条 detail；
英文翻译表 `doc/alarm_catalog_en.csv` 可覆写，未翻译的用 macro 名转写兜底——目前还有
41 条报警 + 48 条 detail 走兜底，标题能看但 `desc`（处理建议）是空的）+ 报文
detail 组合；查不到的 code/detail 降级显示原始编号
**绝不丢弃**（固件可能比字典新）。历史落盘 `/userdata/messages/alarm_history.jsonl`
（上限 200 条，只存原始字段，文案加载时按当前字典重新生成）。**目录由
`ui_alarm_state_init()` 自己 `create_directories`**——UI 可能比 Media 先起，不能
指望别的进程建；不建的话 `fopen` 失败会把历史整段静默丢掉（现已补 `UPC_LOG_ALARM_STATE`
错误日志）。这里刻意**不放在 `/userdata/media` 下**：报警历史不是媒体，早先挂在那儿只是
为了借 `UPC_MEDIA_ROOT` 那个宏，现已解除对 `media_paths.hpp` 的依赖。

复位：`ui_alarm_request_reset(code)` → `UI_Alarm_Reset_Ntf` → Glavo 组 0x82 帧下发。
受理后置 `reset_pending`，解除它有三条路径：该 code 的新上报、`GLAVO_Alarm_Reset_Result_Ntf`
失败回执（0xA3/0x82 TLV 0x04，MCU 只在失败时发）、10s 超时兜底。

**「此刻能不能点 Reset」只有一个判据 `ui_alarm_reset_allowed()`**，三个复位入口
（消息页详情抽屉 / 首页终止结果条 / 打印入口拦截弹窗）共用，别各写各的——早先抽屉自己
加了 `stage == RTN_LATCHED` 而另两处没加，同一条报警在两个界面表现相反。规则：

| 状态 | `reset_policy` | 能否点 |
|---|---|---|
| RTN_LATCHED | 任意 | 能 |
| ACTIVE | `OWNER_TASK` | **能**——MCU 转给业务任务做硬件恢复 |
| ACTIVE | `LOCAL` | 不能，条件仍在时 MCU 直接拒绝 |
| 任意 | 字典查不到 | **能**——查不到是字典没跟上固件，不是「已知不可复位」 |

ACTIVE + OWNER_TASK 这一档最容易漏：激光器 1301、各轴 1203~1230 都是这类，**不点复位
它们永远好不了**（激光器要等 MCU 下发清除命令才恢复），锁住等于死锁，只能断电。
现表里 30 条 `clear_mode=RESET` 中 28 条是 OWNER_TASK，只有 1124/1125 是 LOCAL。
最后那行「字典查不到」以前按 LOCAL 拦着，是**反了**：字典外的码恰恰最可能是新固件
刚加的（2026-08-30 那批 18 条新增里，1004/1126~1129 五条全是 RESET + OWNER_TASK），
拦下来它们永远复不了位、只能断电；而误发一帧的代价只是 MCU 不认就不回、10s 超时
自解锁。`reset_policy` 是「运行时一律以报文为准」的**唯一例外**——报文里没这个字段，
只有 `报警map.csv` 有，所以只能查字典。

**快照对齐（`UI_Alarm_Snapshot_Ntf` / `GLAVO_Alarm_Snapshot_Ntf`，0xA3/0x81）**：
单条上报只在状态变化时发，所以 UI 重启 / 串口重连 / SOC 比 MCU 后启动这几种情况下，
之前就亮着的报警**再也收不到**，本地表会和 MCU 真实状态脱节。快照是补这个洞的：

- **请求-响应**：MCU **不会主动推**，不发 `UI_Alarm_Snapshot_Ntf`（TLV 0x05 len=0）
  就一帧都收不到。触发点五处：`ui_alarm_state_init()`、串口打开成功（Glavo 侧
  `McuLink::request_alarm_snapshot()` 自己发）、进消息中心页、30s 兜底轮询、开始打印前。
  `ui_alarm_request_snapshot()` 内部带节流（上一轮在途或距上次不足 2s 就跳过），
  调用方不必自己防抖；
- **稀疏攒帧**：只回 `(active|latched)!=0` 的 slot，**没回的一律按全零算**。收齐
  `nonzero_slot_count` 个不同 slot 才原子提交——半套位图会把没收到那些 slot 的报警误判成
  已解除。`nonzero_slot_count=0` 是合法的「当前无报警」响应，收到即全清 CONDITION；
- **轮次边界用「发出请求的时刻」界定，不做 Seq 匹配**：串口组帧把 Seq 写死成 0x01
  （`serial_frame.cpp`），收发两侧都没有 Seq 通路。而快照本来就是我们主动请求、同时只维护
  一个，请求时刻比 Seq 更可靠且零协议成本。作废整轮的条件：攒超 2s、同轮 `count` 或
  `highest_severity` 不一致、`slot_id` 重复或越界、`count>0` 却送来空 mask；
- **槽位→报警号是固定公式** `1000 + group*100 + word*32 + bit`（`slot_id = group*3+word`，
  见 `middleware::alarm_snapshot_code_of()`），**不查表**。`报警map.csv` 已降级为显示
  元数据，生成脚本只拿它校验公式一致性和读 `reset_policy`——快照指导 §6.5 明令禁止改用
  内部 index 推算。好处是字典比固件新旧都能算出号来；
- **对齐**：快照亮着而本地没有的补进来，本地有而快照说不亮的转历史。补进来的那条只有
  code 可用，`severity` / `clear_mode` 走字典兜底（`severity_fallback` /
  `clear_mode_fallback`），`detail` 记 0、时刻记补齐时刻——这几样快照里根本没有。
  字典连这个 code 都查不到时 severity 取 warning、**`clear_mode` 取 RESET**：快照报文
  里压根没有 clear_mode 字段，这里填的是个凭空造的值，填 NONE 会被
  `ui_alarm_reset_allowed()` 第一道判断直接否掉、抽屉连 Reset 钮都不建。兜底只活到
  该 code 的第一帧单条上报，之后一律以报文为准；
- 反向清理**只认受快照管辖的 code**（`alarm_snapshot_covers()`：1000..1395 且组内偏移
  < 96）。EVENT 不占 bit、每组末尾 `xx96..xx99` 是保留号，这些"快照里没提"是信息缺失
  不是状态变化，照清会把真报警抹掉；
- 已在表里的条目只校正生命周期（ACTIVE ↔ RTN_LATCHED），
  `severity` / `clear_mode` / `detail` 保持单条上报给的值——那才是权威数据；
- **快照在途时到达的 CONDITION 单报先攒着**（`g_pending`，上限 16 条），等快照原子提交后
  按序重放；**同时立刻 `on_alarm_condition` 上屏**（灯效板已按单报变色，浮层不应等 MCU
  回齐）。提交时反向清理会跳过仍在 `g_pending` 里的 code，避免「先撤卡再重放」闪一下。
  快照是「请求那一刻」的状态，暂存的单报都比它新，不压在快照之上就会被盖回去
  （快照指导 §11）。EVENT 不进这个队列，它不改 CONDITION 状态。
- **攒帧 2s 超时由 oneshot 定时器主动触发**（`g_snap_assemble_timer`），不再只靠「下一帧
  快照 / 下一轮请求」被动检查；MCU 忙着起印握手时迟迟不回，也不会把 pending 单报卡到
  30s 轮询。

打印融合（停机类报警未复位时 MCU 拒绝新打印，只回一条给不出原因的 EVENT 2003
`PRINT_START_DENIED`）：

- **阻断名单**：`ui_alarm_print_blocker()` 里硬编码七条——1004 内门运动故障、
  1124 打印步骤超时、1125 运动失败打印中止、1126/1127 筛粉电机过流与电流检测故障、
  1128/1129 PTC 加热过流与电流检测故障，都是 `clear_mode=RESET` 的停机等待复位类。
  **报文和字典里都没有「是否阻断打印」这个字段**，只能按报警语义维护，MCU 新增停机类
  报警要跟着补；
- **入口拦截**：`control.cpp` 的 `on_detail_start_print()`（文件页详情面板底部那枚
  「Confirm Print」）先查 `ui_alarm_print_blocker()`，有阻断报警就在 `lv_layer_top()`
  弹「Unable to start print」，可复位的给 Reset 直达键。同时亮多条时一次只提示 / 只
  复位排在最前的那条，用户得点几轮——七条里 1126/1127 与 1128/1129 各是一对同源故障，
  真撞上再考虑批量复位；
- **通知卡 + 详情抽屉**：阻断报警与其它报警一样上浮层卡，复位走抽屉里的 Reset 钮
  （三态判据 `ui_alarm_reset_allowed()`，与入口拦截弹窗同一份）。1124/1125 是 LOCAL，
  ACTIVE 时置灰；1004/1126~1129 是 OWNER_TASK，ACTIVE 时就能点。

> **曾经有过、已删除的两件东西**：终止态浮层顶部那条橙色「失败原因条」
> （`build_result_alarm_bar()` / `sync_result_alarm_bar()`，V3.9.7 引入）不是设计稿元素，
> 已按产品要求整块删掉；随它一起去掉的是 `ui_alarm_set_print_session()` 那套**会话期抑制**
> ——那套逻辑本来是「打印会话内 1124/1125 不上浮层卡，因为结果页已给原因 + Reset」，条没了
> 抑制就必须一起去，否则打印中断时停机原因哪儿都看不到。现在 1124/1125 全程走正常通知卡。

消息页视觉基线是**浅色新稿** 6230:89616（列表）/ 6091:13405（左滑删除）/
6091:13612（详情抽屉）/ 6091:13847（滚动态与 Load More）/ 6101:14155（加载态）。
几处稿→工程的换算与取舍记在这里，改版前先看：

- **整页是浅色档**：`page_manager` 的 `set_shell_light()` 名单里加了本页，底色走
  `user_light_bg`。同时**本页不显示常驻设备条**（同历史页）：稿上顶部从屏顶起就是
  Tab 行，那条设备条在图层树上还躺着（6101:14137）但被不透明顶栏整条盖住，等价于
  不画。代价是这一页看不到设备名与联机状态，照稿的取舍；
- **顶栏是浮层、列表是满高的**：稿 6091:13847 顶部那张卡被顶栏切了一半，说明列表
  从顶栏底下滚过去。所以列表铺满整页、用 `pad_top = chrome_h()` 把首屏顶到顶栏之下
  （padding 会跟着一起滚），顶栏另铺一层 `user_light_bg` 不透明底 + 一道
  `0 2 6 rgba(0,0,0,.05)` 投影盖住。旧版那条通栏分隔线随之去掉；
- **顶栏与列表横向同缘**，都是 48/48（`content_pad_l/r()`，落在 `USER_HIST_PAD_L`）。
  旧稿里 Tab 行 48/112、列表 64/160 那套错位是给悬浮翻页钮让位的，钮按新稿去掉后
  四个量塌缩成一个，专用的 `user_hist_list_pad_l` 字段也随之删掉；
- 卡片是**白底实色**（`user_light_card`）+ 1px 白 10% 描边 + `0 2 6 rgba(0,0,0,.04)`
  投影，圆角 16（`USER_HIST_CARD_RADIUS`）、内距与列间距 24。白底压白描边等于看不见，
  稿上就这么画的，照留；真正撑出边界的是那道投影。底**必须不透明**：垫在卡片下层的
  删除钮靠卡片挡着，半透明会直接透出来；按下态同理只改底色不调 `bg_opa`；
- 左侧徽标是**实色圆角方块 + 白图标**（稿 48 见方 / 圆角 8 / 内含 24 的图标），
  与左下通知卡 `ui_notice_stack::build_card()` 同款，两处观感必须一致。四档配色
  `#485DFF / #FFC942 / #FF7843 / #FF4242` 与 `ui_message_severity_color()` 现有值一字不差；
- **卡内字号是 20，不是历史页那套 16**——稿 6230:89630/89631 里标题 / 正文 / 时间三处
  都用命名样式「一级标题」「一级正文」（20），与打印历史卡的 16 是两个尺度，别互相
  看齐。标题与正文同色 `#111111`，只靠字重（SemiBold / Regular）+ 50% 透明度分层；
  Montserrat 单字重，字重那半做不出来，只落地「同号 + 透明度」；
- **新稿去掉了卡片右端那枚 ›**。「点了有没有抽屉」的判据一直是 `card_click_cb` 里的
  `ItemSrc::StoreMsg`，那枚 › 只喂过视觉，所以 `MsgItem::has_detail` 一并删掉。
  代价是卡片可点这件事不再有视觉提示，稿上四张列表帧都没画它；
- **多选批量删除整体去掉**（稿四张列表帧都没画多选态）：随之删掉的有管理模式开关、
  卡片复选框与勾选态、底部多选浮条、批量删除确认弹窗，以及只服务它们的
  `user_hist_btn_w` 字段。删除只剩左滑单条，仍只对历史条目开放；
- **右侧两枚悬浮翻页钮按稿去掉**，翻页靠底部 Load More。`swipe_reveal_px()` 早就
  不依赖 `scroll_btn_size()` 了（那段注释专门说过这件事），所以删钮不会连带改手感；
- 左滑删除：卡片左移 104 = 删除钮 72（`USER_LIGHT_BTN_SZ`）+ 净距 32（`USER_PAD_LG`）。
  稿 6091:13405 量到卡片左缘 176→72、右缘 1232→1128，而钮固定在 1160..1232（距屏右 48，
  与内容区右缘同线）。钮是 **72 见方、圆角 24 的方钮，不是圆钮**，红色渐变
  `#FC3D3E→#FF6667` + 一圈浅白描边，**挂在卡片之外**（`LV_ALIGN_OUT_RIGHT_MID` 再偏
  `USER_PAD_LG`），不像旧稿那样贴在卡片内的右缘；
- 左滑机制见下方「左滑删除：跟手拖动」——消息页与历史页共用同一套写法；
- **删除后不跳回顶部**：`rebuild_list()` 重建前记 `lv_obj_get_scroll_y()`、重建后
  `lv_obj_update_layout()` 再按 `lv_obj_get_scroll_bottom()` 夹着滚回去。
  `lv_obj_clean` 会把滚动位置清零，不接这一手删一条就弹回顶部；夹取也不能省——
  删掉末尾几条后列表变矮，旧滚动量越界会停在空白上。两页都是这么做的；
- **三 Tab + All read 行**（稿 6230:89689）：`All` / `Device` / `System`，字号 24，
  **没有下划线**——选中与否只差不透明度（选中 `user_light_text` 实色、未选中同色 50%）。
  共享构造 `ui_user_make_tab_item` 仍会建出那条下划线，建完永久藏起来，与历史页同一
  处理。分档判据是**码段**：`DeviceEventCode::UiWearMask`(512) 以上是「UI 本地事件，
  后端不应发送」，正好是天然分界——**Device** = MCU 报警（码 ≥1000）+ 后端设备事件
  （码 <512），**System** = 前端本地提示（512..999）。稿上 System 那栏画的是 OTA 升级、
  账号共享，**本工程还没有这类数据源**，所以该 Tab 目前只有打印结束那两条本地提示，
  多数时候是空的。切 Tab 回列表顶部；
- 右上那枚 `All read`（稿 6255:95773）是白底卡片钮（圆角 16 / 内距 16-24 / 字号 22），
  **目前只画不接逻辑**：「进过一次消息页就全部标已读」的现有语义还在
  `page_user_message_refresh()` 里，这枚钮的点击语义待产品定，定了再挂回调；
- **字体只对字号，不追字族字重**：这一页稿上用了五个族（Poppins 管 Tab / 卡片 / 按钮、
  Futura 只管卡片右侧那行时间、Inter 管 All read 与 Load More、Noto Sans SC 管抽屉的
  中文、Google Sans Flex 管设备条），工程只编进了 Montserrat 一族一个字重。好在稿上
  用到的字号 16/18/20/22/24/28 全有现成档位，一个新的都不用加。
  **行距是例外，追得了也追了**：稿把正文行距写死成 28（卡片 Poppins 20/28）与 1.5em
  （抽屉 Noto 18），而 Montserrat 的 `line_height` 只有字号的 1.1 倍左右，不补的话多行
  正文会挤成一坨。差额由 `extra_line_space()` 按 28/22 的比值算出来喂 `text_line_space`。
  行距是稿上独立标注的排版属性，与字族无关，所以补它不算违背上面那条；
  **单行文本的行盒高仍追不平**（稿 28 vs Montserrat 22），卡片因此比稿矮 10，列表往下
  会累积漂移——这是字族差异的直接后果，headless 量过，接受；
- **分页：初始只建最近 10 条 + 底部「Load More」**（稿 6091:13921 的 `查看更多`）。
  `collect_items()` 已按 `sort_ts` 降序，所以取前 `g_visible_limit` 条就是
  最近的那些；每点一次 Load More 加 `kRowsPerPage`(10) 后原地重建，放完了就不建这一行、
  等于自动隐藏。截断可能落在某个日期组中间，但分组标题是"组名变了才插"，最后一组不完整
  也不会漏标题；重建末尾那段 `keep_scroll` 恢复让展开时视口不跳、新内容出现在下方。

  **`g_visible_limit` 的重置只放在切 Tab 与进页，绝不能放进 `rebuild_list()`**：
  observer 的 `rebuild_async()` 也走 `rebuild_list()`，放进去的话用户展开到第 50 条时
  来一条新消息，列表会当场折回 10 条。

  Load More 那行的文字样式挂在**按钮**上而不是 label 上：LVGL 的状态不向子节点传播，
  写在 label 上的 `LV_STATE_PRESSED` 永远命中不了；`text_*` 可继承，挂在按钮上 label
  就会沿父链跟着按压态变。稿上它是 64×24 的纯文字，触摸屏上太小，所以外面包了一层整行宽、
  `USER_BTN_PRIMARY_H` 高的透明热区。

  稿 6101:14155 那个**加载圈有意未实现**：消息数据全在内存（三路 store 合并 + 排序，
  无 IO、无异步），点开就是同步多建几张卡；而 LVGL 单线程，同步重建期间画面根本不刷新，
  转圈一帧都不会转。即便照历史页 `append_list_build_chunk()` 改成异步分块，每块 8 张、
  追 10 张也只有两块约 24ms，仍是 1~2 帧，看不见。将来消息真接了云端、展开变成回调式
  再启用它。

  分页之前进页有肉眼可见的卡顿：`switch_to()` → `refresh()` → `rebuild_list()` 全程同步，
  页面要等整张列表建完 + 首帧渲染才显示。列表长度当时没有上限，三路上限相加约 300 条
  （当前报警 `kCurrentCap` 128 + store `kCapacity` 24 + 历史 `UI_ALARM_HISTORY_CAP` 200，
  历史还落盘跨重启累积），而一条可删的历史卡约 19 个对象 + 9 个事件回调——200 条就是
  近 4000 个 LVGL 对象，加上 `flex_grow` 与 `LONG_WRAP` 互相依赖导致的多趟文本测量。
- **进页只重建一次**：`refresh()` 里的 `ui_message_mark_all_read()` 在有未读时会
  `notify_observers()`（侧栏红点靠这个通知才灭，不能省），一路转到 `on_store_change()`
  → `queue_rebuild()`，于是排下一次异步重建。而 `refresh()` 紧接着又同步 `rebuild_list()`
  一次，等于**每次进页建两遍**——偏偏"有未读"正是点进这个页面的常见前提。
  第二遍还是纯浪费：本页从不读 `unread`（未读只驱动侧栏红点，见 `ui_app.cpp` 的
  `sync_message_badge()`），两遍产出逐像素相同。
  处理办法是同步建完后 `lv_async_call_cancel(rebuild_async, nullptr)` 把那次排队撤掉
  并清 `g_rebuild_queued`。**不要靠调换 `mark_all_read()` 与 `rebuild_list()` 的顺序**：
  `queue_rebuild()` 只看 `g_active`、不看列表刚建过没有，换顺序照样排得上。也别改成用
  「条数 + 最新 seq」当指纹去跳过重建——`title/body` 指向调用方缓冲、内容会原位变化
  （这正是 `ui_message_update()` 非 renew 时也要通知的原因），那样会漏刷成旧文案。
  切 Tab 那条路（`on_tab_click` 同样是同步 `rebuild_list()`）理论上也可能撞上一次待跑的
  异步重建，但要先有消息在你停留期间到达，概率低且结果一致，暂未处理；
- 详情抽屉建在 **`lv_layer_top()`** 上、宽 632（稿 6091:13766，贴右满高），遮罩
  `rgba(0,0,0,.6)` 盖满整屏。**不能挂在页面根节点上**——那样上边会被 `content_panel_`
  的 `pad_top` 顶下来。代价是遮罩连侧栏一起盖住，开着抽屉点侧栏是先关抽屉（标准模态，
  已确认）。layer_top 不随页面隐藏销毁，靠 `page_user_message_exit()` 里的
  `drawer_close()` 收尾；
- **抽屉内部用 `drawer_px()` 折算**：稿上那几个量（92 内距、21 列距、448 内容宽、
  280 图位高、52 顶距、72 按钮高）没一个落得回现有 token 梯度，逐个开主题字段不值当，
  统一按抽屉宽比例折，同 `page_user_home` 的 `card_px()`；
- 抽屉外壳按新稿换过：返回行是 **chevron + 标题**（旧稿那枚圆形返回钮没了），
  整行当返回热区——稿上那枚 8×16 的 chevron 单独当按钮太小；chevron 本身是条 2px
  描边的折线，工程没有这个矢量资源，取 Montserrat 自带的 `LV_SYMBOL_LEFT` 顶上；
- 抽屉**内容项按原稿一个不少地保留**，只换样式：稿上只画到「标题 → 时间 → 描述 →
  图位 → 提示语 → 按钮」，而状态胶囊（Active / Awaiting reset / Resolved / Event）
  与底部的 code/detail 小字是稿外保留的——报警必须一眼看出解没解除，报修要报号；
- 稿上抽屉里 448×280 那块是处理步骤图，**真图还没出，照稿画成占位灰块**
  （`rgba(0,0,0,.1)` / 圆角 16）。不画的话抽屉会比稿矮 300 左右，下面主按钮的位置
  跟着不对。补回胶囊与小字后内容会超过 800，`body_col` 可竖向滚；
- 稿是中文的，工程 UI 统一英文（Montserrat 字库），文案不照抄（"通知详情" →
  "Notification Details"，"确认解障" → Reset 三态，"查看更多" → "Load More"）；
- **抽屉字号**（稿 6091:13612）：返回行标题 20（`user_font_title`）、详情标题 28
  （`user_font_dlg_title`，稿是 Noto Sans SC Bold 28）、时间 18 @30%、正文 18 @50%
  （都用 `user_font_text`）；主按钮 448×72、`user_light_text` 底、文字 20。

**模拟数据一律带 `[SIM] ` 前缀**：调试页两个注入开关造出来的 10 条假消息，标题前面
都加 `[SIM] `，真消息一个字不改，联调时一眼分得清。报警侧在 `compose_text()` 里加
（浮层卡 / 消息页列表 / 详情抽屉 / 历史读的都是这一份文案，一处加完四处都有）；
设备事件侧走 `ui_message_debug_push()`（文案仍取自码表，只是套上前缀）。
**假报警还不落盘**：`history_save()` 跳过 `simulated` 条目，重启即净，不用再手删
`/userdata/messages/alarm_history.jsonl`。同一个码后来收到真报文会自动摘掉 `[SIM]`。

联调：MCU 不发真报警时用调试页 Other → Inject → `Alarm Inject`
（`ui_alarm_debug_inject()` 绕过总线直走上报路径，造 6 条假报警：1124 阻断 /
1104 RTN_LATCHED / 1101 自动清除 / 2000 EVENT 四条显示路径，外加 1003 一条
critical 把 information/warning/error/critical 四档等级配色凑齐——等级不进字典、
以报文为准，不造就看不到；关掉开关注入 NORMAL 帧模拟解除）。
1003 同时是 **ACTIVE + `reset_policy=OWNER_TASK`** 的样本，用来验那档在抽屉里
可点 Reset；1124 是 LOCAL，ACTIVE 时该置灰——两条对照着看。
第 6 条 **1999 是字典外的未知码**，验固件比字典新时的降级路径：标题降级成
"Unknown alarm #1999" 而 Reset 仍可点。选 1999 是因为它在快照管辖范围
（1000..1395）之外，30s 兜底轮询的反向清理不会把这条假报警抹掉。
注意注入条目按 Reset 会真发 `UI_Alarm_Reset_Ntf` 到串口，MCU 不回帧时 10s 后
pending 自动解除——正好顺带验超时路径。

### 左滑删除：跟手拖动（现仅消息页）

实现在 `page_user_message.cpp` 的「左滑删除」一节。**历史页曾有一份字面相同的副本，
随浅色稿 6122 一族整套删掉了**（新稿不画左滑，删东西只剩长按多选一条路），所以这一节
现在只服务消息页。消息页自己也改过新稿（6230 一族），但左滑**保留且成了唯一的删除入口**
——多选批量删除按稿去掉了，所以这一节短期内清不掉。

**不要改回 `LV_EVENT_GESTURE`**，那条路踩过两个坑：

1. **门槛太高，触屏基本划不出来**。`indev_gesture()` 要求「每帧位移 ≥
   `gesture_min_velocity`(3px) 且累计 > `gesture_min_distance`(50px)」，慢一帧累加器
   就被清零，手指慢慢拖永远触发不了，只有快速甩才算数；
2. **手势不吞掉释放时的 `LV_EVENT_CLICKED`**（`lv_indev.c` 释放分支只看有没有发生
   滚动），卡片上再挂点击就会一松手把刚划开的卡片当点击关回去。

现在的写法：

- `PRESSED` 记起点与位移基准并清状态，`PRESSING` 算 dx/dy，横向超过 8px 才起拖，
  起拖瞬间做方向锁定（`|dy| > |dx|` 判给列表滚动，本次按压不再拖）；
- 拖动中 `translate_x` 夹在 `[-swipe_reveal_px(), 0]`，`RELEASED` / `PRESS_LOST`
  过半吸附、否则弹回；
- 拖完置 `just_dragged` 挡掉紧随其后的那次 `CLICKED`，**在下次 `PRESSED` 清掉**而不是
  在 `CLICKED` 里清——`PRESS_LOST` 收场那次没有 CLICKED 来消费，不然会吃掉下一次点击；
- **轴向排他**：起拖后清掉卡片的 `LV_OBJ_FLAG_SCROLL_CHAIN_VER`，松手（含 `PRESS_LOST`）
  无条件加回来。不这么做的话，`find_scroll_obj()` 每帧会拿**整次按压的累计位移**重新判
  方向，斜着拉一下（竖向累计超过横向且 >`scroll_limit`）列表就开始滚、卡片被弹回原位。
  卡片自身不可滚，清掉这个标志后滚动搜索会断在卡片这层（`lv_indev_scroll.c:330`），
  够不到列表。代价：划开一半不能直接改滚列表，得先抬手（同 iOS 列表）；
- 起拖之前仍每帧查 `lv_indev_get_scroll_obj()`，列表真滚起来就取消拖动并弹回；
- 靠卡片自带的 `LV_OBJ_FLAG_PRESS_LOCK`（`lv_obj_create` 默认带）保证卡片滑出手指
  下方也不丢按压——**别在卡片上清这个标志**；
- 命中目标要唯一：消息页把卡内子容器的 `CLICKABLE` 全清掉。

> 历史页现在用的是**长按进多选**（`LV_EVENT_LONG_PRESSED`），与这套跟手拖动无关，
> 但「LVGL 会在长按/拖动之后照发一次 `CLICKED`、得自己吃掉」这个坑是同一个，
> 历史页那边的挡法见「打印历史页布局」一节的 `just_long_pressed`。

### 摄像头整屏预览

`g_print_area`（打印直播）与 `g_monitor_area`（查看监控）都是 `lv_obj_set_pos(area,
-USER_SIDEBAR_W, 0)` + 宽度 `LV_HOR_RES`，画面左边缘落在屏幕左边缘、侧栏浮于其上。
首页根节点同样负偏移铺满整屏、再用自己的 `pad_left` 还原内容区，所以按钮行/仪表盘/
开门滑块的几何不变（纵向的负偏移见「常驻设备信息条」，那一格不还原）。

摄像头层必须留在首页根节点**内部**：切页是靠 `SimplePage::on_exit()` 隐藏根节点实现的，
挂到 `content_panel_` 上会导致打印中切去设置页时画面仍留在底下（`switch_to` 允许
打印中导航）。这也是不能靠 `LV_OBJ_FLAG_OVERFLOW_VISIBLE` 的原因——它只放开到
`ext_draw_size`，给不到一整个侧栏宽度。

侧栏可读性由两层叠出来：`menu_panel_` 自己的 `LV_OPA_90`，加上 `build_camera_gradient_masks()`
里那条覆盖 `USER_SIDEBAR_W` 的实心底衬。底衬取 `LV_OPA_70`，与左渐变遮罩的起始值
相同，两者在侧栏右边缘无缝衔接成「恒定段 + 淡出段」，不会出现硬边；左渐变同时补了
`USER_SIDEBAR_W` 的 x 偏移，在画面里的位置与改造前一致。

副作用：`cam_apply_layout()` 是 cover 填充（`zoom = max(zoom_w, zoom_h)`），viewport
变宽后纵向裁得更多。1024×600 屏上纵向可见从 50.9% 降到 46.9%，但缩放系数正好变成
1.000（屏宽 = 流宽 1024，见 `CAMERA_WIDTH_DEFAULT`），走 LVGL 无缩放快路径，省掉逐帧
重采样。

**`zoom` 必须是 1.000，这不是"顺便"而是硬要求。** 1280 档把上面那个前提破掉过一次：
屏宽 1280、流宽 1080，`zoom` 变成 303，于是 `lv_image` 落进 `lv_draw_sw_transform`
逐像素双线性重采样，整屏一次约 98ms（`refr 5s:` 里 `draw_avg_us≈98000`、
`loops=41/5s`），LVGL 主循环被压到约 8fps，触摸跟着发滞——解码侧明明还在
30fps（`ok≈150` 而 `show_n≈41`）。修法不是回去调 LVGL，而是把"宽度适配 + 纵向居中
裁剪"整个交给 RGA 在色彩转换那一趟里做掉，见「预览输出几何」。

## UiMsgProxy（Glavo）

| 方法 | 方向 |
|------|------|
| `send_*()` | UI → `APP_GLAVO` |
| `register_*()` | Glavo → UI 回调 |

`Subscription` 必须保存在 `subs_` 等成员中。其它后端用独立 `XxxMsgProxy`，不要堆进 UiMsgProxy。

`register_*` 回调在总线线程上跑，**要建/毁 LVGL 对象就得先 `lv_async_call` 回主线程**
（例：`control.cpp` 的 `on_powder_low_async`、`page_user_home.cpp` 的 `on_clean_done_async`）。
只改现成 label / 显隐的回调沿用直调。

### 选文件页目录数据

| Tab | 数据源 |
|-----|--------|
| **Local** | UI 直接读 `/userdata/Gcode/config.json`（`common/fs_manifest/gcode_manifest.*`），**不发** `Fs_DirOpen_Req` |
| **USB** | `send_fs_dir_open(Usb)` → Glavo 扫盘 → `Fs_DirOpen_Rsp` |

删除 Local / Prepare / Start 仍走 IPC。

### 选文件页详情面板（稿 4014:16631）

右侧详情列自上而下：缩略图 → 文件名 → meta 行 → 撑开 → `Confirm Print` → `Delete File`。

- **信息元 `make_detail_inline_kv()`**：「[图标] caption value」单行小项，**详情面板与
  文件列表行共用同一个**。三个可选项：`icon` 传 nullptr 不占图标位（USB 的文件大小行）；
  `caption` 传 nullptr 不建标签，也就不会多吃一份 `pad_column`（列表行只要图标+数值）；
  `value_font` 传 nullptr 用 `user_font_text()`。
- **图标一律 24px 原尺寸 1:1，别加缩放参数**。`lv_draw_sw_transform` 是 2 抽头点采样、
  不做面积平均，缩小时整行整列跳过源像素，而 `ic_clock` / `ic_cube` 的描边只有 1.33px，
  跳掉一行就断一截，实机上是肉眼可见的锯齿（曾经缩到 18px，已回退）。真要小一号就照
  `METRIC_ICONS` 的做法让 `tools/gen_home_metric_icons.py` 另出一档原生尺寸
  （同 `ic_home_26/32/48`），运行时始终 1:1——这条规矩在那个脚本的文件头有实测数据。
- **文件列表行的 meta** 也走这个 helper，两个元「[clock] 耗时」「[cube] 克数」，字号压到
  `user_font_caption()` 以保持行高（图标与详情面板同为 24px），两元同用 `text_dim`
  一个色阶（accent 蓝留给详情页强调）。原先那行拼好的纯文本 `"Xh Ymin · Zg"` 与配套的
  `ui_user_format_file_meta()` 已删。
- **占位符一律 ASCII `"--"`**，别用破折号 `—`(U+2014)：内置 Montserrat 只收
  0x20–0x7F、0xB0、0x2022，U+2014 会渲染成豆腐块。
- **1024 档的高度预算**：详情列 `pad_row` 用 `USER_PAD_SM`（不是 `USER_PAD`），且耗时/耗材
  由两行压成「图标 + caption + value」一行。再往这一列加东西前先算 600px 屏。
- **按钮圆角**：稿里这套按钮组件是 8px@1280 的圆角矩形，不是全圆胶囊，用
  `USER_BTN_RADIUS`（1024=6 / 1280=8 / 1920=12）。`Confirm Print` 借 `user_style_btn_primary()`
  取主色与外发光，但要就地覆盖它的 `LV_RADIUS_CIRCLE`。

> **已删除：打印板厚度选择**（`Plate Thickness` 标题 + `5mm/10mm` 分段 + 未选提示 +
> `Confirm Print` 置灰联动，稿 4014:16631）。按产品要求整块去掉，`backend_start_job()`
> 直接调用 `send_print_start(file_index, source)` 下发起印请求，不再携带厚度字段。
> 随它一起删掉的还有 `g_plate_thickness` /
> `apply_plate_thickness_ui()` / `reset_plate_thickness()` / `make_detail_warn_row()`，
> 以及 `Confirm Print` 的 `LV_STATE_DISABLED` 样式——选中文件后该按钮现在恒可点。

`Confirm Print` 之后的「开始打印」二次确认弹窗保留。删除确认与终止打印确认共用
`open_user_confirm_dialog()`：标题问句（`user_font_value`）+ 一句说明（`user_font_text`）+
取消(4% 白底) / 危险(实心红)，两枚等分整行、`USER_BTN_RADIUS` 圆角。页脚这两枚按钮的样式
已提到 `ui_user_make_confirm_footer_btn()`，打印历史页的批量删除确认用的是同一枚。
用户层弹窗标题下的主色装饰线已全局去掉
（`ui_modal_dialog_cfg_user()` 里 `accent_underline = false`）。

### 按钮样式基线

选文件页详情列那两枚按钮是全工程的样式基准，其余弹窗按钮都按它对齐过一轮：

- **几何**：高 `USER_DLG_FOOTER_BTN_H`（1024=51 / 1280=64 / 1920=96），圆角
  `USER_BTN_RADIUS`（6 / 8 / 12）。**弹窗里的按钮一律不用 `USER_DLG_RADIUS`**——那是卡片
  圆角（1024 档 16），压在 51px 高的按钮上会明显比选文件页更圆。
- **主按钮**：`user_color_accent` 实心 + 24px accent 阴影 `LV_OPA_30`。满宽的主按钮
  （门弹窗、放版弹窗、缺粉弹窗）左右两侧的光会被弹窗内容区裁平，只剩上下能看见，这是
  接受的取舍；置灰态（`enabled=false`）连阴影一起去掉。
- **副按钮**：`user_color_text` 压到 opa 10（稿 `rgba(255,255,255,0.04)`）、无描边，按下
  态 opa 30。四处副按钮工厂（`make_dlg_wide_ghost_button`、`ui_user_make_confirm_footer_btn`
  的 `filled=false`、`ui_user_make_footer_ghost_btn`、`ui_xfr_dialog` 的 `make_secondary_btn`）
  现在是同一套色值，区别只在满宽 / 等分 / 定宽。
- **`user_style_btn_primary()` 是胶囊，不要改它**：调试页那几颗按钮还在原样用它的全圆
  形状。用户层三处（选文件页 `Confirm Print`、设置页两个密码弹窗的 `Enter`/`Connect`）
  只是借它的主色和外发光，都在调用点就地覆盖掉 `LV_RADIUS_CIRCLE`。历史页空态的
  `Start Print` 走的是另一条路：`ui_user_make_primary_pill_btn()`，那枚按稿就是渐变胶囊
  （首页左下那枚 `Start Print` 也走这个工厂，靠后两个参数 `width`/`spread` 换成定宽 +
  图标贴左文字贴右，见「首页待机布局」）。

## 页面（`lib/pages/`）

| Page ID | 文件 | 说明 |
|---------|------|------|
| `user_home` | `page_user_home.*` | 首页：环境、打印模式、预览、打印结果页 |
| `user_history` | `page_user_history.*` | 打印历史：三 Tab + Storage 条 + 日期分组卡片 + 管理态批量删除 + 左滑删除 |
| `user_control` | `page_user_control.*` | 控制面板 + 更多控制（页内两视图）。见下方「控制页」一节 |
| `user_settings` | `page_user_settings.*` | 设置（浅色档）：8 项左导航 + 右栏白卡版式。Network（Wi-Fi 开关/列表/子页）、Display & Brightness（背光滑条 + 熄屏选择子页，占位）、Storage（容量 + 格式化 + 文件管理，仅前端）、About（版本 + Galvo/MCU OTA）、Debug 密码弹窗。按稿 6089:11322 / 6235:91450 / 6240:92007 / 6251:94362 实现 |
| `user_message` | `page_user_message.*` | 消息中心（浅色档）：三 Tab + All read 浮层顶栏 + 日期分组混排列表（MCU 报警 + 设备事件）+ 初始 10 条与底部 Load More + 左滑删除历史 + 右侧 632 详情抽屉（Reset 三态）。按稿 6230:89616 / 6091:13405 / 6091:13612 / 6091:13847 实现 |
| `user_debug` | `page_user_debug.*` | 调试控制台；Move 页振镜卡 Abs X/Y + Move 走 `send_galvo_abs_pos`；Other 页按 Process / Cabin / Print Gates / Media & Sim / Inject 分组。`Galvo Point Test` 在 Print Gates，按当前校准表依次显示中心和四角红光点，与正式打印及 Test image 双向互斥。O2 Calib 在左列：输入参考浓度后点圆钮写 `/userdata/CAL/o2_cal.json`；失败 toast 提示、标签仍显示原 K。Media & Sim 的 **Sys HUD** 打开后，`ui_sys_hud` 在 `lv_layer_sys` 右上角红字显示 CPU 温 / 整机与本进程占用 / 内存（`upc_sys_monitor_sample` 读本机 `/proc`，不经 IPC），不进布局、不可点，仅本次运行 |
| `user_timelapse` | `page_user_timelapse.*` | 延时摄影播放器（隐藏页，历史缩略图进入；全屏 `lv_layer_top`） |

注册仅在 `ui_app.cpp`；打印流程走 `control_*`，pages 不直接订阅 MsgBus。详见 `lib/pages/README.md`。

### 首页待机布局（浅色稿 6201:85911）

浅色改版把首页从「整机渲染铺满整屏 + 四角浮层」换成了「`#f5f5f5` 浅底 + 定位摆放的
一张设备图 + 规整的卡片区」。`g_normal_area` 仍是铺满整屏的那一层，子元素全是
`FLOATING`，坐标直接抄稿上的整屏坐标系：

| 位置 | 控件 | 说明 |
|------|------|------|
| 设备图 | `build_main_area()` 内 | 700 见方、带 alpha，摆在 `(USER_HOME_DEV_IMG_X, _Y)` = 稿 (663, 172)。**右下两侧都是出血的**（663 + 700 = 1363 > 1280、172 + 700 = 872 > 800），超出部分由 `area` 裁掉，稿本身就这样 |
| 左上 | 问候语 `build_greeting()` | `Good morning` / `Good afternoon` / `Good evening` 按本机时段切（稿只画了第一态，另两档照它的句式补），副标题 `Ready for your next print?` 固定。顶距 `USER_HOME_GREET_TOP`（稿 160）、左缘与 HOME 卡同一条竖线 |
| 左中 | HOME 空气质量卡 `build_home_card()` | PM2.5/PM10 可切换 + 四段刻度 + 游标。`USER_HOME_CARD_W` × `USER_HOME_CARD_H`（稿 240×300）、顶距 `USER_HOME_CARD_TOP`（稿 287）。父层 `area` 铺满整屏，所以左对齐要自己补一个 `USER_SIDEBAR_W` |
| 右中 | 四宫格 `build_metric_grid()` | 2×2 白卡：温度/氧浓度/湿度/气压。总宽 `USER_HOME_GRID_W`（稿 340）、高与 HOME 卡齐，紧贴卡右侧隔 `USER_PAD_SM`。单卡尺寸由总宽高减去一道 `USER_PAD_SM/2` 的间距再折半推出（1280 档正好是稿的 166×146），不单开字段 |
| 设备图上 | 余量胶囊 `make_supply_pill()` | 两颗白胶囊（主色料桶图标 + 桶号），坐标换算成相对设备图左上角的比例，换档位时跟着图一起缩放、不会飘离料盒。第二颗的取值还借着 `filter_pct`，见下 |
| 左下 | 入口两枚 `build_action_tiles()` | 84 见方的摄像机方钮 + `USER_HOME_PRINT_BTN_W`×84 的深色 PRINT 胶囊，见下 |
| 右上 | 通知三态 | 不属于本页，是 `ui_notice_stack` 的全局对象，见「消息中心与通知三态」 |
| 右下 | 开门滑块 `build_door_floating()` | `SLIDE TO UNLOCK`（三处开门滑块统一同一个串：首页、监控页、放版弹窗，控件的默认兜底值也是它；取件那条仍是 `SWIPE TO COLLECT`）。到底那帧的 `RELEASE TO OPEN` 是控件内写死的，四条滑块共用。轨道走 `USER_HOME_SLIDE_W`×`USER_HOME_SLIDE_H`（稿 453×94）、旋钮 `USER_ACTION_BTN_SZ`（稿 84，上下各内缩 5） |

**底部那一行的退让量是不对称的**：横向仍是 `USER_PAD_LG`（稿 32，与上方卡片同一条竖线），
纵向是 `USER_HOME_EDGE_GAP`（稿 48）。稿上那 48 落不回现有的内距梯度（1024 档的
`USER_PAD_LG` 反而比 1280 的大），所以单开了一个字段。滑块的底距还要再减掉旋钮的内缩：
稿上与左下按钮上下对齐的是**旋钮**（两者顶都在 668、底都在 752），轨道比旋钮上下各高出 5。

**开门滑块走控件的浅色档**（`ui_slide_unlock_cfg_t::light_theme`）：白 90% 轨道、深色
`#333145` 滑块、深色提示字，轨道与滑块都从全圆改成圆角矩形（稿 28 / 24）。差异集中在配色与
圆角、**结构一模一样**，所以做成一个开关而不是另起一套控件；其余三处调用不设它，观感不变。

浅色档还另外分了两处，都是稿上与深色那三条不同的地方：

- **旋钮里是右箭头**（稿 6201:85974），不是控件默认那枚 `ic_door_open`，由 `cfg.knob_icon`
  传 `ic_arrow_right_set` 挑出的原生尺寸。图标占比也不同：深色档是 `knob_sz * 11/20`
  （约 0.55 直径），稿这里是 32/84，故浅色档取 `knob_sz * 8/21`，三档算下来 48/32/25，
  与生成脚本里 `ICON_SIZES["arrow_right"]` 一致、上屏 1:1 不缩放。
- **没有那三个 chevron**。`build_chevrons()` 只在深色档建。

**两行提示是随拖动量连续叠化的**（`apply_progress()`，五条滑块都生效）：

| 元素 | 静止 → 到底 |
|---|---|
| `hint`（`SLIDE TO UNLOCK`） | `text_opa` `LV_OPA_70` → 0，同时 `translate_x` 由 `hint_off` 归零（边淡出边左移到轨心） |
| chevron（仅深色档） | `line_opa` `LV_OPA_50` → 0，跟着上面那行走 |
| `release`（`RELEASE TO OPEN`） | `text_opa` 0 → `LV_OPA_COVER`，位置恒定在轨心 |

几个当初定下来的口径：

- **两行字必须是两个独立标签**。早先是一个 `hint` 在三态之间换文案、拖动中整行
  `HIDDEN`，那样做不出叠化——同一个标签没法和自己交叉淡入淡出。
- **进度的分母是 `threshold_w` 不是 `max_w`**：到阈值就是「可松手解锁」那一刻，那时
  `RELEASE TO OPEN` 必须已经完全显出来，再往后拖不该还有变化。
- **两条斜坡错开、各占一半行程**，不是同步对冲。两行字最终都落在轨心、长度又接近，
  同步淡入淡出的话中段是两行半透明的字直接叠在一起、糊成一片；错开之后任一时刻至多
  一行可见（行程 50% 处两行都为 0，是一次干净的交接）。
- 用 `text_opa` / `line_opa` 而**不是**对象级的 `lv_obj_set_style_opa()`：后者一旦小于
  255，LVGL 会把对象先渲染到独立图层再合成，而 `apply_progress()` 是拖动期间每帧都跑的。
- 回弹不必另写：`return_fill()` 的动画走的也是 `set_fill_w()`，叠化自动倒放。
- `apply_state()` 因此只剩末端红色氛围层（`tint`）与 `at_end` 标志，不再碰文字。

- **静止态的提示文字居中在「旋钮右边那段白区」**，不是整条轨道。稿实测文字中心 1048.5，
  轨道（779..1232）中心 1005.5、旋钮右侧区间（868..1232）中心 1050。

  落地方式是 `lv_obj_center(hint_row)` 保持整轨居中、再用 `translate_x` 右偏
  `hint_off = (inset + knob_sz) / 2`——这是白区中心减轨心的闭式解，三档算出
  66.5 / 44.5 / 35.5，1280 档与稿实测的 44.5 分毫不差。**只有浅色档非零**，深色那四条
  滑块没有同口径的稿，`hint_off = 0` 维持整轨居中。

  早先这里按整条轨道居中（与稿差 45px），当时注释里担心「1024 档文字左缘距旋钮右缘只剩
  个位数像素」——**按现在的字号复核过是多虑了**，三档余量分别是 108 / 54.4 / 36.9px；
  深色那四条即使也开偏移，最紧的一档（`SWIPE TO COLLECT` @1024）也还剩 63.8px。
  （Figma 的 `justify-between` + 295 宽容器推出来的是 1079.5，比渲染图还靠右 31px，
  **推导对不上渲染，以渲染图为准**。）

**首页这条滑块不与 `USER_DOOR_SLIDE_*` 共用尺寸**：那一族是控制页、打印态、监控态那三条
深色红渐变滑块，出自各自的稿、1280 档是 400×80。两边都**不复用 `USER_SLIDE_*`**：那一组
还被延时摄影页的拖动预览框（`USER_SLIDE_W * 3/5`）和放版弹窗滑条的最小宽钳制用着。

**HOME 卡内部全是绝对定位**，不走 flex——稿本身就没有流式结构，硬套 flex 反而要为每一格
编一套间距。卡内坐标用 `card_px()` 按卡宽比例折到当前档位，**基准是 1280 档的卡宽 240**，
改 `user_home_card_w` 就要改 `card_px()` 里的除数。底是预生成位图（见下），分级阈值、四档
判定、游标插值这些逻辑一个没动；只有色条从「绿/黄/橙/红四段」换成了稿上的「白色四段、
不透明度 20/40/60/80% 递增」——分级改由游标位置和分级大字表达，色条只当刻度，所以早先
那枚 `air_level_color()` 已无调用点、删掉了。

卡内那几处稿值：房子图标 `(24,24)` 24 见方、标题 `PM2.5` 在 `(52,24)`（20px、白 90%）、
切换钮 `(180,12)` 48 见方、分级大字块顶 136（40px）、读数 198（24px）、色条 `(24,262)`
192×8、游标 20 圆点在 `(36,256)`。**色条下原先那四个刻度名（Good/Fair/Poor/Bad）新稿去掉
了**，卡下方原先那两行 `Home Status` / `Cabin Status` 说明小字也去掉了——`build_status_hints()`
整个删除，连带主题里只给它俩用的 `user_light_hint` 一并删掉。

**四宫格每格右上角原先那枚图标新稿也去掉了**，只剩左上 caption 与左下读数。caption 与读数
都是「大字 + 小一号后缀」的组合（稿 `Temp.` 20 配 `(°C)` 13、`592.0` 32 配 `kpa` 20），
LVGL 一个 label 只能一种字体，所以各自拆成两个 label 横排。**flex 只能按盒子边对齐、没有
基线对齐**，两个字号盒底对齐时小字的基线会偏低，差值正好是两份字体的 `base_line`（下伸部）
之差，由 `baseline_pad()` 补成小字的 `pad_bottom` 抬回去。

**余量胶囊**（稿 6201:86004 / 86016）：白底全圆、`ic_barrel_1_set` / `ic_barrel_2_set`
两枚**主色料桶图标** + 20 的黑字。两颗分别钉在设备透图里那两个料桶上，**桶号要与位置
对上、不能对调**。几何照稿：底 white @90%（压在设备图上，不是纯白）、内距左 12 / 右 16
（文字那侧多留一截）/ 上下 11（胶囊高 44）、图标↔文字 4、图标边长 20。

数据取 `control.cpp` 那份现成的 supply 快照（`control_supply_get()` + 观察者，**别在页面
里再注册一次 `register_supply_status`**，那个槽是单槽的）。

> **一处等后端补数据**：第二颗胶囊的取值是借来的。协议的 `SupplyStatusPayload`
> （`common/upc_contract.hpp`）只有 `material_pct` / `filter_pct` 两项，**没有第二桶的
> 余量**，所以 `update_supply_pills()` 暂时拿 `filter_pct` 顶着 2 号桶。后端补上字段后
> 改那一行即可，几何与样式不用动。
>
> 早先这两颗的语义是照更早的草稿猜的（「左=耗材、右=滤芯」，那版稿的图标还是从四宫格
> Temp / Humidity 复制过去的占位），新稿的料桶编号把答案给了一半：两颗都是料桶。

**那批 A8 指标图标的显示边长走 `USER_METRIC_ICON_SZ`**（1280 档 32 = 稿值，1920/1024
两档按 ×1.5 / ×0.8 折算成 48 / 26）：打印态/监控态各卡头的图标。浅色稿首页那批卡头图标
（HOME 卡头的房子与切换钮）是 24，按 `*3/4` 折算——三档整除得 36 / 24 / 19，
见 `light_icon_sz()`。余量胶囊里那枚料桶稿上是 **20**，走 `light_icon_sz() * 5/6`
（即 `USER_METRIC_ICON_SZ * 5/8`）得 30 / 20 / 16，与 `ic_barrel_*` 的尺寸表对齐。
首页左下两枚按钮与滑块旋钮里的图标是 40，按 `*5/4` 折算得 60 / 40 / 32，见 `action_icon_sz()`。

左下那两枚（`ic_camera` / `ic_folder`）**原先是 `home_btn_icons.c` 里两张 96×96 的手维护
资源**，每次都要缩到目标尺寸。浅色稿给了新图形（相机的机身从直角变成大圆角、文件夹的标签
与横线也不同），借这次一并收进 `gen_home_metric_icons.py` 出多档原生尺寸，`home_btn_icons`
那对文件随之整个删掉。`ic_folder` 比 `ic_camera` 多出 45 / 27 两档：历史页空态那枚
`Start Print` 主按钮也用它，那边的边长跟的是 `USER_BTN_PRIMARY_H * 5/8`。

**这批图标按档位各存一份原生尺寸的位图，运行时 1:1 不缩放。** 原先一张 40×40 喂三档，
而 LVGL 的 `lv_draw_sw_transform` 是 2 抽头点采样、不做面积平均，缩小时步长大于一个源
像素就会整行整列跳过源数据——实测切换钮的横杠 alpha 在 32 档 225..251、26 档 188..220
之间周期性起伏且峰值到不了实心，屏上就是毛刺、发糊甚至断续。改成原生尺寸后横杠是平的
255。资源由 `tools/gen_home_metric_icons.py` 生成（大小两套尺寸 `METRIC_SIZES` +
`METRIC_SM_SIZES` 现在对**每一枚**图标都出，`ic_switch` 的横杠还额外做了像素网格吸附），
挑图走 `page_user_home.cpp` 的 `pick_icon()`。**改 `USER_METRIC_ICON_SZ` 必须同步改脚本
里那两组尺寸**，否则挑不到等宽的就又退回缩放。同一套做法见 `ui_message_store.cpp` 的
`ui_message_severity_icon()`。

四宫格与 HOME 卡头的图标里，`ic_temp` / `ic_o2` 是从稿重描并收进生成脚本的（此前在
`home_assets.c` 里、源矢量已丢失）。**描边沿用工程原有的 1.6 而不是稿的 1.0**：稿值折到
低档位是亚像素、峰值 alpha 只有 211，且会和同一列里 `ic_humidity` / `ic_pressure` 的
1.6 劈叉。`ic_pm` 与 `ic_aqi` 已无调用点、一并删掉，`home_assets.c` 随之整个移除，
`home_assets.h` 现在只剩设备图与 HOME 卡背景的声明。

**两张预生成位图**（`tools/gen_home_bg.py` 生成，都按屏分辨率出两份）：

| 符号 | 尺寸（1280 / 1024） | 格式 | 源节点 | 挑图 |
|---|---|---|---|---|
| `home_device_*` | 700² / 560² | RGB565A8（带 alpha） | 6201:85912 的图片填充 | `home_device_dsc()` |
| `home_card_bg_*` | 240×300 / 192×240 | RGB565（不透明） | 6201:85915 的卡背景 | `home_card_bg_dsc()` |

合计约 2.52MB，1920 档复用 1280 那份（放大 1.5×、会软，该档不出货）。两张都用
`LV_IMAGE_ALIGN_COVER`，1280/1024 两档尺寸与目标框严丝合缝、COVER 落到 1.000、零重采样。

设备图那张的合成有四件事要一起做，**顺序不能乱**：

1. **不是 `object-cover`。** 稿上图片在 700 框里是 `w 161.61% / h 130.44% / left -38.18% /
   top -10.72%`，即先按框边长放大到 1131×913 再左上偏移、由框裁掉外面的部分。源图
   3837×3097 的左侧还带着一排设计用的色卡，其中最右一块**落在裁窗里面**（源坐标
   x 933..1125），上屏会是左缘一块突兀的蓝方块——稿上它正好被四宫格第二列那张白卡盖住，
   但那是布局巧合不是设计意图，生成时按源坐标整列抹掉（`DEV_SRC_CLIP_X`）。
2. **`mix-blend-darken`**：背后是纯色页面，逐通道 `min(src, 0xF5)` 即可，不必真走混合模式。
3. **四道把边缘淡入 `#f5f5f5` 的线性渐变**。遮罩色与页面底色是同一个，所以等价于把设备的
   alpha 乘上 `(1 - 覆盖率)`——省一层合成，资源也还是干净的 RGB565A8。注意底那道峰值是
   1.0 且满值位置在 41%，框下半截整个是看不见的，稿上设备本来就只露出上面一截。
4. **两块料面**（稿 6201:86027 / 86028）压在最上层，**要在四道淡入之后**。它们不在那张
   位图里，稿上是两个独立的矢量层摆在整帧最后；烤进来不影响层序，因为它们与两颗余量胶囊
   实际不相交（料面底缘 320/351 都在胶囊顶缘 325/353 之上）。

透明像素的 RGB 统一垫成页面底色 `#f5f5f5`，缩放时万一渗到边缘也看不见——**别垫黑或白**：
前者会给玻璃描一圈暗边，后者在 1920 档放大时边缘发灰。

HOME 卡背景把稿上三层里的两层烤进位图（**卡的圆角不烤**，交给 LVGL 的 `radius` 裁，换圆角
不用重跑脚本）：

1. 外层 335×317 的线性渐变 `#04091a → #020916` —— **不用画**，它被下面那层 279×315 的
   不透明径向渐变整个盖住（两层都居中摆在 240×300 的卡上、都比卡大），一个像素都露不出来；
2. 内层径向渐变 —— 卡的实际底色，`#2261F5 → #A3C3FF` 五档。中心几乎贴着卡底缘（卡坐标
   `(119.92, 303)`、半轴 233×263），所以卡顶最远取末档的浅蓝、卡底最近取首档的深蓝；
3. `Dots` 点阵（1560 个各带自己那份径向渐变的小圆），`mix-blend-mode: plus-lighter` 加色。

改版前还有第四层「底缘往内的白色柔光」（两层 `rgba(255,255,255,0.3)` 的 inset 阴影，LVGL
画不了内阴影只能烤）。**新稿去掉了**：实测旧稿卡底 3px 内由 `(49,113,255)` 提亮到
`(101,148,255)`，新稿一路加深到 `(32,96,245)`。

**那层是整屏，不是内容区**：`lv_obj_set_pos(area, -USER_SIDEBAR_W, 0)` + 宽度取 `LV_HOR_RES`
跨回本页根节点的 `pad_left`，与打印态/监控态的摄像头层同一套做法。稿上的坐标都是整屏坐标系
（设备图 x=663、HOME 卡 x=160 都含侧栏那 128），只有铺整屏才能直接照抄；**若改回只铺内容区，
所有 x 都要减去一个侧栏宽**。代价是 `area` 里左对齐的控件要自己补一个 `USER_SIDEBAR_W`。

改版前那套「整机渲染铺满整屏」的 `home_bg_*`（3.36MB）已随浅色稿退场。当时踩出来的两条经验
留在 `gen_home_bg.py` 的文件头里，免得日后有人重做整屏背景时再踩：平移补偿要跟着布局一起
重算（它是补偿不是设计值）；平移后空出来的边要用主体同宽的**水平镜像**补，不能拿「最边上
一列 resize」——单列拉宽后每行退化成纯色横杠，上屏就是边缘一片竖向条纹（实机复现过）。

**左下两枚入口**（稿 6201:85989 + 6201:85981）：

```
┌────┐ ┌──────────────────────┐
│ 📷 │ │  [文件夹]   PRINT      │
└────┘ └──────────────────────┘
```

| 项 | 取值 |
|---|---|
| 位置 | 左缘距侧栏退 `USER_PAD_LG`（稿 32）、底缘距屏底退 `USER_HOME_EDGE_GAP`（稿 48），**两者不同值**。父节点是本页根节点、`pad_left` 已让开侧栏，不用再补侧栏宽 |
| 左钮 | `make_light_square_btn()`：`USER_ACTION_BTN_SZ` 见方（稿 84）+ `USER_LIGHT_RADIUS`（稿 24），`user_light_glass` 底 + 白描边 + 淡投影，只有一枚 `ic_camera` 图标、没有文字 |
| 右钮 | `USER_HOME_PRINT_BTN_W` × `USER_ACTION_BTN_SZ`（稿 202×84）+ 同样的圆角，`#333145` 深底、白字 `PRINT`、图标 `ic_folder` |
| 图标 | 两枚都取 `action_icon_sz()`（稿 40） |
| 间距 | 两枚之间 `USER_PAD_SM`（稿 16） |

相比改版前那一行「两枚等高全圆胶囊」（`Monitor` + `Start Print`），浅色稿把左边缩成了纯图标
方钮、右边换成圆角矩形并把文案缩成 `PRINT`。于是整行不再定宽（`USER_HOME_ACTION_ROW_W` 随之
作废），两枚各自定宽、按内容排。右钮也**不再复用** `ui_user_make_primary_pill_btn`：那个组件
是全圆胶囊、主色蓝底，与稿这里要的圆角深底已经不是同一枚按钮了。

稿上那两处白色都带 `backdrop blur(15px)`，LVGL 没有背景模糊，直接给不透明白——底下是
`#f5f5f5`，肉眼差别只在边缘那一点点。同一处理见铃铛。

### 打印态浮层（六态一套）

打印中与打印结果不是两套界面，而是 `page_user_home` 打印浮层 `g_print_area`
**内部**的同一层 `g_print_overlay`（稿 3588:8735 / 4045:17582 / 3589:9992 /
3588:8907 / 4234:6385 / 4045:18253）。摄像头背景与 `CameraPreviewPlayer` 全程
不断流，避免黑屏闪烁。

浮层里的常驻控件：

| 位置 | 控件 | 说明 |
|------|------|------|
| 左下 | stats-card `g_pc_card` | 缩略图 + 徽标行（图标/状态词/层号/百分比）+ 进度条 + TIME/CONSUMABLES 两列。1280 档宽 `USER_PRINT_CARD_W` 取稿值 416，按稿分解正好是 16 + 缩略图 100 + 16 + 右列 268 + 16 |
| 右上（环境区下方） | 通知浮层 | **不在本层**，是全局的 `ui_notice_stack`；进出打印态时按环境区高度把它往下压 |
| 右下 | 运行组 `g_ctl_run` | 暂停·恢复钮 + 终止钮；与取件组互斥 |
| 右下 | 取件组 `g_ctl_collect` | 吹粉钮 + `SWIPE TO COLLECT` 胶囊；与运行组互斥。胶囊里那颗圆头显式给 `cfg.knob_sz = USER_PRINT_CTL_BTN`（64、相对轨道内缩 8）——稿 4972:32141 那颗红钮在组内 x=96，而滑块从 `blob+gap`=88 起，正好内缩 8；不给这个值控件会按 `track_h` 推出 72/内缩 4 |
| 右中 | 拍照钮 `g_result_photo_btn` | 六态都有；`make_glass_round_btn()` + `ic_photo`，直径同 `USER_PRINT_CTL_BTN` |
| 右上 | HOME PM2.5 卡 + 4 列环境条 | 六态都有；`build_env_wrap()`，右边缘与拍照钮同齐（都退 `USER_PAD_LG`）。稿上的舱内 PM2.5 那列已删，见「右上环境区」 |

**本层所有浮层退的都是 `USER_PAD_LG`（1280 档 32）**，不是 `USER_PAD`：稿上 stats-card
在 x=160 y=636、运行组 x=1080 y=688、取件组 x=760 y=688、环境区 x=699 y=32，右缘一律
1248、底缘一律 768，换算下来距屏边就是 32。左对齐的那两个（stats-card、终止态失败原因条）
还要各补一个 `USER_SIDEBAR_W`，因为本层已随摄像头层铺到屏幕左边缘。待机监控层的 EXIT 钮、
开门滑块、拍照钮同理。

两个状态量决定外观，改哪个都走 `apply_print_phase()` 一处刷新：

- `g_print_phase`（徽标五档）：`PRINTING...` 蓝 / `PAUSE` 蓝 / `PRINT TERMINATED` 橙 /
  `PRINT COMPLETE` 绿 / `OPTICAL PATH CHECK` 蓝（打印前光路检测，层号位显示 n/5，暂停钮灰掉）。
  徽标色同时是百分比色与进度条填充色。
- `g_purify_stage`（结束态下的吹粉子阶段，走 `purify_stage_set()`）：`IDLE` / `RUNNING`
  （吹粉钮图标 2s 一圈逆时针转、胶囊文案换 `AIR PURIFYING...`）/ `DONE`（通知换口罩提醒）。
  按稿 4234:6385，吹粉中**不改徽标**——吹粉态只靠状态栏与胶囊文案表达。

稿上卡片上方那条提示（结束态两个变体：催吹粉 / 提醒戴口罩）不由本层画：
`apply_print_phase()` 把它们推进消息中心，态一变就撤回，见「消息中心与通知浮层」。
这样离开首页也看得到，且与后端上报的告警排在同一摞里。

两列语义分态（`TIME` 列的 caption 由 `apply_print_phase()` 改写）：

| 态 | TIME | CONSUMABLES |
|----|------|-------------|
| 打印中 / 暂停 | `TIME LEFT`（进度推送的 `remaining_minutes`） | `--`（打印中拿不到用量） |
| 中断 / 完成 / 吹粉 | `TIME COST`（manifest `duration_sec`，H:MM:SS） | manifest `material_g` |

层号 `g_pc_layer` 摆在**徽标行**、紧贴百分比左侧，不是往两列旁边再插一列：右列
只有 268 宽（1280 档），两列均分后 `CONSUMABLES` 已经得靠 `user_font_caption_sm()`
才不被裁，再插第三列每列剩不到 90，那个词必溢出。徽标行有 spacer 撑出的空档，
`999/999` 放得下。取 dim 色 + 小一档字号，避免与同色的状态词/百分比抢视线。

显示的是「正在打第几层」（后端 `current_layer` = 已完成层 + 1），`total_layers`
为 0 时整条隐藏——OVF 和无 `M3000` 的 G-code 数不出总层，不留 `--/--` 占位。开始
打印时先隐掉，等第一帧进度推送才显。结束态与卡片其它字段同样定格在最后一次推送
上（完成态即 `N/N`，中断态停在断点那层）。

底部两组控件的玻璃底衬（稿 4796:6172 运行组 / 4231:6034 取件组）是一张预生成的
ARGB8888 图，`make_ctl_backdrop()` 直接贴，按 profile 三档挑（`ctl_backdrop_dsc()`）。

之所以不用 LVGL 图元拼，是踩过两个坑：

- 稿上那条 Union 是「圆 blob + 细腰 + 圆/胶囊」并集后又给结果路径加了圆角平滑，
  腰部是**内凹**圆滑过渡；圆角矩形只能凸，拼出来是尖角。稿上还有一圈 1px 白 10% 的
  内描边，各块自描会把藏在并集内部的圆弧也画出来。
- 半透明块相互重叠时交叠处会叠深。注意 `lv_obj_set_style_opa()` 是**逐控件继承**的，
  每个子块各自混合一次；触发整棵子树先合成再整体混合的是 `opa_layered`
  （见 `lv_obj_style.c` 的 `lv_obj_get_layer_type()`）。

出图脚本 `tools/gen_ctl_backdrop.py` 从设计稿 SVG 栅格化，填充色（黑 50%）和描边都
烤进像素，运行时一张图一次混合。取件组的胶囊长度不随 blob 等比变化，所以先按稿比例
等比渲染、再只在胶囊直段（稿上 x∈[128,448]，每列相同）按列增删到目标宽，圆头和细腰
保持原样。

几何全部由 `USER_PRINT_CTL_BTN` 一个常量推出：blob（底衬上那颗圆的直径）=
`ctl_blob()` = btn×5/4，两块之间的缝 = `ctl_gap()` = btn/8，均取自稿的 64→80→8。
三档 btn 为 96 / 64 / 51，是稿（1280×800，btn 64）按屏宽等比折算，故 1280 档得到的
168×80 与 488×80 正是稿的原尺寸、零重采样。**改 `USER_PRINT_CTL_BTN` 或
`USER_PRINT_PILL_W` 必须同步改脚本的 `PROFILES` 并重跑**，否则底衬与按钮对不齐。
注意 btn 同时还是右侧中部拍照钮的直径（稿上两者同尺寸）。

### 右上环境区（打印态 / 待机监控共用）

`build_env_wrap()` = HOME PM2.5 卡（稿 4231:5740）+ 4 列环境条（稿 4231:5749）。
四列 caption 全是文字（`Temp.` / `O2` / `Humidity` / `Pressure`），不是图标。
整体贴右上、退 `USER_PAD_LG`（稿 4972:31316 是 x=699 y=32，右缘 1248 → 距屏右/屏顶各 32），
与右侧中部的拍照钮右边缘齐。

**以新版稿 4972:27560 为准，那一版本身就是四列** —— 舱内 PM2.5 那列是设计在这一版去掉的，
与首页右侧 CABIN 面板早先按稿 5277:11590 删掉同名那行是同一个动作。旧版 frame
（4231:5749 / 4972:31258 / 4972:32054）还是五列，**对稿时别照旧版把它加回来**。

几何全取新版稿：内边距 8（`USER_PAD_SM/2`）、列间距 12（`USER_PAD_SM*3/4`，1280 档正好
整除）、每列定宽 64（`USER_ENV_COL_W`），整条宽度因此落在稿的 **308**。

**字号比稿小一档，是为了塞进稿的 64 定宽**：稿是 caption 14 / 值 16 / 单位 10，而工程排
Montserrat（稿是 Poppins，同字号更宽）。实测 `Humidity` 的 caption @14 要 66.6px、装不进
64，降到 `caption_sm`（1280 档 12）后是 57.1，四列最宽也只有 62.1。值取
`user_font_text_sm()`（16，稿值），单位取 `caption_sm`（12——Montserrat 没有 10 号）。
`USER_ENV_COL_W` 为 0 的档位（1024/1920）装不下稿的等比列宽，退回按内容自适应。

**PM 卡的宽度仍是内容自适应，没取稿的 157 定宽**：最坏文案 `Very Poor (150.0 ug/m3)` 按
稿字号实测要 165.1px，而 157 扣掉两侧 16 的内边距只剩 125，差 40px，降字号救不回来。
根因是稿上那行画的是短文案 `Fair (25μg/m³)`，没考虑最长分级词加三位小数的读数。

打印态浮层与待机监控层是**同时建、靠显隐切**的两层，所以这一块的 label 句柄不能是
单例——后建的会覆盖先建的，先建那层从此收不到刷新。八个句柄收进 `env_block_t`，
`g_env_print` / `g_env_monitor` 各一份，都登记在 `kEnvBlocks[]` 里；
`result_pm_card_update()` 和快照回调遍历这个数组，两份一起写。

### 待机「查看监控」（稿 3122:13224）

右上环境区、右侧中部拍照钮都与打印态共用上面那套构造函数，几何配色天然一致。
左下角是同款玻璃圆钮（图标换 `ic_home`）退出监控，右下角保留 `SLIDE TO UNLOCK` 滑块。

拍照钮的图标是 `ic_photo`（照相机，稿 3847:9440），不是首页左下「查看监控」入口那枚
`ic_camera`（摄像机）——两处语义不同，别图省事复用。

时序：

```
Glavo Print_Terminate_Rsp / 进度 100% → control.cpp finish_printing()
  → enter_state(CONTROL_STATE_PRINT_DONE)   // 只停看门狗，预览保活，界面维持打印中原样
  → （开门 finalize：promote 目录；`UPC_TIMELAPSE_PLAYBACK_PNG=1` 时不编 timelapse.mp4，保留 PNG 即可播；`=0` 时后台编 mp4）
  → APP_Print_Video_Manifest_Ntf (Camera→UI)
  → control.cpp on_print_video_manifest()：按 frames_dir 主键读 /userdata/media/video/config.json
  → page_user_home_enter_print_result()     // 徽标切结束档 + 底部换取件组
```

- **数据源**：`has_entry=1` 时以 manifest 条目为权威（`status` / `duration_sec` / `material_g`）；
  `has_entry=0`（0 帧未落盘）时用 payload 的 `aborted` 判完成/终止，耗时耗材显示 `--`。
- **出口**：右下 `SWIPE TO COLLECT` → `control_print_result_collect()` 直接进 `DOOR_OPENING`
  走开门流程，**刻意不借道 IDLE**（IDLE 分支的 `page_user_home_exit_printing_mode()` 会停预览
  并收掉浮层，弹窗就浮在待机首页上了）。结果页与预览原样留在弹窗背后，等关门确认后才由
  `PrintCollect` 分支统一回 IDLE 收尾。
  新稿去掉了右上那颗「回首页」圆钮——离开结果态改点左侧标签栏，页面被 PageManager
  缓存住，再点首页仍回到结果态。`control_print_result_dismiss()` 因此暂时没有 UI 入口，
  作为 API 保留。
- **PRINT_DONE 期间** `control_is_printing()` 返回 true → PageManager 挡住 `user_files`，
  取件前不能开新任务。
- 吹粉钮：触发清粉测试；Media 按 `TIMELAPSE_VIDEO_FPS` 抽帧接续 PNG，成功后合并进同一
  `timelapse.mp4`（一次打印结果仅允许成功一次）。稿上「吹粉完成前门钮/滑动取件置灰」
  的门禁还没做，当前滑动取件从一开始就可用。

### 转运盒流程（稿 3171:7305 / 3225:10476 / 3225:13586 / 4811:6178+6191）

滑动取件开门之后、关门确认之前的一段：把工件转移进转运盒再取下。默认**不启用**，由调试页
`Other` → `Print Gates` → `Transfer Box` 打开（`control_set_transfer_box_flow()`，纯 UI
本地标志，不发消息也不落盘，重启回到关闭）。

```
SWIPE TO COLLECT → DOOR_OPENING（PM2.5/清粉 → 开门）
  → TRANSFER_BOX  ① BoxInstall  装盒：满宽按钮由「Transfer Box Not Ready」置灰翻可点
                  ② BoxMoving   转移：只有转圈，无按钮无关闭钮
                  ③ BoxShutter  推闸门：「Unlock and Remove the Box」直接可点
                  → 绿勾 toast「Transfer complete...」
  → DOOR_CONFIRM  关门确认（取件分支专用样式）→ clean_test_close → IDLE
```

- **共用门弹窗骨架**：`DoorVariant` 扩了 `BoxInstall/BoxMoving/BoxShutter` 三档，`show_door_dialog()`
  一个不变，只由 `door_apply_variant()` 换顶图与内容列。右上关闭钮只在 `Opening/Cleaning`
  显示，转运盒三步一律隐藏——门已经开了，必须走完才能关门。
- **关门确认分态**：`transfer_box_flow_active()`（开关打开 **且** `g_door_origin == PrintCollect`）
  为真时走新稿——两态同标题 `Confirm the door is closed`、口罩徽标改成「开门期间持续佩戴」、
  没有灰字说明行，只有顶图（置灰态 `img_mask` / 可点态 `img_door_close_hint`）和按钮随门是否
  关好切换。开关关闭或另两个开门入口（打印前置、首页独立开门）仍是原来的两态两标题。
- **`control_is_printing()`** 把 `TRANSFER_BOX` 也算进打印占用，转运期间同样挡住 `user_files`。
- **装盒到位占位**：① 仍靠 `BOX_READY_MS` 定时器翻 `g_box_ready`（复用 `g_door_advance_timer`）。
- **自动转运**：Ready 确认 → `send_transport_request()`（串口 A2/0x41 TLV 0x02）并进
  BoxMoving；等 `APP_Transport_Result_Ntf`：`result=0` → BoxShutter；`1/2` 失败/中止 →
  **停留转圈页**，无超时重试。③ 闸门稿上本就没有到位检测，用户点完成。

### 缺粉弹窗（稿 4273:5892）

打印中粉量位 bit0/bit1 同时为 0 时，Glavo 自己先暂停送料再推 `APP_Print_Powder_Low_Ntf`
（同时另发一条 `APP_Print_Pause_Rsp(paused=1)`，徽标与状态栏因此自动翻到 PAUSE）。
UI 侧只做提示：`control.cpp` 的 `show_powder_dialog()`，几何复用门弹窗那套
（496 卡片 + 496×321 顶图 + 标题/副标题 + 满宽主副按钮）。顶图在稿里也还是占位。

挂 `lv_layer_top()` 而不是 `g_overlay`：打印中用户可能正待在别的标签页，`g_overlay`
只覆盖首页那一屏。

**判空一律在后端**，UI 不自己猜粉量：

| 操作 | UI 动作 | 之后 |
|------|---------|------|
| `Refilled, resume printing` | 关窗 + `send_print_resume()` | 仍缺粉 → 后端再暂停并再推一次 Ntf → 弹窗重开 |
| `Cancel` | 只关窗 | 停在暂停态；点底部播放钮恢复，仍缺粉则同上 |

作业跑完或被终止时 `finish_printing()` 会兜底关窗，避免残留。注意后端的
`powder_low_latched_` 会把重复通知压掉一次，所以「补粉后仍缺粉要再弹」这条依赖 Glavo
在收到 Resume 时重新判一次粉量——那是后端侧的事，前端按契约照收即可。

### WiFi 传输弹窗（PIN 配对 / 文件接收）

`lib/components/ui_xfr_dialog.*`，对齐 Figma 3859:9545 与 3859:9810。两者共用
`ui_modal_dialog` 外壳，只在建窗时就地覆盖两处（卡内行距 `USER_PAD_SM`、遮罩
`LV_OPA_50`），不动 `USER_DLG_*` 宏以免连带改掉打印流程那批弹窗。

挂 `lv_layer_top()` 而不是 `control` 的 `g_overlay`：配对/传输是网络事件，任何
时刻都可能到，而 `g_overlay` 在 IDLE 态是隐藏的。按设计打印中也照弹。

| 弹窗 | 触发 | 状态 |
|------|------|------|
| PIN 配对码 | `XFR_Pair_Pin_Ntf` → `control.cpp wire_msg_proxy()` | 已接线 |
| 文件接收进度 | `XFR_Transfer_Progress_Ntf` → `control.cpp wire_msg_proxy()` | 已接线 |

接收弹窗由 XFR 进度推送驱动：`Uploading/Slicing` 状态自动开窗并更新进度条；
`Finished/Error/Idle` 自动收窗。`XFR_File_Received_Ntf` 仍保留原有 toast 和
本地文件列表刷新逻辑。

两处已知遗留（缺契约，非实现疏漏）：

- **取消按钮只关 UI**。UI→XFR 方向目前一条消息都没有，点 `Cancel Pairing` 之后
  后端配对会话仍然存活，手机那时候依然能配对成功。
- **PIN 弹窗只能手动关**。`XfrPairPinPayload.expires_s` 没有使用，PIN 过期不会自动
  收窗；弹窗遮罩会吞掉全屏点击，无人处理时会一直挡着（含打印中）。

文案统一用英文：CJK 子集字库 `assets/src/lv_font_cjk_data.c` 只覆盖调试测试页用字
（403 字），缺稿上的「备 / 码 / 消」，且只有单一 18px 字号。

## CameraMsgProxy / CameraPreviewPlayer（Camera）

| 组件 | 文件 | 职责 |
|------|------|------|
| `CameraMsgProxy` | `camera_msg_proxy.*` | `UI_Camera_Stream_*` / Snapshot IPC 封装 |
| `CameraPreviewPlayer` | `camera_preview.*` | dma fd 元数据环消费、NV12→BGRX、LVGL 显示 |
| `RgaDmaBuf` | `rga_dma_buf.*` | 封装 `common/ipc/dma_heap_buffer` 的 dma32 heap 缓冲，供 RGA `importbuffer_fd` |

| 方法 | 方向 |
|------|------|
| `send_stream_start()` / `send_stream_stop()` | UI → `APP_CAMERA` |
| `register_stream_start_rsp()` | Camera → UI（分辨率、SHM 元数据环名、`dma_mode`） |
| `register_print_video_manifest()` | Camera → UI（manifest 已落盘，`control.cpp` 据此切打印结果页） |

### 预览数据路径

预览统一走 dma fd 旁路（`dma_mode=1`）：

```
rongsu_camera 经主 IPC（/tmp/upc_camera.sock）SCM_RIGHTS 附带隔离 pool 的 dma_buf fd 表
  → CameraPreviewPlayer::on_stream_start_rsp 调用 MsgPort::take_peer_dma_fds(APP_CAMERA)
  → SHM /upc_camera_preview 只读 slot 状态/seq/buffer_idx
  → 解码线程 acquire_latest_frame 得到 buffer_idx
  → [RGA] importbuffer_fd(src_fd) → 裁剪 + 缩放 + NV12→BGRX → slots_[3] BGRX（上屏尺寸）
  → [SW 回退] mmap(src_fd) → nv12_to_bgrx_sw（同样裁剪 + 最近邻缩放） → slots_
  → lv_timer(16ms) 换 img_dsc_ 指针 → lv_image (XRGB8888，1:1 贴图)
```

`CameraStreamInfoPayload.dma_socket_path` 为 ABI 保留字段；fd 不经独立 socket 传递。源 fd 来自 Camera 隔离 pool（dma32/CMA），不是 ISP 的 V4L2 buf，见 [camera/CAMERA.md](../camera/CAMERA.md) 「ISP IOMMU 与 dma-buf 所有权」。

- **开流**：`page_user_home` 打印模式 → `CameraPreviewPlayer::start()` → IPC Start → `apply_stream_start_ui` 开 SHM 元数据环 + `take_peer_dma_fds` + `setup_frame_buffers` + 解码线程。
- **关流/换流**：UI 仅请求解码线程退役；后台回收线程等待退出，LVGL 定时器确认退出后才关闭旧 SHM、DMA fd 与显示缓冲，因此主线程不阻塞在 `join()`。
- **恢复职责**：Camera 侧采集看门狗是唯一的自动重连发起者；UI 只检测并记录预览卡帧，等待 Camera 重建 SHM 与重推 `Stream_Start_Rsp`，不再自行 Stop/Start 流。
- **交互降频只降转换，不降取帧**：按压 / 有动画时 `decode_target_fps_override_` 置 `kInteractiveDecodeFps`(12)，解码线程据此**跳过 RGA/SW 转换**，但 `acquire_latest_frame` + `release_slot` 始终按相机帧率跑。若连取帧一起降频，滞留的槽位会占满 Camera 的在飞窗口并反压采集线程，把 Visual 与直播一起拖到 12fps（见 [camera/CAMERA.md](../camera/CAMERA.md) 「在飞窗口」）。5s 诊断里 `skip=` 即这类跳过的帧。
- **解码线程不按帧周期补睡**：限速由 `acquire_latest_frame` 天然承担（没有比 `last_seq` 更新的帧就返回 `nullopt`），所以循环末尾**不再**睡到一个相机帧周期。补睡会把 SHM 的轮询粒度从 `kNoFrameNapMs`(5ms) 拉到整整一帧（无帧那圈只耗 5ms，末尾要再睡 28ms），凭空给预览加上平均半帧的等待。无帧时只睡 `kNoFrameNapMs`。
- **`lv_timer` 周期 16ms**（`kUiRefreshPeriodMs`，与 `LV_DEF_REFR_PERIOD` 对齐）：这一拍等的是"转换好的帧什么时候被切上屏"，周期越长平均多等半个周期。交互期仍降到 `kUiRefreshInteractPeriodMs`(66) 让位给触摸响应。
- **RGA 路径**：`setup_frame_buffers` 时 `RgaDmaBuf` 分配成功则 `rga_path=1`；失败退化为 `std::vector` 软件转换。无进程级预热，首帧到达即转换。
- **RGA 常驻导入数受相机缓冲数约束**：streaming 模式下 `prepare_rga_streaming_cache()` 开流时把**每个**源 dma fd 都 `mmap` + `importbuffer_fd` 常驻，导入总数 = `CAMERA_V4L2_BUF_COUNT` + scratch 块数（1:1 条带路径 2 块；缩放路径不需要 src scratch，只有 1 块）。rk3568 实测 4 个缓冲（6 句柄、约 11.4MB 常驻映射）可用，6 个（8 句柄、约 15.6MB）必然导入失败。**失败只会退化成软件转换（22ms/帧）、画面照常出，外部看不出异常**，所以三个失败点各自打了带 errno 的 Error 日志；调相机缓冲数后务必 `grep RGA /var/log/topic/topic_ui_preview.log` 确认 `rga_path=1`。
- **DMA 约束**：RK3568 RGA2 不是 IOMMU 客户端，仅支持 4G 以下物理地址（板端节点 `fdeb0000.rk_rga`，核对见 [common/COMMON.md](../../common/COMMON.md) `DmaHeapUsage`）。源 fd 必须来自 V4L2 / dma-heap（dma32），UI 直接用 `importbuffer_fd`。相机缓冲上的 IOMMU 是 ISP 的事，见 [camera/CAMERA.md](../camera/CAMERA.md) 「ISP IOMMU 与 dma-buf 所有权」。
- **Stride 对齐**：RGA 源 stride 使用后端传来的实际 `stride_y`，不再假设 `stride_y == width`。
- **Cache 同步**：phx streaming（`UPC_UI_RGA_STREAMING=1`）**不对相机源 fd 做** `DMA_BUF_IOCTL_SYNC`，只对 dst scratch 在 CPU `memcpy` 前后 `SYNC READ`；1:1 条带回退 memcpy 时才 `SYNC WRITE` 源 scratch。k1mini 非 streaming 才对源 fd `SYNC START READ` 再逐帧 import。软件回退两条工具链都会对源 fd `SYNC` + `mmap`。目的显示槽若是 dma 则写完 `SYNC WRITE`。
- **线程**：解码在独立线程；`on_stream_start_rsp` 与换帧须 `lv_async_call` / `lv_timer` 回到 UI 线程。
- **CMake**：找到 `librga` 时 `-DUPC_UI_HAS_RGA=1`（当前源码仍硬依赖 RGA 头文件）。

### 预览输出几何

**解码侧输出的不是相机原始帧，是上屏那张图。** `resolve_output_geometry()` 在
`apply_stream_start_ui()` 里按 viewport 一次算定，全部记在 `out_width_` /
`out_height_`（上屏尺寸）与 `src_crop_y_` / `src_crop_h_`（对应的源可见带）：

| 项 | 值（1080×1280 流 + 1280 宽屏） | 说明 |
|----|------|------|
| 源 | 1080×1280 | 相机 NV12，`UPC_CAMERA_STREAM_*` |
| 可见带 | y=302 起 676 行 | 按 `vp_h × width_ / vp_w` 反算，居中；**偶数**（NV12 的 UV 按 2×2 子采样） |
| 输出 | 1280×`vp_h` | `lv_image` 里的真实尺寸，`stream_width()/stream_height()` 返回它 |

几何是**开流时定一次**，不跟 viewport 的后续变化。viewport 真改了尺寸，
`cam_apply_layout()` 算出的 zoom 就不再是 1.000，画面仍然对、只是那段时间回到
LVGL 重采样——这是刻意的取舍：重建缓冲要停解码线程，不值得为一次布局抖动付这个代价。

三条退回 1:1 的路径（`scaled_output()` 为假，行为与改造前完全一致）：

- **屏宽 = 流宽**（1024 档 + 1024 屏）：本来就无缩放，纵向继续交给 LVGL 裁剪。
- 取不到 viewport 尺寸（`image_` 没父节点 / 宽高 ≤ 0）。
- 缩放系数超出 RGA2 的 1/8~8 倍区间（会打一条 Warn）。

两条转换实现按 `scaled_output()` 分流：

- **缩放**：`convert_frame_rga_scaled()`，一趟 `improcess`（独立 src/dst 矩形）出整张输出，再一次 memcpy 进显示槽。**刻意不分条带**——RGA 缩放器只在给定 `src_rect` 内采样，按输出行切条带会在每个边界钳出一条 1px 接缝。
- **1:1**：`convert_frame_rga_streaming()`，沿用原来按源行分条带那条路径，不动。

排查入口是 `decode ready` 那条日志的 `out=` / `crop=` / `scaled=`：`out` 与
viewport 不一致，或 `scaled=0` 而屏宽 ≠ 流宽，就说明重采样又回到 LVGL 身上了，
`refr 5s:` 的 `draw_avg_us` 会立刻涨上去。

### 预览延迟分段诊断

`decode 5s:` 一行（`/var/log/topic/topic_ui_preview.log`，`proc=rongsu_ui`）把预览延迟拆成可量化的三段。同一个文件里还有 `proc=rongsu_camera` 的 `stream 5s:`（相机侧的驱动滞留，见 [camera/CAMERA.md](../camera/CAMERA.md) 「延迟诊断」），两条对照就能定位延迟落在哪里。

| 字段 | 含义 | 正常范围 |
|------|------|----------|
| `ok` | 解码线程实际取到的帧数 | 应与同期 `stream 5s:` 的 `committed` 相当（@30fps 约 150）。**稳定只有一半且 `fail`/`slot_fail` 全为 0** 时不是"取不到"，而是元数据环把帧静默扫掉了，见 [common/COMMON.md](../../common/COMMON.md) 「预览元数据环 seq 契约」 |
| `shm_wait_avg_us` / `shm_wait_max_us` | Camera `commit_slot` 到解码线程 `acquire_latest_frame` 取到之间的空等（两侧同用 `CLOCK_MONOTONIC`，可跨进程相减） | ≤ `kNoFrameNapMs`(5000)；明显更大说明解码线程被自己的工作拖住了 |
| `cvt_avg_us` / `cvt_max_us` | 单帧 NV12→BGRX 耗时 | RGA 路径 3000~8000 |
| `show_avg_us` / `show_max_us` / `show_n` | 解码线程发布 `display_buf_` 到 `ui_timer_cb` 真正 `lv_image_set_src` 的间隔 | ≤ `ui_refresh_period_ms_`(16000)；`show_n` 应接近 `ok` |
| `loop_avg_us` / `loop_max_us` | 解码线程整圈耗时 | 与相机帧周期同量级 |

### 主线程性能探针（`ui_perf_probe`）

`show_n` 明显小于 `ok` 时，瓶颈在上屏这一步，`decode 5s:` 就查不下去了——它只看得到"发布了多少帧"，看不到主线程为什么来不及取。`ui_perf_probe`（`lib/core/{inc,src}/ui_perf_probe.*`）补上这一段，同样输出到 `topic_ui_preview.log`，`UPC_MOD_CAMERA_PREVIEW` 关掉时整体编成空函数。

**关键前提**：LVGL 一次 `lv_timer_handler()` 里每个到期定时器**最多只跑一次**，不管已经过了几个周期。所以 `ui_timer_cb` 的真实触发周期 = 主循环周期，`lv_timer_create(..., 16ms)` 里的 16 只是下限。`show_n` 偏低要先怀疑主循环被拖长，而不是定时器周期配错。

`uiloop 5s:` —— `main.cpp` 主循环一圈的四段，加起来就是一圈：

| 字段 | 含义 | 怎么读 |
|------|------|--------|
| `loops` | 5s 内主循环圈数 | 就是 `ui_timer_cb` 的真实频率上限；应与 `show_n` 同量级 |
| `ipcwait_avg_us` / `_max_us` | `port.process(8)` | ≤ 8000（自身超时上限），是一圈的下限 |
| `lv_avg_us` / `_max_us` | `lv_timer_handler()` | 与 `refr 5s:` 的 `refr_avg_us` 对照：接近说明是屏幕刷新，明显更大说明是其它定时器 / indev |
| `ipc_avg_us` / `_max_us` | `port.process(0)`（非阻塞补发） | 应接近 0 |
| `mon_avg_us` / `_max_us` | `upc_sys_monitor_tick()` | 应接近 0（读 `/proc` 已改为只在上报点做） |

`refr 5s:` —— 用 LVGL 的显示事件把一次刷新拆成**可加的四段**（`refr = pre + draw + wait + flush`）：

| 字段 | 对应区间 | 含义 |
|------|----------|------|
| `n` / `idle` | — | 有效刷新次数 / 定时器跑了但无失效区可画的次数 |
| `refr_*` | `REFR_START`→`REFR_READY` | 整次刷新，即 `lv_timer_handler` 里的大头 |
| `pre_*` | `refr` − `render` | 布局更新 + **双缓冲同步区拷贝**（DIRECT 双缓冲下 `refr_sync_areas` 要把上一帧失效区从在屏缓冲拷回离屏缓冲，整屏预览时是一次全屏 memcpy） |
| `draw_*` | `render` − `wait` − `flush` | 纯绘制 |
| `wait_*` / `waits` | `FLUSH_WAIT_START`→`FINISH` 累计 | 等 DRM 翻页（`drm_flush_wait` 里的 `poll(-1)`）。**`waits ÷ n` 是一次刷新等了几回翻页**；明显大于 1 说明失效区没合并，每帧被多个 vblank 串行拖住 |
| `flush_*` / `flushes` | `FLUSH_START`→`FINISH` 累计 | `flush_cb` 本身（`drmModeAtomicCommit`），应很小 |

排查顺序：先看 `loops` 确认主循环频率 → 用 `lv_avg_us` 判断是不是 LVGL → 再在 `pre`/`draw`/`wait` 里定位。三者各有对策：`pre` 大是双缓冲同步拷贝，`draw` 大是绘制面积或逐像素变换，`wait` 大是翻页/失效区合并。

> **`draw_*` 偏大先查缩放，不要查旋转。** `rongsu_ui.defaults` 开了 `LV_USE_OPENGLES 1`，
> 于是 `LV_LINUX_DRM_USE_EGL` 被自动置 1，`lv_linux_drm.c` 整个文件被
> `#if LV_USE_LINUX_DRM && !LV_LINUX_DRM_USE_EGL` 排掉，真正编进去的是
> `lv_linux_drm_egl.c`。那边 `set_viewport()` 按 `lv_display_get_rotation()` 交换宽高、
> 由 GPU 在贴纹理时完成旋转，所以 `2.0` 平台的 `LV_DISPLAY_ROTATION_270` 几乎不花 CPU，
> 把它改成 `_0` 做 A/B 只会让画面躺倒、`refr_avg_us` 不动。
>
> 同时 `LV_USE_DRAW_OPENGLES` 默认是 0，**绘制仍然是软件的**，GPU 只负责把整屏结果
> 当纹理贴上去并旋转（那部分计入 `flush_*`，整屏 4MB 上传 + `eglSwapBuffers` 约 12ms）。
> 所以 `draw_*` 就是纯 CPU 像素工作，整屏预览下的头号嫌疑是 `lv_image` 的非 1.000
> 缩放（走 `lv_draw_sw_transform` 双线性），见「摄像头整屏预览」与「预览输出几何」。

## 打印历史 + 延时摄影播放器

| 组件 | 文件 | 职责 |
|------|------|------|
| `page_user_history` | `pages/page_user_history.*` | 读 `/userdata/media/video/config.json`（`common/fs_manifest/print_history_manifest`）→ 按 `start_unix` 倒序、按自然日分组的卡片列表；卡片三指标 Duration / Filament / Layers 分别取 `duration_sec` / `material_g` / `layer_count`（后两者由 Media 从 `/userdata/Gcode/config.json` 快照，为 0 时显示 "--"）；缩略图 `A:/userdata/media/video/{thumb}`（RTHM 解码，`LV_IMAGE_ALIGN_COVER`）；**Album** 走 `ui::album::scan()` 扫 `/userdata/media/photo`，缩略图优先同名 `.rthm`；顶部三 Tab + Storage 容量条；管理（多选）模式批量删除；上下翻页；**左滑删除**（跟手拖动，见「左滑删除：跟手拖动」一节）→ History/Timelapse **按类型独立删除**后重写 config.json；Album 删 PNG + 配对 `.rthm` |
| `page_user_timelapse` | `pages/page_user_timelapse.*` | 全屏播放页（建于 `lv_layer_top()`）：受 `UPC_TIMELAPSE_PLAYBACK_PNG` 控制——默认 PNG 序列；`=0` 时优先 `timelapse.mp4`（失败回退 PNG）；拖动预览为后台小图 scrub，松手后再 settle 主画面 |
| `page_user_photo` | `pages/page_user_photo.*` | 相册全屏浏览器（同建于 `lv_layer_top()`，与播放器同一套 chrome）：主图 `LV_IMAGE_ALIGN_CONTAIN`，左右滑动 / 两侧箭头翻页、首末循环回绕，底部 filmstrip 点格跳转，单击主图切控件层 |

- **进入/退出**：历史页缩略图点击 → `page_user_timelapse_set_source(目录,mp4路径,标题,时间)` + `switch_to("user_timelapse")`；Album 照片 → `page_user_photo_set_source(照片绝对路径)` + `switch_to("user_photo")`；两者关闭均 → `switch_to("user_history")`。
- **LVGL 线程规则**：PNG 序列播放由主线程 `lv_timer` 换主画面；拖动预览解码在后台线程，经 `lv_async_call` 贴预览框。
- **进度条底部那条 `pad_bottom` 别删**：knob 直径是 `3 × USER_PROGRESS_H`、轨道高只有
  `2 ×`，knob 上下各溢出 `H/2`；而 `bottom` 容器是 `LV_SIZE_CONTENT`，高度只按子对象的
  coords 算（knob 是 slider 的 style part，靠 `ext_draw_size` 画到框外，容器不认），
  LVGL 又默认把子对象裁到父容器盒子里。结果本该是正圆的 knob 被削成上下平的横向胶囊
  （实机 1024 档：36×36 渲染成 36×24，恰好丢掉 1/3）。补 `knob_overflow + USER_PAD_SM`
  的下边距把它圈回来，上方由命中区那 48px `pad_top` 顺带兜住。
- **PNG 序列停在末帧时重点播放要先回 0 帧**：否则 `play_timer_cb` 下一拍就撞上结束判定
  又把 `g_playing` 关掉，表现为点了没反应。MP4 分支到末尾先 `seek(0)` 是同一回事。

### 相册全屏浏览器（`user_photo`）

- **为什么不能直接 `lv_image_set_src(原图)`**：照片是 `1080×1280` PNG（`camera_resolution_profile.hpp`），`LV_COLOR_DEPTH=32` 下 lodepng 会在 LVGL 线程内同步解出约 5.5MB ARGB8888。故大图交给 `ui::PhotoDecoder` 后台解，缩到「CONTAIN 贴合屏幕」的尺寸出 RGB565，绘制变成 1:1 blit。
  **注意图片缓存已开启但不改变这个结论**：`rongsu_ui.defaults` 现在给了 `LV_CACHE_DEF_SIZE (8 * 1024 * 1024)`（此前未覆盖、沿用模板默认 0，即每次重绘都重解一遍）。8MB 缓存能兜住成片的 `.rthm` 缩略图与延时序列帧，但**单张全尺寸照片解出来就约 5.5MB**，一张就几乎占满、两张必然互相顶掉，且**首次解码依然发生在调用线程**。所以「后台解 + 缩到屏幕尺寸」仍是这里的正解，缓存只是顺带减少了缩略图侧的重复解码。
- **画面分两拍上**：先贴配对 `.rthm` 缩略图顶住（自定义 decoder 只是读文件 + memcpy，几乎零成本），后台解完再换清晰图；无 `.rthm` 时显示 `Loading...` 而不是在主线程抢着解原图。
- **解码目标尺寸只能运行时算**：主题分 1920/1280/1024 三档且按实际 display 最长边挑（`ui_theme.cpp` 的 `pick_profile()`），所以 box 取 `lv_display_get_*_resolution()`，不能写成编译期常量。竖构图照片贴合横屏后左右各留约 1/4 屏宽黑边，**两侧箭头正落在黑边上**，这是设计选择。
- **线程与生命周期**：`PhotoDecoder` 后台线程解码、latest-wins（世代号对不上就丢），经 `lv_async_call` 投递且合并成一次；与 `VideoPlayer` 的双缓冲翻页不同——相册每张照片几何可能不同，`dsc` 的 w/h/stride 必须由**主线程**在 `async_apply` 里发布（LVGL 是在绘制时刻读 dsc 的）。页面对象由 `PageManager` 惰性建出后**永不销毁**，故 `page_user_photo_exit()` 必须先 `lv_image_set_src(img, nullptr)` 再 `PhotoDecoder::release()`（顺序反了下一帧会读到已释放内存）。
- **滑动手势不用 `LV_EVENT_GESTURE`**：与历史页左滑删除同一套理由（见「左滑删除：跟手拖动」），手写 `PRESSED/PRESSING/RELEASED/PRESS_LOST` 四件套 + 起拖阈值 + 方向锁定，并靠 `just_dragged` 吃掉松手时那一下 `CLICKED`，否则每滑一次会顺带闪一下控件层。
- **翻页判据是「距离 or 轻扫」两条**，缺一不可：只看距离（屏宽 1/5）的话快速短扫会被判成没动、手感发滞；只看速度的话慢慢拖过半屏反而翻不了页。轻扫阈值 600px/s 且至少 24px 位移（挡住原地手抖）。速度取按下到松手的**平均值**而非瞬时值——轻扫本就是一气呵成的短手势，两者接近，做瞬时速度得留位置历史，不值当。
- **两处手感细节**：跟手位移要**扣掉起拖阈值**（`shift = dx ∓ kSwipeStartPx`），否则判定成立那一帧画面会突然跳 12px；翻页与弹回都走 `lv_anim` ease-out（`kSlideMs`）而不是瞬间归位。因为只有一个 `lv_image`，做不出「旧图滑出 + 新图滑入」的双图轮播，退一步只做**滑入**，方向跟手——观感已经连贯，且不必多养一份解码缓冲。跟手拖动前要 `stop_shift_anim()` 掐掉上一次的滑入动画，离页同理。
- **非 PNG 的兜底**：后台解码只吃 PNG（libpng）。`ui::album::is_photo_file()` 还收 `.jpg/.jpeg/.bmp/.webp`，前三者由 `PhotoDecoder` 回一个空 image、页面转交 LVGL 内置 TJPGD / BMP 同步解；`.webp` 当前无解码器，先用 `lv_image_decoder_get_info()` 问一遍，认不出来就显示 `Cannot display this photo`，不留黑屏。
- **列表与浏览器必须同序**：枚举/排序/配对 `.rthm` 统一在 `ui::album::scan()`（`core/album_index.*`），历史页 `rebuild_list()` 与本页 `refresh()` 都调它。两处各自扫目录的话，「下一张」会和列表看到的顺序错位。落点照片若在此期间被删（`index_of` 返回 `kInvalidIndex`），退到最新一张。
- **共用 chrome**：半透明黑底圆形浮层按钮抽成 `ui_user_make_overlay_round_btn()`（`components/ui_user_widgets.*`），播放器与浏览器共用；原先只存在于播放器的匿名 namespace 里。尺寸不足 `UI_USER_MIN_TOUCH` 时该函数自动外扩命中区，见下面「触摸命中区」一节。
- **filmstrip（底部横向缩略图条）**：
  - **格子固定数量、只换绑定**：格数按屏宽算（`(屏宽×4/5) / (格宽+间距)`，强制奇数好让当前格居中，夹在 3..15），`create()` 里建一次，之后 `update_strip()` 只改各格绑定的照片下标——绑定没变就不重读。格几何无设计稿，沿用历史页缩略图的 8:5、高取卡高的 2/5。
  - **`.rthm` 像素常驻内存，不给 lv_image 指文件路径**：缓存关闭下指路径会让每帧重绘都重新「读 136KB + `lv_draw_buf_create`」，一排格子同时重绘就是几十 MB/s 的无谓 IO。读进内存后滚动零 IO，只剩 LVGL 那点缩放（输出才几千像素，可忽略）。常驻量 = 格数 × 约 136KB，有界。
  - **`std::vector<std::unique_ptr<StripCell>>` 不是随手写的**：`lv_image` 持有 `&dsc` 裸地址，vector 扩容搬家会让它悬空，所以格子地址必须稳定。
  - **窗口在两端夹住而不回绕**（尽管翻页本身回绕）：在列表头把末尾几张拼到左边会看着像跳接，而此刻用户并不知道自己已经在头上。照片数少于格数时多余格子 `HIDDEN`，flex 自动跳过、整条变短。
  - **不做用户横向滚动**：窗口始终跟着当前索引走，滑主图时整条自动平移。这样既不必虚拟化一个可滚容器，也没有「条的横向滚动」与「主图横向滑动」的轴向之争。
  - **格子自己消费点击**：格子 `CLICKABLE` + 自带回调，格内图片加 `EVENT_BUBBLE` 让点击落到格子上（注意别照抄历史页缩略图那句 bubble 的用意——那里是要冒到卡片）。条本身与 overlay 都不可点，缝隙处的点击穿透到主图去切控件层。主图是 overlay 的**兄弟**而非父级，所以格子的点击不可能误伤主图。
  - 只有一张（或没有）照片时整条藏掉；离页 `release_strip()` 把各格像素还掉。

### 设置页布局（浅色稿 6089 一族）

对应稿：网络 `6089:11322`、显示与亮度 `6235:91450`、熄屏选择 `6240:92007`、
存储管理 `6251:94362`、格式化弹窗 `6283:26740`。全页文案英文。

**这一版是从旧深色稿 `3238:14296` 整体改过来的，随稿去掉的东西别照旧规律找**：
右侧上下翻页钮、`Device & Account` 那一项、**顶部常驻设备信息条**——**新稿都没画**，
不是漏做。翻页钮删掉后 `USER_SCROLL_BTN_SZ` / `_GAP` / `USER_SETTINGS_GAP` /
`USER_SETTINGS_GUTTER_R` 四个 theme 字段全工程无人再用，已一并清掉。

**设备信息条必须排除，不是可选的美化**：左栏 8 项的高度预算
`48 + 8×80 + 7×16 = 800` **正好等于屏高**，稿上末项 Debug 落在 y 720..800。
设备条会给 `content_panel_` 加 `USER_CONTENT_TOP`（112）的顶内距，整页下推之后
Debug 会整个掉出屏外。排除后不用补内距——左栏 48、右栏 72 是页面自己给的，
本来就从内容区顶端量起（这点与历史页不同，那页的顶内距是随排除一起补的）。
代价同历史页/消息页：本页看不到设备名与联机状态。

**两栏几何**（1280 档，三张帧互相印证过）。与旧版最大的不同是
**内距全落在各自栏里、`body` 零内距**，不再靠 body 的左内距/列间距/右留白凑：

```
左栏渐变带 364 = 32 + 左栏 300 + 32     ← build_subnav，USER_SETTINGS_NAV_W=300
右栏       788 = 64 + 内容 660  + 64     ← g_detail，内容起点 (556, 72)
                                 合计 1152（侧栏 128 之外的内容区）
```

左栏那条底带是稿 `6240:92194` 的**竖向渐变** `#FAFAFA → #F4F4F4(50%) → #FAFAFA`。
LVGL 的 `bg_grad` 只有两个色标、做不出中间那档，故拆成上下两半各一段渐变，接缝落在
中点、正好与稿的 50% 色标重合（`user_light_rail_mid` 只此一处在用）。

**右栏版式**：分区之间 48、分区内标题↔卡片 16；分区标题 22（`user_font_dlg_btn()`）；
行卡片 = row / 内距 24 / 白底 + 1px 白 10% 描边 + `0 2 6 rgba(0,0,0,.04)` + 圆角 16，
行内文字 20（`user_font_title()`）。卡片外观集中在 `apply_card_style()`，右栏所有行卡
共用一份，与历史页/消息页卡片同款。

> **右内距别在子页里改**。旧版 `build_network_detail()` 把 `pad_right` 加宽到 79 给
> 悬浮翻页钮让位，`net_reset()` / `show_detail()` 又各自把它清零——钮删掉后这三处
> 全成了错的，会把整页横向挤偏 15px。现在内距只由 `page_user_settings_create()`
> 设一次，别的地方一律不碰。

**导航**（8 项，顺序即稿）：Account / Network & Connectivity / Display & Brightness /
Storage / Advanced Print / Privacy & Security / About / Debug。

- 项高 `USER_NAV_ITEM_H` = 80（稿 24 + 32 图标 + 24），项间距 16，首项顶 y=48；
- **未选中无底、圆角 8；选中才是白卡**（圆角 16 + 白 10% 描边 + 那道投影）。两态圆角
  不同，所以不能只切 `bg_opa`；
- **只有选中项出右箭头**（旧版恒出）。Debug 是例外：它跳开发者页、稿上给的是外链
  箭头 ↗，LVGL 符号字库没有斜向箭头，退而用同向的 ›；
- 选中与否的字重差（SemiBold / Regular）Montserrat 追不了，只落地不透明度差
  （50% → 100%，图标同步）；
- `Device & Account` 去掉了，它装的 Galvo OTA / MCU OTA 两行**并进了 About**——那是
  固件升级、与账号无关，而 About 上面几行正是版本号；
- Account / Advanced Print / Privacy 走 `build_placeholder_detail()`。

**图标**：8 枚全部重出，`tools/gen_settings_icons.py` 从稿的整帧 SVG
`tools/pages/setting/new/image1` **按坐标裁窗**取出，路径数据原样不动（同
`gen_home_metric_icons.py` 里 `hist_*` 那批的做法）。旧版是共用一个 viewBox、
墨迹只占图标盒 50%，新稿是 75%，裁窗一换就对了。

**各分区的特殊处**：

- **Network**：Wi-Fi 开关分段 **256 宽**（稿 `6101:14231`，**不跟其余卡片一样满 660**），
  高 `USER_SETTINGS_SEG_H`=72、内距 2、段间 4、圆角 16，选中段是白色实块、圆角 14
  （= 16 − 2，两个圆角才同心）。宽度那 256 是 `USER_NAV_ITEM_H*16/5` 算出来的，
  行高 80 之后正好，不用另给。
- **Display & Brightness**：稿上滑条左右各一枚小/大太阳图标盒，工程没有那两个矢量
  资源，用同尺寸的百分比标签顶上。滑条填充段是深色 `user_light_text`，**不是主色蓝**。
- **熄屏选择子页**（`disp_show_sleep()`，照 `net_show_*` 那套状态机）：7 个选项在
  **一整张卡**里、行间以分隔线断开（不是 7 张卡），选中项右侧蓝勾，**右下浮动返回钮**。
  浮动返回钮**只有这一页用**，其余子页仍是顶部返回条（`make_subpage_header`），
  两种并存是稿上就有的，别顺手统一。分隔线要压成浅色档的 `user_light_divider`，
  `ui_user_make_divider_line()` 默认给的是深色档那个、压在白卡上太重。
  选项**纯 UI 占位**：只存静态变量，不落盘、也不真的熄屏。
- **Storage**：两张容量卡（并排、各 `flex_grow`，内距 20 / 行距 12）+ 格式化两行
  （红描边白底钮；U 盘那行按 `register_usb_hotplug` 置灰）+ 文件管理两行（只画不接）。
  确认弹窗复用历史页那个 `light_theme` 的 496×231。**三个卷不是同一批**：概览是
  Internal + SdCard，格式化是 Internal + UsbDisk。后端只实现了 Internal 的容量查询，
  其余见 [doc/STORAGE_BACKEND_API.md](../../doc/STORAGE_BACKEND_API.md)。

### 打印历史页布局（浅色稿 6122 一族）

对应稿：Timelapse 网格 `6122:79998`、Video 网格 `6122:79784`、Photo 网格 `6122:80163`、
三个空态 `6122:80392`/`80601`/`80532`、多选态 `6073:73736`。全页文案统一英文
（CJK 字库只有调试页那点子集，稿上底部那句「已选择2项」落地为 `N selected`）。

**这一版是从旧深色稿（`4366:5400` 一套六态）整体改过来的，随稿去掉的东西别照旧规律找**：
顶部 Storage 容量条与存储告警横幅、左滑删除、右侧上下翻页钮、`Manage` 按钮、空态那颗
`Start Print`、**以及常驻设备信息条**——**新稿一个都没画**，全部去掉，不是漏做。
（`Select all` 是个例外：第一版浅色稿没画、跟着删了，后来 6251:94118 又补了回来。）

- **本页不显示常驻设备信息条**（消息页与设置页后来也照此办理，三页都在
  `apply_device_bar()` 的排除名单里）。排除后 `content_panel_` 的 `pad_top` 收回 0、
  整屏高度还给页面；Tab 行到屏顶那段留白改由页面自己的 `pad_top` 给（同取
  `chrome_pad_l()`，稿上左右上三边都是 48）。
  **这两件事得一起做**：只排除不补内距，Tab 行会贴死在屏幕顶沿。
- **顶部 Tab 行**（高 `USER_HIST_TAB_H`，稿 60）：`Timelapse` / `Video` / `Photo`，左缘
  `chrome_pad_l()` = 48（稿上文字 x=176 减侧栏 128），项间距同为 48。字号 `user_font_body()`
  （稿 24）**在建的时候就传进 `ui_user_make_tab_item()`**，不能建完再改——项宽是照字号量
  完写死的，原因与踩坑记录见「Tab 项」一节。选中与否**没有下划线**——稿上只靠字重
  （SemiBold / Medium）与不透明度区分，而字库只有一个字重，落地成「满值 / 50% 不透明」，
  见 `apply_tab_style()`。共享构造仍会建出下划线对象（消息页要用），本页一律 `HIDDEN`。
  行下**不再有通栏分隔线**，Tab 与网格之间只留白。
  右侧仅多选态出 `Select all` + `Cancel` 两枚（稿 6251:94118），都是主色文字、无底无框，
  `Cancel` 右缘与内容区右缘齐。两枚走 `make_tab_text_btn()`，**零内距 + `ext_click_area`
  外扩**撑命中区（同 `ui_user_make_tab_item` 的处理）——加内距的话会叠在行的 `pad_column`
  上，两枚之间的视觉间距从稿的 44 涨到 80。

  `Select all` 是**切换钮**：全勾上之后文案翻成 `Deselect all`，再点一次全不选。稿上没画
  全选后那一态，措辞是产品定的。文案与底条的 `All N selected` **共用同一个判据**
  （`refresh_sel_count()` 里的 `all_on`），免得两处说法打架。`Deselect all` 比 `Select all`
  宽，三档核过都还剩 300px 以上，挤不到 Tab。
- **三列网格**：`g_grid` 是 `LV_FLEX_FLOW_ROW_WRAP` 的纵向滚动容器，左右内距同为 48，
  列距与行距同为 `USER_HIST_GRID_GAP`（稿 32）——**但只有列距是 `pad_column`，行距走的是
  各元素的下边距**，见下面「纵向间距」那条。

  **卡宽现算、不写死**：`card_w_of()` = `(网格内容宽 − 2×gap) / 3`。稿上 1280 档内容区
  176..1232 共 1056，算得 330.67；直接写稿标注的 331 会让 `3×331 + 2×32 = 1057` 比 1056
  多 1px，最后一列被挤到下一行。因为要现算，`rebuild_list()` 清空之后**必须先
  `lv_obj_update_layout(g_grid)`**，否则首次进页时网格还是未决的 `LV_PCT(100)` 宽，
  会按 0 算出 1px 的卡。

  选 `ROW_WRAP` 而不是 LVGL 的 grid 布局：条目数不定、中间还要插占满整行的月标题，
  grid 得先算行数、每插一条标题都要重排，wrap 天然按内容流。
- **卡片两种形态**，由 `build_card_shell(parent, ctx, with_info)` 的 `with_info` 分叉：

  | Tab | 形态 | 卡高 |
  |---|---|---|
  | Timelapse / Video | 缩略图 + 中央播放钮 + 白色信息条 | 缩略图高 + `card_info_h()` |
  | Photo | **纯缩略图**，左上角压一枚 `HH:MM:SS` 时间戳，没有信息条 | 缩略图高 |

  所以 Photo 那一档的行明显比另两个矮——**是稿上就有的差别，不是没对齐**。

  缩略图高 = 卡宽 × 186/331（稿 331×186.67，约 16:9），`LV_IMAGE_ALIGN_COVER` 居中裁切。

  `card_info_h()` 不另开 token，取「**稿的比例** 80:186.67，跟着缩略图走」与「**两行字
  实际要的高度**」的较大值。两个都要是因为**字库行高比稿的行框矮**——1280 档
  Montserrat 18/16 的 `line_height` 是 21/18，而稿按 24 排，只按字算会比稿矮 9px、
  卡的比例跟着走形；反过来只按比例算，小档上缩略图一矮就装不下两行字。
  三档落地：1920 卡 520×416、1280 卡 330×264（稿 330.67×266.67）、1024 卡 272×217。
- **卡片样式**：白底 `user_light_card`、圆角 `USER_HIST_CARD_RADIUS`（稿 16）、
  `drop-shadow(0,2,3) #0C1C61 @4%`（落地 `shadow_width 3` / `ofs_y 2` / `user_light_shadow`
  / `opa 10`，255 的 4% ≈ 10）。**整张卡只在卡这一层开一次 `clip_corner`**，缩略图自己
  是直角——不这么做四角会露出方形的深色槽底，而每个缩略图各裁一次要多建一层掩膜。
- **信息条**（稿 `6122:80028`）：内距 16/12、两行间距 8。文件名 `user_font_text()`
  单行省略（`LONG_MODE_DOTS` 必须同时锁高度，见下方说明）；meta 行两端对齐，左边
  `May 26 • 22:03`（`format_card_stamp()`），右边
  **Timelapse 是帧数、Video 是时长**。

  > **分隔符是 U+2022（`•`）而不是稿上的 U+00B7（`·`）**。LVGL 预生成的 Montserrat 只覆盖
  > ASCII `0x20..0x7E` 加一张 62 个码点的稀疏表，那表里只有 `U+00B0`(°)、`U+2022`(•) 和一批
  > FontAwesome——**`U+00B7` 不在其中，上屏是豆腐块**（踩过一次，实机截图里是
  > `Sep 18□11:06`）。要用别的符号先照这个办法查一遍字库，别想当然；真要加字符得重生成
  > 字库。同理，文件名里若有中文也会是豆腐（Montserrat 不含 CJK），那条路得走
  > `user_font_cjk()`。
- **帧数是扫盘数出来的**：manifest 里没有帧数字段，`session_frame_count()` 在会话目录上
  **倍增探上界再二分**找最大的 `N.png`（帧从 1 连号，清粉段也在同一串里），约 2·log₂(N)
  次 `stat`。不用 `directory_iterator` 逐个数：一个会话动辄上千帧，而这函数是分块建列表
  时每张卡调一次的，列目录会把建列表的耗时拖上去。探不到（非 PNG 播放模式 / 帧已被删）
  返回 0，此时退回显示时长。
- **月分组**（稿 `6122:80020`）：`build_month_title()` 建的标题设 `LV_PCT(100)` 宽，在
  `ROW_WRAP` 里天然独占一行、把后面的卡挤到下一行，不用另建行容器。
  `append_list_build_chunk()` 里「上一条月份不同就插一条标题」两个分支同一套写法——
  **Album 早先是平铺无分组**，这一版补齐，用的是 `PhotoEntry` 新增的 `mtime_unix`。

- **网格的纵向间距全部由各元素自己的 `margin_bottom` 给，`pad_row` 置 0**。

  LVGL 的 flex 里这两者是**叠加**的、不是取其一：一行的高度算的是
  `margin_top + 对象高 + margin_bottom`（`lv_flex.c` 的 `lv_obj_get_height_with_margin`），
  行与行之间**再无条件加一道 `pad_row`**。两套一起用就会打架——第一版 `pad_row` 给 32、
  月标题又加 16 的下边距，稿上 16 的间距实际渲染成 48，月标题看着不像在领它下面那组卡。

  现在的分工：

  | 元素 | `margin_bottom` | 稿 |
  |---|---|---|
  | 卡片 | `USER_HIST_GRID_GAP`（32） | 行距 32 |
  | 月标题 | `USER_PAD_SM`（16） | 标题框底 164 → 首行卡顶 180 |
  | 网格 `pad_top` | `USER_HIST_GRID_GAP`（32） | 首个标题上方没有前一行，得由容器给 |

  月标题**不设 `margin_top`**：它上方那一份由前一行卡片的下边距给（稿量到 33，与卡行距
  同值）。落地后 1280 档是 标题框顶 140 / 首行卡顶 177 / 次行卡顶 473 / 后续标题上间距 32，
  稿是 142 / 180 / 476 / 33，差的 2~3px 来自字号——稿上月标题是 20，而 `user_font_text()`
  在 1280 档是 18（行高 21 对 22）。

  **别改回用负 `margin_bottom` 去抵消 `pad_row`**：那样也能对上，但 1920 档算下来行高只
  剩 1px（`mont24` 行高 27 减负边距 26），字库一改就会变成负数。
- **`PhotoEntry::mtime_unix`**：照片没有清单、文件名也不带时间，`::stat` 的 `st_mtime` 是
  唯一的时间来源（`album_index.cpp` 的 `file_mtime_unix()`）。不用
  `std::filesystem::last_write_time`：它返回的 `file_time_type` 在 C++17 里没有到 `time_t`
  的可移植转换。`scan()` 现在每个文件只读一次 mtime 并存进结构体——早先是在排序的比较器
  里现读，同一文件会被 `stat` 上 O(log n) 次，排完那个值还丢了。
- **空态**：`USER_EMPTY_RING_SZ` 圆底（稿 80，底 `#333145 @3%` + 同色 `@7%` 描边）+ 图标
  （按 `ring*2/5` = 稿的 32 导出）+ 标题 20 + 说明 16，整组在内容区居中，**没有按钮**。
  三个 Tab 只差图标与文案，故只建一次，切 Tab 由 `apply_empty_copy()` 换 `lv_image_set_src`
  与两行字：

  | Tab | 图标 | 标题 / 说明 |
  |---|---|---|
  | Timelapse | `ic_hist_clock_set` | `No Timelapse Videos` / `Your completed and interrupted prints will appear here.` |
  | Video | `ic_hist_film_set` | `No Videos` / 同上 |
  | Photo | `ic_hist_image_set` | `No Photos Yet` / `Photos captured during printing will be saved here.` |
- **多选由长按进入**（`LV_EVENT_LONG_PRESSED`，LVGL 默认 400ms），三个 Tab 都支持，
  `on_tab_click()` 里切 Tab 先 `set_manage_mode(false)` 自动退出。长按的那张直接勾上，
  省一次点。

  **长按之后 LVGL 照样会发 `CLICKED`**，不挡的话手一松就顺势进了播放器：
  `card_long_press_cb()` 置 `just_long_pressed`，由 `card_click_cb()` 吃掉紧随的那一次。
  这个标记**在下一次 `PRESSED` 清**（`card_press_cb()`）而不是只在 `CLICKED` 里清——
  长按后手指若滑出卡片，那次 `CLICKED` 根本不会来，标记会一直挂着，把下一次正经点击吃掉。
- **多选呈现**：卡片右上角（压在缩略图上，距上距右各 16）出 `USER_HIST_CHECK_SZ` 复选框
  （稿 24、圆角 8）；未选中是白描边空心方块，选中换成 `user_light_accent` 底 + `ic_hist_check`
  白勾，同时整张卡描一圈 `kSelRingW`（4px）的主色环。

  **那圈环走 `outline` 而不是 `border`，别改回去**。`LV_STYLE_BORDER_WIDTH` 带的是
  `LAYOUT_UPDATE`，而且 border 算进 `lv_obj_get_style_space_left()`——一勾选内容区左右
  各缩 2，定宽的缩略图与 `LV_PCT(100)` 的信息条跟着变，卡内排版当场位移（实机上就是
  这个症状）。`LV_STYLE_OUTLINE_WIDTH` 带的是 `EXT_DRAW_UPDATE`，只刷额外绘制区、
  不动布局，额外绘制区还由 LVGL 自己刷，不必手调 `lv_obj_refresh_ext_draw_size()`。
  `apply_card_selection()` 在 `outline_width` 0/`kSelRingW` 之间切，为 0 时不画、常态无开销。

  **环是 4px、其中 1px 压在卡内、3px 露在卡外**（`kSelRingW` / `kSelRingPad`），稿上
  则是 2px 整圈压在卡内（描边 rect 覆盖 538.7..540.7，卡从 538.7 起）。两处偏离都是
  实机逐像素量出来后定的，别照着稿改回去：

  - **2 → 4**：环在圆角那一段是细弧，斜穿像素网格、没有一个像素被完整覆盖，整条弧都是
    抗锯齿混合色。2px 时直边是满值 `#485DFF`(72,93,255)、四个角只有 (88,105,226)，淡
    一档且发毛。加粗后弧的横截面能盖满整像素，角上才吃得住色。
  - **往卡内压 1px**：角上原先有一圈**比环和照片都亮**的白边（亮度 114 > 环 105 > 照片
    63）。拆开是 `0.753×蓝 + 0.147×照片 + 0.100×纯白`，三通道都对得上——那 10% 的白来自
    **卡片自己的底色**：圆角处缩略图是被 `clip_corner` 以抗锯齿方式裁的，覆盖率不足 1 的
    那一圈就把底下的 `user_light_card` 露了出来。直边像素对齐、`clip_corner` 不介入，
    所以只有四个角有。
    `lv_draw_rect` 的绘制顺序是 shadow → bg → border → **outline**，环画在卡片白底
    **之后**、而子对象又画在环之后，所以把环往里压 1px，缩略图圆角的 AA 就改成与蓝色
    混合，白就没了。那圈白实测正好 1px 宽（半径 16.5..17.5），压 1px 足够。
    **不能整圈压进去**——那样会被缩略图完全盖掉，环就看不见了。

  按 `lv_draw_rect` 的公式复算：外框外扩 `W+P`=3、环半径 `R+W+P`，三档都与卡角同心，
  环覆盖卡缘 −1..+3。选中的卡视觉上大 1.8%（330×264 → 336×270），相邻两张选中卡净距
  32−3−3 = 26，不会碰。

  两个量都**跟分辨率无关、不进主题表**：抗锯齿的量化门槛是绝对像素数，三档同值。
  要严丝合缝跟稿只能另加一层浮层子对象画环（环的内侧就能直接与照片合成），代价是每张
  卡多一个对象——网格是把所有条目都建出来的（分块建只是摊开耗时、不是虚拟化），
  200 条就是 +200 个对象。

  复选框与空态那枚圆图标上的 `border_width` 是**静态**的（只切 `border_opa`），不参与
  这个问题。

  **复选框自己不收点击**，只把事件冒到卡上：整张卡都是勾选热区，若「点框」和「点卡」
  各切一次会互相抵消。
- **底部多选浮条**（`build_sel_bar()`）：稿上 x=160..1248（侧栏右缘 128 再让 32，右侧同量）、
  高 `USER_HIST_SELBAR_H`（稿 80）、圆角 `USER_LIGHT_RADIUS`、白底；左侧计数，
  右一颗 `USER_HIST_DEL_BTN_W × _H`（稿 140×64）的 `user_color_status_err`（`#FF4242`）
  `Delete`，未选中时置灰不可点。

  计数**分两套文案**：全部可勾选项都选中时是 `All N selected`（稿 6240:91813），否则
  `N selected`。「全选」的判据是 `selected_count() == selectable_count()` 且后者非零——
  不加非零判断的话，空列表会显示 `All 0 selected`。

  条宽**现算**而不是 `LV_PCT(100)`：`FLOATING` 不吃 flex 布局，百分比只会撑满父宽、让不出
  两侧那 32。网格的 `pad_bottom` 另留了 `SELBAR_H + PAD_LG*2`，多选态下最后一行不会被压住。
- **删除确认弹窗开了浅色档**（稿 6251:94239）：走共享的 `ui_modal_dialog`，但
  `cfg.light_theme = true`——**全工程十五个弹窗里目前只有这一个开**，别顺手全开，另外
  十四个装着白字读数、深色输入框与键盘、进度条，底一翻白那些内容会当场看不见。

  开关只管**外壳**（白卡 `user_light_card` / 深色标题 / 圆角 `USER_LIGHT_RADIUS` / 内距
  `USER_PAD` / 遮罩 `LV_OPA_50`，并收掉深色档那圈描边与重投影）。body 与 footer 里的内容
  得调用方自己上色：

  | 部位 | 怎么给 |
  |---|---|
  | 标题 | 由 `light_theme` 管，`user_light_text` |
  | body 文字 | 调用方改色**与字号**（`ui_user_make_body_label` 有 13 个调用点，不为一处加档位参数） |
  | 两枚页脚钮 | `ui_user_make_confirm_footer_btn()` 的 `light` 形参 |

  body 那行稿上是 Inter Regular 20 / `rgba(51,49,69,0.5)`。**颜色不是实色灰**：用
  `user_light_text` 配 `LV_OPA_50` 的文字不透明度压出来，白底上正好混成稿的
  `(153,152,162)`；先前填实色 `#9e9e9e` 量到 `(158,158,158)`，偏亮且丢了那点偏紫。
  字号走 `user_font_title()`（24/20/16），1280 档正是稿的 20。

  页脚钮的 `light` **必须与弹窗的档位一致**：非填充档的底色在两档里是反的（深色档白 4%、
  浅色档 `#333145` 10%），给错那枚副按钮会整个消失。浅色档同时把高与圆角抬到稿的 72 / 12
  （`USER_DLG_FOOTER_BTN_H * 9/8` 与 `USER_LIGHT_RADIUS / 2`，三档 108/72/57 与 18/12/9），
  文字也从 `user_font_text()` 换成**单开的** `user_font_dlg_btn()`（32 / 22 / 18，稿
  22@1280 折算 33/22/17.6 取最近可用字号）——22 号是为它才在 `rongsu_ui.defaults` 里
  打开 `LV_FONT_MONTSERRAT_22` 的。起初借的是 `user_font_title()`，1280 档落在 20、
  比稿小一档，实机量到按钮文字窄约 9%。
  卡宽不必分档：`USER_DLG_W` 的 1280 档就是稿值 496；两枚按钮 `flex_grow` 等分后各
  212，与稿一致。

  **字重两处都追不平**：稿上标题是 PingFang SC Medium、两枚按钮文字是 Poppins Medium，
  而项目的 Montserrat 只有一个字重（同 Tab 行选中态的处境，那边是用不透明度近似的）。
  所以只对字号，不对粗细——实机看上去会与稿略有出入，这是已知的、不是漏做。

  标题字号**单开了一档** `user_font_dlg_title()`（40 / 28 / 24，稿 28@1280 折算 42/28/22.4
  取最近可用字号），只服务开了 `light_theme` 的弹窗；深色那套仍走 `user_font_value()`。
  起初想沿用 value 省一档，实机量下来不行：user 系在 1280 档正好跳过 28（body 24、
  value 32，两边等距），用 32 时标题字身 cap 22 对稿的 19.5、大 13%，而卡片是
  `LV_SIZE_CONTENT`，标题一大整张卡就比稿高 9px、按钮跟着下移 9px。

  **纵向节奏另有两笔补偿**，`USER_DLG_TITLE_LEAD` / `USER_DLG_BODY_LEAD`（15/9/4 与
  9/2/1），只在 `light_theme` 下生效：

  - 间距本身落回稿：稿把标题与正文收在同一个 16 间距的组里，组与页脚之间才是 32。
    card 的 `pad_row` 只有一个值，所以取 `USER_PAD_SM`(16) 管「标题→正文」，
    「正文→页脚」那 32 由它再加上页脚自己的 `pad_top`（同样 16）凑出来。
  - 但光有间距还不够：稿的文字行盒比 LVGL 预生成 Montserrat 的 `line_height` 高一截
    （稿标题 PingFang SC 28 的 1.4 倍行距 = 39，`montserrat_28` 只有 30；稿正文
    Inter 20 的 1.21 倍 = 24，`montserrat_20` 只有 22）。差额不补的话卡片会比稿的 231
    矮 11px，而卡片是 `lv_obj_center()` 居中的，于是正文、页脚、卡片下缘整体上移
    5~6px——量过。两个 LEAD 就是这个差额，上下按稿的基线位置分（上 = `lead - lead/2`，
    下 = `lead/2`）。标题的加在标签的 `margin` 上，正文的加在 **body slot** 的 `pad` 上
    ——后者这样页面往 body 里塞什么都不用管这件事。

  补完后卡片正好 496×231、落在稿的 (392, 285)，两枚按钮各 212 宽、y 420..491——与稿
  逐像素一致（headless 渲一张 1280×800 和稿的导出图比对过）。**剩下的差只在字身本身**：
  Montserrat 的字宽与稿的 PingFang SC / Inter / Poppins 不同，标题与正文的横向长度比稿
  各宽 10~20px，这跟着字库走，不是布局没对上。
- **删除动作**走原有那套不变：`remove_cards_with_rollback()` 按当前 Tab **独立删除**（Timelapse 只清延时资产
  `*.png` / `thumb.rthm` / `timelapse.mp4`；Video 只清 `print_h264.mp4` / `print_thumb` /
  `print.h264`；Photo 删 PNG 原图及配对 `.rthm`），另一侧仍保留；两侧都空才移除清单条目
  并删会话目录。`video_pending` / `print_video_pending` 的条目不可勾选，避免打断落盘。
- **分块建列表照旧保留**（`append_list_build_chunk()` / `list_build_timer_cb()` /
  `kListBuildChunkSize`）：长列表一次建完会卡帧。只是每块建的东西从「一行卡」变成
  「一张网格卡」。
- **`USER_HIST_*` 里有三个本页已不用、只剩别页在用的**，别顺手删（`ui_theme.hpp` 也记了）：

  | 字段 | 谁还在用 |
  |---|---|
  | `user_hist_card_h` | `page_user_photo.cpp` 的 `strip_cell_h()`（底部缩略图带格高取它的 2/5） |
  | `user_hist_btn_w` | 消息页管理态的按钮宽 |
  | `user_hist_list_pad_l` | 消息页的列表与日期标题左内距 |

  `USER_SCROLL_BTN_SZ` / `_GAP` 是反过来的例子：三页的翻页钮陆续按稿删光之后，
  两个字段全工程无人再用，已从 `ui_theme` 的结构体、宏与三档表里一并清掉。
- **`LONG_MODE_DOTS` 必须同时锁高度**：v9 的实现是「先按宽度换行、再截末行」，不
  `lv_obj_set_height(lbl, lv_font_get_line_height(...))` 就只会换行、出不来省略号。

**上板必核两项**（1024 档与手势，台架上看不出来）：

1. 1024 档卡宽只有 272（缩略图 152、信息条 65），三列会不会太挤、文件名省略得是否太狠；
2. 长按与滚动手势会不会打架——列表滚动时按住不动容易误触发多选。LVGL 在滚动开始时会
   发 `PRESS_LOST` 并复位长按计时，理论上不会误触，实机要确认触屏的抖动没把这条绕过去。

**一处等后端补数据**（其余字段 manifest 已齐）：

| 缺口 | 现状 | 补法 |
|------|------|------|
| Timelapse 帧数 | `session_frame_count()` 扫盘二分数 `N.png` | manifest 落盘时顺手记一个帧数字段，UI 直接读 |

### 控制页（稿 5782:12911 / 5814:16936 / 5815:17390 / 5788:14284+14635 / 5788:15012）

**一个 Page 两个视图，靠显隐切换，不是两个 Page。** 主面板 ←「More Controls」→ 更多控制。

之所以不把「更多控制」做成隐藏页：`apply_device_bar()` 按「侧栏有菜单按钮」判显隐，
隐藏页会连常驻设备条一起隐掉并把 `pad_top` 收回 0——而稿 5814:16936 / 5815:17390 顶上
明明画着设备条。做成页内视图则设备条与侧栏天然照稿保留，`ui_app.cpp` 也不必多注册一个页。

代价是右上角的通知浮层会压住更多控制那条横跨全宽的传感器条（浮层 400 宽、退 32，
传感器条顶在 y=185），所以本页在 `show_view()` 里用 `ui_notice_stack_set_visible()` 自己
关开。**换页时不必还原**：`switch_to()` 的顺序是 `on_exit()` → `apply_device_bar()` →
`on_enter()`，紧随其后那句就按新页规则重设了；在 `on_exit()` 里写 `set_visible(true)` 只会
被覆盖，还会让人误以为本页要管别页的浮层。反过来，进页（`refresh()`）**必须**按当前视图
再设一次，因为 `apply_device_bar()` 刚把它按「五个标签页一律显示」重置过——同首页
`sync_notice_offset()` 的处理。

**三路数据一路都不是本页自己订阅的**，三个订阅点全是单槽的，抢了会静默弄坏别人：

| 数据 | 取法 | 单槽点被谁占着 |
|---|---|---|
| 环境四项（舱内/室内 PM2.5、舱温、氧浓度） | `ui_device_state_add_observer()` | `ui_device_state`（`register_env_update`） |
| 耗材 / 滤芯 | `control_supply_add_observer()` + `control_supply_request()` | `control.cpp`（`register_supply_status`） |
| 传感器条 / 三轴位置 / 设备开关态 / 激光读数 | `DebugMsgProxy` 的 sensor/axis/device/laser | 调试页也在用**同一组** |

最后那组因此**进页注册、离页清空**（`page_user_control_exit()`），与
`page_user_debug_exit()` 对称。不清的话从本页切去调试页，那边的传感器条、三轴位置、
设备行、激光读数全都不再刷新。

**`lv_obj_create()` 的默认尺寸是固定的 130×130，不是 `LV_SIZE_CONTENT`。** 本页所有布局
容器都从 `make_box()` 起手，那里**必须**显式 `lv_obj_set_size(o, LV_SIZE_CONTENT,
LV_SIZE_CONTENT)`——`lv_obj_class` 的 `width_def/height_def` 都是 `LV_DPI_DEF`（=130，工程
未覆盖）。第一版漏了这句，实机上一次性炸出四个症状，值得记下来避免再踩：

- **大数值被裁掉下半截**：`head(130) → txt(130) → vrow(130)` 三层固定高，`txt` 装不下
  caption + vrow 就把 vrow 裁到约 110 可见；而 vrow 上有 `LV_FLEX_ALIGN_END`（让单位与
  数字底对齐），行容器的交叉轴是竖向，数字被推到 130 高的底部、正好落在裁剪线之外。
- **整列顶出屏幕**：卡高从应有的约 156 涨到 218，左列四张 872 > 内容区 688。
- **弹窗里 "INTERIOR PM2.5" 裁成 "INTERIOR PM"**：`nrow` 固定 130 宽装不下 165，图标也被挤变形。
- **命中区跟着错位**，点哪儿都可能落到别的控件上。

需要百分比宽高或 `flex_grow` 的调用点在 `make_box()` 之后各自覆盖对应那一维，不冲突。

**页面底与卡片底色是一对、不能只改一个。** 稿上底是 `#272628` + -49° 渐变、卡片是白 4%
（折成不透明约 `#302F31`，只比底高 9 级）。第一版跳过了背景层，页面透到
`user_color_bg`（`#0a0909`），白 4% 叠出来约 `#191818`——**实机两块屏上都读不出卡片轮廓**，
1024 那块几乎完全看不见。现在两件都做：

- 背景：页根整屏出血铺 `#272628` + 渐变，做法与理由见下一段；
- 卡片：不透明 `kCardBg` = `#3D3C3E`（等效白 10%，比底高 22 级）。**别改成
  `user_color_panel`(#1c1c1c)**——它比页面底更暗，卡片会变成凹陷的、与稿的「浮起」层次
  正好反过来；`user_color_panel_alt`(#262626) 与底几乎同色，等于没对比。历史页/消息页用
  `user_color_panel` 是因为它们的页面底是近黑的 `user_color_bg`，那个前提在本页不成立。

**背景整屏出血**：页根 `pos(-USER_SIDEBAR_W, -USER_CONTENT_TOP)` + `size(LV_HOR_RES,
LV_VER_RES)`，再用自己的 `pad_left`/`pad_top` 把内容推回内容区——同首页
`page_user_home_create()`。不出血的话设备条那条透明带会露出 `user_color_bg`、在
y=`USER_CONTENT_TOP` 处出现一道横向接缝。两个坑：尺寸**不能**用 `LV_PCT(100)`（那算的是
父内容区、会被 `pad_top` 扣掉一条）；纵向抵消的是 `pad_top`，跟 `USER_CONTENT_TOP`
而**不是** `USER_TOPBAR_H`（1280 档 112 对 128），用错整页竖直错开 16。渐变取**不透明
两停**（把稿的 `rgba(61,58,78,0.4)` 预先与 `#272628` 混成 `#302E37`）而不是带 alpha 的
叠加层：alpha 停会让那一段透到底下的 `user_color_bg`，就得再养一层实色底。描述符必须
static 持久，`bg_grad` 只存指针（同 `ui_slide_unlock.cpp` 的红条）。

**列高与卡高一律钉死，不让内容累加决定**（`col_h()` / `card_h_*()`）。稿上两列精确地从
y=125 铺到 y=729、**各 604 高**，靠的是每张卡自己的确定高度：

| | 稿@1280 | 备注 |
|---|---|---|
| 指标卡（条+胶囊） | 156 | 内距 32 + 卡头 67 + 12 + 条 8 + 12 + 胶囊 25，一像素不剩 |
| 温度+氧组合卡 | 200 | 稿里明写 `dimensions.height: 200` |
| Powder Clean / Laser Power | 100 | 带状态字/数值 |
| LIGHT / More Controls | 64 | 见下条，LIGHT **没有**状态字 |

左列 156+7+156+7+200+8+64 = 604，右列 156+7+156+7+100+7+100+7+64 = 604。两列都钉死
`col_h()` + `SPACE_BETWEEN`，几像素出入落进行距（稿上左列首张 162 比第二张高 6，是那枚胶囊
内距用了四周 8px 而非 4px 8px，这 6px 不单独建模）。

**为什么必须钉死、不能内容自适应**，两个理由都是实机踩出来的：

1. 两列卡种类不同（左 4 张、右 5 张），累加出来必然不等，底边对不齐；
2. 指标卡的状态胶囊在无数据时要隐掉，而 **flex 会跳过 `LV_OBJ_FLAG_HIDDEN` 子节点**——卡片
   会当场矮约 37px，等数据一到又长回去，**列高跟着运行时数据跳**。实机上表现为无环境数据
   时左列反而比右列短。现在 `apply_metric()` 无数据时改用**透明占位**而不是 HIDDEN，连卡内
   进度条的位置也不会挪。

**1024 档不能按 0.75 折算**：那样得 117/150/75/48，而按该档字体实测（value 24 行高 33、
caption_sm 12 行高 17、内距 11、图标 26）指标卡要 118、组合卡要 151，会裁掉 1~3px。故该档
按算术给值并各留几像素余量（120/152/78/48），`col_h` 取两列所需的较大者（右列 468）加 2 =
470；可用高度 600 − 84 − 8 = 508，仍余 38。

> **⚠ 待删（上机前）**：`page_user_control.cpp` 顶部的 `UPC_CONTROL_DEV_CLEAN_TIMEOUT`
> 宏 + 三处 `#if` 块 + 那条 `#warning`。它给 Powder Clean 加了 10s 自解锁，仅供联调期把
> 界面从忙态里捞出来；真实清粉一轮远超 10s，**开发期它会在清粉还在跑时把开关翻回 Off**，
> 那是预期行为不是 bug。编译时每次都会刷一条 `DEV: control page ...` 提醒。

几处对稿的取舍：

- **稿全中文，实现全英文**（Montserrat；CJK 子集只覆盖调试测试页）。术语一律沿用调试页
  已有的串：`Print Plane Height (Z)` / `Powder Scraper (S)` / `Powder Bucket Rod (D)`、
  `Click move` / `Long-press`、`Print Params` / `Layer (um)` / `Balance` / `Send`、
  `Fan` / `Duty`、Other 页 `Process` / `Sieve Motor` / `Inner Door` / `Gas Wash` / `Base Heat`、
  `Filter` / `Material`（后两个取自 `control.cpp` 的 supply bar，**耗材是 Material 不是
  Consumable**）。**稿上 S 轴标题写的是「打印平面高度（S轴）」，那是把 Z 卡标题复制过去
  忘改了**——S 轴实际是铺粉刮刀，按调试页取 `Powder Scraper (S)`，别照稿改回去。
- **`backdrop blur(25px)` 略去**：LVGL 画不了背景模糊，同首页那两枚玻璃钮的 inset 阴影。
  只留白 4% 底 + 白 10% 描边（稿写 0.5px，整数栅格上取 1）。
- **PM2.5 的红/绿门限不写死 50**：后端 `pm25_clean_threshold_` 默认 50 但可由 `cal.csv` 的
  Config 段 `PM2.5Clean` 覆盖，且开门前清粉与 `APP_PM25_Rsp.is_high` 都按那个值判。写死会让
  改过 cal.csv 的机器上「红胶囊说不能开门」和「实际能开门」对不上。条按门限的两倍做满量程，
  门限处正好半条，与稿上 73.6 占半条一致。
- **设备线稿插画（稿 333×672，主面板右侧）本轮未做**，右侧留空只保留背景。补的话照
  `gen_home_bg.py` 出位图，别指望 LVGL 图元拼出那种 line-art。
- **Laser Power / LIGHT 的卡头图标是本轮新描的** `ic_bolt_*` / `ic_bulb_*`（进了
  `gen_home_metric_icons.py`，出 48/32/26 三档）。Filter / Material / Powder Clean 三枚稿上
  的矢量未导出，按语义就近取 `ic_layers` / `ic_cube` / `ic_purify`。
- **LIGHT 与 Powder Clean 在稿里不是同一个模板**，别当成一个：Powder Clean
  （`#5815:18006`）有 28 号状态字「关闭」、卡高 100；**LIGHT（`#5782:13071`）只有 caption**
  （子节点 `#5782:13079` 里只有一个 `[TEXT] "LIGHT"`）、卡高 64。第一版给 LIGHT 也套了带
  状态字的模板，多出一行 32px，是两列底边对不齐的原因之一。`build_toggle_card()` 现在靠
  `state0 == nullptr` 区分两种，`ToggleCard::state` 因此**可能为空**，写它的五处都要判空。
- **稿未给弹窗的那五张卡**（EXTERIOR PM2.5 除外，它的箭头在稿里是 `opacity 0` 即不可点）
  箭头照画、点了弹「Not available yet」，等设计稿到了换弹窗内容。
**chrome 顶行在稿里是两层，别摊平**（`#5814:16955`，两个 Tab 完全相同）：

```
row gap 24
├── edit-btn(Back)  hug×40 · padding 6/16/6/8 · gap 3 · radius 8 · 无填充
│   ├── IMAGE-SVG 20×20 @ opacity 0.5
│   └── TEXT "返回"
└── 子组 layout_5ddafec8   row gap 16 · hug     ← 两枚 pill 归成一组
    ├── pill 120×40 radius 100
    └── pill 120×40 radius 100
```

第一版摊成一层三项等距（都 24），「Back」与「一组 Tab」读起来像三个并列按钮。稿上的绝对
坐标可作校验：Back 176..255（宽 79 = 左 8 + 图标 20 + 间距 3 + 「返回」32 + 右 16）、
pill1 279..399、pill2 415..535。落地后 Back 会比 79 略宽几像素，因为 `Back` 比「返回」宽，
按钮是 hug 宽度、往右顶开一点，无解也无碍。

三处容易想当然的细节：

- **Back 的内距是非对称的**（左 8 / 右 16）。图标在左、文字在右，右边多留一档才能让整组的
  视觉重心落在中间；左右都给 8 的话文字显得贴边。
- **选中 pill 的底色与字色不是同一个色**：字是 `#4B87FF`（`kTabBlue`），底是更亮更偏青的
  `#64BEFF` 压到 10%（`kTabFill`，SVG 导出里那行 `fill="#64BEFF" fill-opacity="0.1"`）。
  当成同一个色会让选中态的底偏暗偏紫。
- **Back 的箭头是 20×20 的 SVG 而不是文字符号**，走 `ic_chevron_left_set`（路径直接取自
  `tools/pages/control-3/image2` 里那段 `<path d="M197 151L191 145.001L197 139"
  stroke-width="2"/>`，平移到 20 画布即 `M13 16L7 10.001L13 4`）。原先用 `LV_SYMBOL_LEFT`
  按 16 号排，形状与粗细都对不上。
  > **同类缺口尚存**：本页另有 15 处 `LV_SYMBOL_*`（主面板 7 张卡的 `>`、轴卡与激光弹窗的
  > −/+/复位、设备控制那批 80 圆钮的电源/播放/左右），稿上**全是 SVG**
  > （`EL-89f6cb31` 20×20、`EL-333f352d` 48×48、`EL-b67e1850` 80×80）。要补的话按本条同一
  > 套路走生成脚本；**卡片那枚 `>` 影响最大**（主面板 7 张卡都带它）。

**更多控制的左右内距是非对称的**（`body_pad_l()` = 48 / `more_pad_r()` = 32）：稿上内容左缘
x=176（侧栏 128 + 48）、右缘 x=1248（距屏右 32）。第一版照左边抄成对称的 48，整块窄了 16。
修正后横向全部落回稿的精确值：内容宽 **1072**、每张轴卡 **344**、Device 右列 **752**。

**轴卡底部那三枚步进圆钮刻意放大到 72/60**（`axis_btn_sz()`，稿值 48）。48 在实机上偏难点、
1024 档折出的 40 还低于 `UI_USER_MIN_TOUCH`；倍率取工程既有的 1.5（同下面「触摸命中区」记的
`user_media_btn_h` 那次）。**不要顶到上限 80**：那就和 Device 页那批 80 圆钮一样大了——那批是
每张卡唯一的主操作，这三枚只是步进微调，同大会喧宾夺主，且 1280 档只剩 2px 余量。
余量校验：1280 卡 344−32=312 装 3×72+64=280（余 32）；1024 卡 293−22=271 装 3×60+72=252（余 19）。

**两个开关（LIGHT / Powder Clean）的轨道从稿的 32 提到 40，宽度仍是稿的 64。**

**只加高不加宽**是关键：难点的本来就是竖向那一维，而加宽会立刻撞上 Powder Clean 的横向死线
（见下面的引用块），得靠降状态字号或改文案去换，不值得。命中区那句
（`sw_track_h() < UI_USER_MIN_TOUCH` 时补差额）**保留**——40 仍小于 48，视觉 40 + 命中 48。

**40 是不动其他任何东西的上限。** 再往上 LIGHT 卡要高过 76，左列 604 里的行距会掉到 5 以下，
那时该放宽 `col_h` 而不是继续挤行距，但那会打破「两列都 604、底边落回稿的 y=729」这个已经
对上的结论。落地后左列内容 `156+156+200+72 = 584`、三道行距 ≈6.7（稿 7/7/8，差 0.3 看不出），
右列完全不受影响。

**`card_h_light()` 与 `card_h_entry()` 必须是两个 helper**（第一版共用一个 `card_h_slim()`）：
开关加高后 LIGHT 卡涨到 72，而 More Controls 入口卡按稿仍是 64。共用的话入口卡会一起被顶上去，
右列内容从 576 涨到 584、四道行距从 7 掉到 5，右列反被拖累。`card_h_light()` 写成
`max(sw_track_h(), USER_METRIC_ICON_SZ) + card_pad()*2` 而不是写死 72——**从开关高度反推**，
以后再调开关尺寸不会漏改卡高；取 `max` 是防开关将来又调得比图标矮时卡高塌掉。

> **Powder Clean 的横向没有余量**：卡内可用宽 240（272 − 16×2），那行是
> 图标 32 + 8 + 文字 + 8 + 开关 64 → 文字得 128，而状态字 `Cleaning` 在 `user_font_value()`(32)
> 下约 **127**，正好卡在极限。**以后加宽开关、加长状态文案、或改卡片内距，这里会第一个出问题。**
>
> 顺带两条：状态字用 32 而稿是 **28**，是字库只有 20/32 两档、没有 28 的取舍，选了更接近的那个；
> 圆钮从 40×28 变成 40×36（高度跟着轨道走），圆角是 `LV_RADIUS_CIRCLE`，所以看上去比稿更接近
> 正圆——横向还有余量（LIGHT 那行能给开关 160），嫌胖可以把 `sw_knob_w()` 也提一点。

**三张轴卡的正文字号刻意从稿的 16 上调到 18**（`axis_font()`）。**同样 16px，中文比英文视觉上
大将近一倍**——中文字形填满 16×16 的字身框，而拉丁小写的 x 高度只有约 0.52em ≈ 8px；稿上
「打印平面高度（Z轴）」很扎实，换成 `Print Plane Height (Z)` 就显得细小，实机确认过。所以这
不是抄错稿，是「照抄字号」在换语言之后本来就会失真。

上调幅度受 **Z 卡**限制（它比另两张多一行工作零点）：跟着长高的有 6 行，18 号每行约 +2.5、
合计 +15，而 Z 卡弹性空隙 24，余 9；**再上到 20 就要 +30、超 6px 会被裁**，那得同时收下半块
内距（24→16）或上半块行距（16→12）。范围只含上半块与子 Tab 那 8 个 label；下半块的
`Step Distance` 及其单位仍是 16——那是另一个视觉层级。

> **1024 档这一改实际不变**：该档 `user_font_text()` 与 `user_font_text_sm()` 同为 16。要那档
> 也变大只能动主题的字号表，属全局影响，不在本页范围内。
>
> **别拿这条去「对齐」历史页**：那一页的结论是回到稿的 16（`text_sm`，见「打印历史页布局」
> 的字号表），语境不同——它的卡片信息密度与可用宽度都与这里不是一回事。

**输入框里的数值是 20**（稿 `EL-e80747d3` / `style_d2900971`，轴卡步距、打印参数、风扇占空比
三处都是），原先误用了 16。不吃布局预算——框高由 `num_input_h()` 钉死。

**弹窗里的 ± 圆钮与那条填充轨道都是 64**（`dlg_btn_sz()`，稿 `EL-0fcb9837` / `#5788:15383`
都明写 64）。第一版误用了 `USER_BTN_PRIMARY_H`(52)——**那是主按钮高度、不是弹窗圆钮**，
同一行三个元素等高才是稿的样子。

**更多控制的 chrome 纵向节奏按稿给三个数**（`more_gap_chrome/strip/bot`）：稿上 chrome 行
y=125 高 40、传感器条 y=185 高 109、内容 y=318，而 Motion 的轴卡高 **434**
（`layout_4c04db04` 里显式写死）→ 底边距 800−318−434 = 48。原先两段间距都用统一的
`USER_PAD`(24)、底边距也是 24，于是条与内容各下移 4、内容区高出 20。**两段间距不等（20/24）**，
所以 `pad_row` 取前一段、后一段靠 body 的 `margin_top` 补差。

**稿自身在这两个 Tab 上不自洽**：Motion 的内容收在 752，Device 的却收在 779
（`#5815:17746` 在 y=555 高 224）。共享同一个内容区高只能取一个，取 Motion 的 434——它是稿里
显式的尺寸，Device 那边是由两块坐标反推的。1024 档的底边距同样**不能按 0.75 折算**（36 会让
内容区只剩 317，而该档最高的 Z 轴卡按字体实测约需 318），取 28 留约 7px 余量。

**设备控制两列的高度策略必须一致。** 右列的 `mrow`/`other` 都 `flex_grow` 恒定撑满，左列第一版
两张卡是 `LV_SIZE_CONTENT`、够不够全看字体推出来的高度凑不凑巧，实机上短了一截。现在
**Fan 卡 `flex_grow(1)` 吃掉左列剩余高度** + 卡内三行 `SPACE_BETWEEN`：底边与右列齐平（稿上
两列本来就齐，右列 318+217+20+224 收在 779、左列两张卡加起来也正好到 779），多出来的高度分给
行距、顺带把 Send 那行撑开。

- **风扇卡按稿没有 Send，实现里加了一枚**（节点 5815:17974 只有占空比行与开关行）。这是刻意
  偏离：`UI_Debug_Fan_Ntf`（开关，带 duty）与 `UI_Debug_Fan_Duty_Ntf`（仅设占空比）本是两条
  消息，只有开关那一下会带占空比，**没有独立 Send 的话风扇转起来之后改 Duty 完全无效**、
  得先关再开。调试页正因如此配了 Send（`on_fan_send_click`），两处对齐。Send 放在 Duty
  那一行的数值组里而不是像调试页那样与电源钮同排：语义上它发的是这行的值，且横向宽裕
  ——这张卡 1280 档只有 300 宽，开关那行已被状态块 + 80 的电源钮占去大半，1024 档更紧。
  高度取**和旁边输入框一样**（`num_input_h()`，1280 档 48）而不是 `make_outline_btn` 的稿值 32：
  32 低于 `UI_USER_MIN_TOUCH`(48)、实机上点不灵，而同一行两个控件等高看着才像有意为之。
  `make_outline_btn()` 另外对**所有**不足 48 的实例自动外扩命中区补足差额（视觉尺寸不变，
  故不影响还原度）——「Set Work Zero」按稿是 32 高，有同样的问题，一并兜住了。
- **激光功率弹窗的副标题在稿上是「当前PM值过高，请手动净化空气」**，那是从 PM2.5 弹窗复制
  忘改的，实现换成操作说明。底部那条按稿是**只有左圆角的填充块、不是带 knob 的 slider**，
  说明只能靠 ± 改值，故用定宽填充而非 `lv_slider`。档位词按区间给（<33 Low / 33~66 Medium
  / >66 High），下面小字显示百分比，± 步长 5%。
- **开关不用 `lv_switch`**：稿那颗钮是 40 宽的横向圆角矩形（比轨道高还宽），套 `lv_switch`
  要跟它按轨道高推出的方钮几何反复较劲。全工程原本也没有任何 `lv_switch` 用例。

两处交互上的坑：

- **点动不能只挂 `LV_EVENT_CLICKED`**：CLICKED 是松手才发的，长按模式下会退化成「按住不动、
  一松手才走一下又立刻停」。点击模式听 CLICKED，长按模式听 `PRESSED` 起步 + `RELEASED` 补停帧，
  且**必须连 `PRESS_LOST` 一起收**——手指按住时滑出钮外只发 PRESS_LOST，漏掉它轴会一直走，
  这是会撞机的那类漏洞。四个事件由 `attach_jog()` 一次挂全。
- **`Purify Now` 与 `Powder Clean` 走同一条 clean_test 路径**，不走 `send_clean_start()`：后者
  的回执由 `control.cpp` 的开门流程独占（单槽 + `invoke_done` 对空回调是 no-op），控制页发了
  只会得到静默。走 clean_test 既拿得到完成回执，两个控件也共享同一份忙态、不会各说一套。
  两者都走 `record_video=false`（`UI_CLEAN_OWNER_CONTROL`）：不开 Media 录像会话、不写
  `config.json`、不进打印历史。清粉期间开关**置灰不可取消**（中途打断会留下舱内 PM 偏高）。

### 触摸命中区（`lv_obj_set_ext_click_area`）

**视觉尺寸是设计稿的事，命中区是可用性的事，两者不必绑在一起。** LVGL 提供的
`lv_obj_set_ext_click_area(obj, n)` 把命中矩形四边各外扩 n，**不改布局也不改绘制**：
`lv_obj_hit_test()` → `lv_obj_get_click_area()` 会把 `ext_click_pad` 加进去
（`lvgl/src/core/lv_obj_pos.c` 的 `lv_obj_get_click_area`）。所以对稿的还原度不受影响。

它还顺带解决另一半问题：LVGL 在手指**移出对象**时发 `PRESS_LOST` 取消点击，小按钮上
轻微的手指漂移就够触发，表现是「点了没反应」；外扩后「仍算在对象内」的范围一起变大。

已落地两处（都是 1024 档暴露出来的，该档 `USER_BACK_BTN_H` 只有 40、`USER_HIST_TAB_H` 只有 32）：

| 控件 | 原命中区 | 外扩量 | 原因 |
|---|---|---|---|
| 全屏浮层圆钮（播放器关闭/播放、相册关闭/左右箭头） | `user_media_btn_size()` 见方 | 不足 `UI_USER_MIN_TOUCH`(48) 时补足 | **三档视觉尺寸提到 96/66/60 后已全部超过 48，这条兜底不再触发**，留着是给日后新档位用 |
| 历史页 / 消息页顶部 Tab 项 | 文字实宽 × `USER_HIST_TAB_H` | `USER_PAD_SM` | 项宽被写死成文字实宽（左右零内距），Tab 之间 `USER_PAD_LG` 全是死区 |
| 延时播放器进度条 | 轨道 `2 × USER_PROGRESS_H`（24~40 高） | `UI_USER_MIN_TOUCH`(48) | 全屏播放时它是唯一定位手段，且是拖动而非点击，起手落偏一点就丢事件。**要同时改两处**：`bottom` 容器的 `pad_top` 让点能递归下来（`lv_indev_search_obj` 只看父节点原始 coords，不含 `ext_click_area`），slider 的 `ext_click_area` 让它自己认这些点 |

**竖向偏差不能靠外扩解决,得改布局**：`ext_click_area` 是四边等量的，治不了不对称偏差。
Tab 项原先照稿做「文字贴行顶 + 下划线贴行底」的绝对定位，那只在行高恰为稿值时成立；
行高从 40 提到 52 后多出来的 12px 全落在文字与下划线之间，表现是**文字偏上、下划线掉到
行底、命中区中心被拖到文字中心下方 17px**。现改为「文字居中 + 下划线紧跟其下
`USER_PAD_SM/2`」的相对关系，整组随行高居中，三档实测偏差降到：

| 档 | 行高 | 文字 y | 下划线 y | 文字中心 − 行中心 |
|---|---|---|---|---|
| 1920 | 60 | 10–37 | 47–49 | −7 |
| 1280 | 52 | 12–30 | 38–40 | **−5**（原 −17） |
| 1024 | 32 | 4–22 | 27–29 | −3 |

剩下这几像素是下划线本身占位导致的（整组居中，文字自然略高于行心），属正常。

外扩量**只补到够用为止**：`ext_click_area` 是四边等量的，给多了会和相邻控件的命中区打架。
Tab 取 `USER_PAD_SM`（小于 Tab 间距 `USER_PAD_LG` 的一半），故相邻两项不重叠。

**Tab 项的构造已抽成 `ui_user_make_tab_item()`**（`components/ui_user_widgets.*`），历史页与
消息页共用——原先两页各写一份**字面重复**的实现（连注释都一样），命中区这类修改要同步改两遍。

**字号必须当参数传进去，不能建完再改**：该函数量出文字实宽后把项宽与下划线宽（文字宽的
一半）都写死，事后改 `text_font` 不会重新量，文字撑出容器、又因为是居中对齐而被两头均分
裁掉。踩过一次：历史页改浅色稿时把 16 换成 24（比例 1.5），`Timelapse` 就只剩中间的
`melaps`、`Video` 剩 `ide`、`Photo` 剩 `hot`。两页的稿值本就不同（消息页 16、历史页 24），
所以 `font` 是必传参数而不是给默认值。

### LVGL 配置依赖（`3rdparty/.../configs/rongsu_ui.defaults`）
- `LV_USE_OPENGLES=1` → `LV_LINUX_DRM_USE_EGL` 被自动置 1（`lv_conf_internal.h`），所以显示驱动是 `lv_linux_drm_egl.c` 而**不是** `lv_linux_drm.c`（后者整个文件被 `!LV_LINUX_DRM_USE_EGL` 排掉）。而 `LV_USE_DRAW_OPENGLES` 默认 0，**绘制仍是软件的**，GPU 只负责贴整屏纹理并做屏幕旋转。看 `refr 5s:` 时这一条是前提，见 [§ 主线程性能探针](#主线程性能探针ui_perf_probe)
- `LV_USE_TJPGD=1`：历史缩略图 baseline JPEG 解码
- `LV_USE_FS_POSIX=1` + `LV_FS_POSIX_LETTER='A'`：按路径加载 `/userdata/media/video`、`/userdata/media/photo` 图片（`"A:/userdata/media/video/x.rthm"` / `"A:/userdata/media/photo/x.rthm"`）
- lvgl 目标加 `_DEFAULT_SOURCE`（`3rdparty/.../CMakeLists.txt`）：修 FS 驱动 `DT_DIR` 在 `_POSIX_C_SOURCE` 下不可见的编译错误
- 缩略图 `toast` 与播放器同建于 `lv_layer_top()`，全屏时提示可见
- **libpng 不走 lv_conf，是 CMake 探测的**（`app/ui/CMakeLists.txt` → `UPC_UI_HAS_LIBPNG`）：拖动预览与相册大图都在后台线程用它（`core/png_decode_util.*`）。探不到时两处解码恒失败——拖动预览退回只刷小框，相册浏览器则转交 LVGL 内置 lodepng 同步解（会卡顿，但不至于打不开）

## 多后端 IPC

- UI = **Hub**；`add_ipc_peer(ModuleId, path, required)`
- 单播：`target_id = APP_*`；广播：`target_id = 0`
- 启动顺序见 `scripts/rongsu_app.sh`（`scripts/rongsu.sh` 选 A/B 槽后交棒）
- 细节：[common/COMMON.md](../../common/COMMON.md) § IPC

当前 peer：`APP_GLAVO`（required）、`APP_CAMERA`（optional，预览）、`APP_MEDIA`（拍照/录像/直播；Glavo 打印录像经 Hub 转发至此）。

## CMake

`ui/CMakeLists.txt` GLOB `lib/*/src`；新页面 `.cpp` 放入 `pages/src/` 即可。

## 常见错误

| 现象 | 处理 |
|------|------|
| 收不到 Glavo 回复 | 主循环是否 `process()` |
| 切页后仍收旧回调 | `on_exit` 清空 register |
| IPC 发错后端 | 检查 `target_id` |

## 相关文档

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [camera/CAMERA.md](../camera/CAMERA.md) · [media/MEDIA.md](../media/MEDIA.md) · [common/COMMON.md](../../common/COMMON.md)
