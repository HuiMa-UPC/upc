# app/ui/ — LVGL 界面进程

> 索引：[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md)  
> 可执行文件：**`rongsu_ui`** · 库 **`ui::port`**

## 职责

| 子目录 | 说明 |
|--------|------|
| `lib/core/` | PageManager、`UiMsgProxy`、`control`、摄像头预览、常驻设备信息条 + 共享设备状态 |
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

while (!g_shutdown_requested) {
    port.process(0);                        // 渲染前先 drain IPC
    const uint32_t delay = lv_timer_handler();
    int wait_ms = delay > 0 ? (int)delay : 1;
    wait_ms = std::min(wait_ms, 8);         // 限制 blocked poll，避免拖死 IPC
    port.process(wait_ms);
    port.process(0);                        // 同帧 Req/Rsp
    upc_sys_monitor_tick();
}
```

后端→后端（如 Glavo→Camera 打印录像）由 Hub 在 `process()` 内 **`IPC_FWD` 同步转发**，不经过 UiMsgProxy。预览像素走 dma fd 旁路 + SHM 元数据环，不走 MsgPort payload。

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

### 侧栏图标的两态（稿组件集 2235:8697）

稿里这套图标是一个组件集，7 个图标（home / history / control / settings / message /
new message / test）× 选中·未选中两态。两态只差填充：

| | 稿 | 实现 |
|---|---|---|
| 选中 | 139° 渐变 `#4054FD → #4D80EE`（节点 2235:8699） | 同向渐变但**色值拉宽**为 `#3348E8 → #6699F5`，见下 |
| 未选中 | `#F0F0FF` @ **20% 不透明** | 不透明 `user_color_icon_off`（`#555555`） |
| 底板 | **无** | 无（只在按压时出现，见下） |

**未选中态刻意不照抄那 20% 半透明**：理由同上一段——整屏预览时侧栏背后是视频，
半透明图标在亮帧下会糊掉。

**选中态的渐变靠位图蒙版做**，因为 `nav_icons.c` 那五枚是 A8（只存 alpha），
`image_recolor` 只能上单色。每个菜单按钮里因此建两层互斥的图标（`PageEntry` 的
`menu_icon_img` / `menu_icon_grad`，由 `update_menu_highlight()` 切显隐）：未选中露
A8 图标 recolor 成灰，选中露一块铺了渐变的方块、用同一份 A8 当 `bitmap_mask_src`
裁成字形。三个坑都在 LVGL 的实现里：

- **蒙版按原尺寸居中贴、不缩放**（`lv_draw_sw_img.c` 的 `apply_mask()`），所以那块方块
  必须与图头等大（64×64），缩到 `USER_NAV_ICON_SZ` 只能靠 `transform_scale`。
- `transform_pivot` 默认在左上角，不挪到中心就会朝左上缩。
- 那块 64×64 比按钮大（1024 档 58、1280 档 52），**按钮必须加
  `LV_OBJ_FLAG_OVERFLOW_VISIBLE`**，否则先被父节点裁掉再缩放，字形四角就缺了。

**渐变色值比稿宽，是刻意的**：稿那两色只差绿通道 44（红 +13、蓝 −15），亮度差约 28/255，
在 54px 的字形上肉眼就是一片纯蓝。所以起点压暗、终点提亮到 `#3348E8 → #6699F5`（亮度差
约 64）。同一处理见 `ui_slide_unlock` 的红条——注释里的稿值是 `#FF2424 → #FF4B4B`，实取
`#D00A0A → #FF8484`。**别照稿值改回去**，那等于把渐变关掉。

色标停靠点用 `{45, 198}` 而不是 `{0, 255}`：渐变铺满整个 64×64 方块，而字形只占 x/y 的
6..57，沿对角线是整条色带的 9%~89%，用 `{0,255}` 两端各浪费一截。稿上的 11%/86% 本来也是
相对字形自身算的（渐变挂在字形节点上，不是带留白的外框），换算过来即这两个值。

另外 `calculate_layer_type()` 里 `transform_scale != 256` 会让图层类型变成 TRANSFORM
而不是 `bitmap_mask_src` 那档的 SIMPLE——两者在 `lv_refr.c` 里走的是同一个分支、蒙版
都会传下去，且 `obj_draw_size` 取的是未变换坐标，所以对齐没问题。但前提是
`LV_DRAW_TRANSFORM_USE_MATRIX = 0`（本工程生成的 `lv_conf.h` 正是 0）；那个宏一旦打开，
满不透明的变换对象会改走 `refr_obj_matrix()` 旁路，蒙版会失效。

**按压反馈分两档**（稿里没有这个元素，是工程自己加的交互反馈）：按未选中的标签是
白 20%，按已选中的标签是主色蓝 20%，靠 `LV_STATE_CHECKED | LV_STATE_PRESSED` 这个组合
选择器区分。LVGL 取「状态位是当前状态子集」里状态值最大的那条，`CHECKED`(1<<2) +
`PRESSED`(1<<7) = 132 > 单独 `PRESSED` 的 128，所以组合态一定胜出。

**`LV_STATE_CHECKED` 那档必须显式设成 `LV_OPA_TRANSP`，不能只是「不设」**。这是实机踩过
的坑：`lv_btn_create` 会带上默认主题，而主题给每个普通按钮的 checked 态都挂了
`bg_color_secondary`（副色 + `LV_OPA_COVER`，见 `lv_theme_default.c` 的 apply 分支），
副色是 `lv_display.c` 自动初始化主题时传的 `LV_PALETTE_RED`。局部样式优先于主题样式，
所以原先「`accent` + 20%」那两行其实一直在替我们压着这块红底；把它们直接删掉，选中态
就变成一块不透明红底。现在改成局部把 `bg_opa` 压成透明来压制，`bg_color` 不用管。
反过来也别给它设可见的背景色——那就成了稿上没有的常显底板。

这类「清理样式反而让默认主题从背后冒出来」的问题，根治办法是菜单按钮改用
`lv_obj_create` 或建完就 `lv_obj_remove_style_all()`，代价是按压反馈等观感要重新核一遍。

### 常驻设备信息条（`ui_device_bar` + `ui_device_state`）

顶部「WiFi + 设备名 + 状态点 + 状态字」那一组不属于任何页面，由 `PageManager::init()`
建在 `shell_` 上（`content_panel_` 之后 → 恒定压在所有页面之上，含首页的整屏摄像头层；
`menu_panel_` 仍在它之上）。它同样是 `FLOATING` + `pos(0,0)`，否则会占掉 `shell_`
横向 flex 的一个格位把内容区挤走。条自身 `remove_flag(CLICKABLE)`，不然整屏宽的带子
会吃掉底下控件的触摸。

- **让位**：`content_panel_` 的 `pad_top = USER_TOPBAR_H`，页面按 `LV_PCT(100)` 建出来
  的内容区自然落在条下面。
- **显隐规则**：`apply_device_bar()` 按「侧栏有菜单按钮 = 显示」判定，即五个标签页显示，
  隐藏页（选文件 / 调试 / 延时摄影）不显示且 `pad_top` 收回 0。以后新增隐藏页默认不显示。
- **弹窗**：条在 `shell_` 内，打印流程/传输弹窗的全屏遮罩建在 `screen_` / `lv_layer_top()`
  上，会连它一起压暗，与页面表现一致。
- **首页例外**：首页要整屏出血，根节点用 `pos(-USER_SIDEBAR_W, -USER_TOPBAR_H)` 跨过两个
  方向的 padding，尺寸取 `LV_HOR_RES` × `LV_VER_RES`（`LV_PCT(100)` 算的是父内容区，会被
  `pad_top` 扣掉一条）。首页自己那条 `USER_TOPBAR_H` 高的透明行现在是**空占位**（稿
  5277:11571 起右侧那两颗按钮已移到左下的方块上），但不能删——首页根节点是纵向 flex，
  其余几层都是 `FLOATING` 不占格位，它是唯一撑起那一格的子节点。
- **`USER_TOPBAR_H` = 屏高 16%**（1024×600 档 96、1280×800 档 128）。调试页那条传感器卡
  原先借用该字段，现在用 `USER_SENSOR_STRIP_H`（值即旧的 `USER_TOPBAR_H`），不随带子变高。

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

### 消息中心与通知浮层（`ui_message_store` + `ui_notice_stack`）

设备事件/告警只有一套显示通道：屏幕左下角那摞通知卡。它和 toast 分工不同——
**toast 是操作反馈，看完就走；通知是待处理的事，撤回之前一直在**。
两者互不替代，不要把 toast 改成通知或反过来。

数据在 `core/ui_message_store`（同 `ui_device_state` 的形态：单例 + 观察者），
两个来源：

1. 后端 `GLAVO_Device_Event_Ntf`（`DeviceEventPayload`：码 + 等级 + active + 触发时刻），
   码表与 [`doc/设备通信协议-数据结构定义（v0.9）.md`](../../doc/设备通信协议-数据结构定义（v0.9）.md)
   第五节那张表一一对应。`active=false` 走 `ui_message_retract()`；
2. 前端本地消息，码段从 `DeviceEventCode::UiWearMask`(512) 起，后端不发这一段。
   目前是打印结束那两条（催吹粉 / 提醒戴口罩），由 `apply_print_phase()` 推和撤。
3. MCU 报警（`core/ui_alarm_state`，见下节「MCU 报警」）：文案在 UI 侧由字典生成，
   走 `ui_message_push_ex()`（外部文案/等级版本的 push，文本指针须指向调用方的
   稳定存储，`ui_alarm_state` 用自有定长缓冲保证这一点）。

**文案不进线协议**：总线上只跑数值码，人话由 `ui_message_store.cpp` 的 `kCatalog` 出。
等级也由码表定，不用 payload 里那个 `severity` 字段——那个是留给云端的，同一个码在
UI 上必须恒定同一种观感，否则后端改个字段卡片就换色了。当前文案是按稿意写的英文占位。

后端那条回调跑在总线线程上，而每一次变更都会让浮层重建 LVGL 对象，所以
`on_device_event` 先 `lv_async_call` 切回 UI 线程再落库（payload 里有个 64 位时间戳，
塞不进 `lv_async_call` 那个指针，只能堆上过一手）。浮层里的 × 和「查看剩余」按钮
同理要异步：它们的动作都会 `lv_obj_clean` 整栈，把正在派发事件的自己一起删掉。

`ui_message_push()` 是**幂等**的：同一个码已经挂在浮层上就什么也不做。
`apply_print_phase()` 每次态刷新都无脑 push 那两条，不在 store 里吸收掉的话卡片会不停
重建、已读也会被顶回未读。排序用内部 `seq` 而不是 `ts_ms`：后端给的是 Unix 时间、本地消息
压根没有时间，两种纪元混着比大小会把本地消息全排到最旧。

浮层（`core/ui_notice_stack`）：

| 项 | 取值 |
|---|---|
| 位置 | `shell_` 上的 `FLOATING`，与常驻设备信息条同级 → 压在页面之上、弹窗层之下 |
| 对齐 | 贴右上角（稿 5277:11778），右缘退一个 `USER_PAD`，与首页右上环境区同一个退让量 |
| 宽度 | `USER_PRINT_CARD_W`（稿上 416@1280，与该常量的 458 同一量级；已不再与 stats-card 对齐） |
| 条数 | 最多两张卡，多出来的收进「View remaining N messages」，点进消息页 |
| 排序 | 口罩提醒钉第一格；其余先按等级降序、同级比新 |
| 徽标 | 四档等级配色 + 图标，见下「通知徽标配色」；与消息页列表共用一份映射 |
| 告警卡 | 非 INFO 级，**不可关** |
| 信息卡 | INFO 级（蓝 + `ic_toast_info`），右上一颗 ×，点了只从浮层撤下、消息在列表里还留着 |
| 显隐 | 同常驻设备信息条那条规则（侧栏五个标签页显示、隐藏页不显示），另外排除消息页自己、历史页与设置页 |

**右上角是有人抢的**，两类页面两种处理：

- **首页打印态 / 待机监控态**：那个位置是环境区（`build_env_wrap()`），整栈用
  `ui_notice_stack_set_top_offset()` 压到它下面去。让位量在 `page_user_home.cpp` 的
  `sync_notice_offset()` 里一处算完（取 `env_block_t::root` 的高度，`LV_SIZE_CONTENT`
  算出来的，取值前必须先 `lv_obj_update_layout()`，否则首次进打印态拿到 0 会压在环境区
  上）。它是首页专属的，`apply_device_bar()` 换页时一律清零，首页在 `on_enter`
  （即 `page_user_home_refresh`，进出打印态/监控态也都会走到）里按当前态再让一次。
- **历史页 / 设置页**：那个位置是 Storage 容量条和 Wi-Fi 的 On/Off 开关，**开关可点，
  被浮层压住连触摸一起吃掉**，所以整栈直接不显示，与排除消息页那句写在一起。这两页的新
  消息只靠侧栏消息图标的红点提示。曾试过降级成单行细条，但细条底是半透明的，与 Storage
  条叠印两层字都读不清；往下移又会压到 `Manage` 按钮和那个开关，故放弃折叠这条路。

稿上这摞卡是紧贴屏幕右边缘的，圆角写成 `8/0/0/8`（右侧两角是直的）。这里整栈退了一个
`USER_PAD` 与环境区对齐、没有贴边，所以四角一律圆。

侧栏红点：`PageManager::set_menu_badge(page, on)`，未读数 >0 时亮，进过一次消息页
（`page_user_message_refresh` → `ui_message_mark_all_read()`）就全部标已读、红点灭。
红点是叠在菜单按钮上的一个小圆 `lv_obj`，不是重出一版图标资源——稿里是把红点烤进
SVG 的，那样没法按未读数开关。

**后端还没实现 `GLAVO_Device_Event_Ntf` 的发布**，接好的回调目前收不到任何消息。
要验「超过两条 + 查看剩余 N 条」这条路径，用调试页 Other → Other Switches → `Msg Inject`
（造 4 条假消息：1 条 critical + 3 条 info）。

### 通知徽标配色（消息页列表 + 左下浮层共用）

唯一来源是 `ui_message_severity_color()` / `ui_message_severity_icon()`
（`core/ui_message_store`）。**两处必须共用**：同一条消息在浮层和消息页列表里
不能是两种颜色。报文 severity（1..4）先经 `ui_alarm_severity_to_msg()` 换成
`ui_msg_severity_t` 再取色，报警与设备事件因此落在同一套阶梯上。

| 等级 | 语义（协议第 7 节） | 色 | 图标 |
|---|---|---|---|
| `UI_MSG_SEV_INFO` | 1 information 信息提示 | 蓝 `#485DFF` | 对话气泡 |
| `UI_MSG_SEV_ERROR` | 2 warning 警告 | 黄 `#FFD426` | 警告三角 |
| `UI_MSG_SEV_CRITICAL` | 3 fault 故障 | 橙 `#FF8A3D` | 警告三角 |
| `UI_MSG_SEV_EMERGENCY` | 4 trip **安全停机** | 红 `#FF4242` | 警告三角 |

图标资产是 `notice_badge_icons`（源 SVG 从稿 3058:7234 / 3058:7059 导出，存在
`tools/_badge_svg/`，`tools/gen_notice_badge_icons.py` 可重跑）。**每档屏各出一版**
（14 / 18 / 27 px，对应徽标边长 26 / 32 / 48 的 9/16），`ui_message_severity_icon()`
按 `g_ui->profile_id` 选。这样 `ui_user_make_icon_img()` 里 `target == src_sz`、
缩放整段被跳过——原先共用一张 36px 位图，1024 档要缩到 0.39 倍，细描边糊成一团。
**改尺寸表要两边同步**（生成脚本的 `SIZES` 与那个 switch）。

两条不要改回去的理由：

- **相邻两档不能同色相靠明度区分**。原先 3/4 是红 `#FF4242` + 深红 `#D32029`，
  实机上分不出来——暗背景下暗色视觉后退，最严重那档反而最不显眼。现在是蓝→黄→
  橙→红的色相阶梯，两两拉开；稿只定义了两端（3058:7058 info 蓝 / 3058:7233
  alert 红），中间两档本就是自由发挥的空间。
- **trip 不是"更严重的 fault"**，是保护动作已经发生、设备已自动停机（如打印中
  开门 → 激光立即断电）。排序上 `ui_alarm_state::rebuild_order()` 也按 severity
  降序，trip 永远排在当前报警最上面。若日后觉得颜色仍不够，加文字胶囊比再调色
  有效，但标签要写结果（"STOPPED"）而不是协议术语（"TRIP"）。

生命周期胶囊（Active / Recovered - awaiting reset / Event / Resolved）是**另一套
语义**，配色在 `page_user_message.cpp` 的 `kPill*`，不要和等级配色混用。

### MCU 报警（`ui_alarm_state` + `ui_alarm_catalog`）

MCU 经串口上报的报警（`GLAVO_Alarm_Report_Ntf`，0xA3/0x20 原样透传）在 UI 侧有唯一
数据源 `core/ui_alarm_state`，按 `state_mask` 推导生命周期（协议见
[`doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md`](../../doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md)）：

- **EVENT**（一次性）：记历史 + 上浮层蓝卡（5s 自动从浮层撤下，列表里还在）；
- **CONDITION**（状态型）：ACTIVE / RTN_LATCHED 期间挂「当前报警」表并同步红卡到
  `ui_message_store`；回 NORMAL 撤卡、转历史。

文案由 `ui_alarm_catalog`（`tools/gen_alarm_catalog.py` 从 doc/ 下三份 CSV 生成
`ui_alarm_catalog_gen.cpp`，英文翻译表 `doc/alarm_catalog_en.csv` 可覆写，未翻译的
用 macro 名转写兜底）+ 报文 detail 组合；查不到的 code/detail 降级显示原始编号
**绝不丢弃**（固件可能比字典新）。历史落盘 `/home/messages/alarm_history.jsonl`
（上限 200 条，只存原始字段，文案加载时按当前字典重新生成）。**目录由
`ui_alarm_state_init()` 自己 `create_directories`**——UI 可能比 Media 先起，不能
指望别的进程建；不建的话 `fopen` 失败会把历史整段静默丢掉（现已补 `UPC_LOG_ALARM_STATE`
错误日志）。这里刻意**不放在 `/home/media` 下**：报警历史不是媒体，早先挂在那儿只是
为了借 `UPC_MEDIA_ROOT` 那个宏，现已解除对 `media_paths.hpp` 的依赖。

复位：`ui_alarm_request_reset(code)` → `UI_Alarm_Reset_Req` → Glavo 组 0x82 帧下发。
仅 `clear_mode=RESET` 受理；受理后置 `reset_pending`，等 MCU 下一帧上报翻转，10s
无回报自动解除 pending（按钮恢复可点）。

**快照对齐（`GLAVO_Alarm_Snapshot_Ntf`，0xA3/0x81）**：单条上报只在状态变化时发，
所以 UI 重启 / 串口重连 / SOC 比 MCU 后启动这几种情况下，之前就亮着的报警**再也收不到**，
本地表会和 MCU 真实状态脱节。快照给的是此刻的全量位图，收齐后把本地这份副本刷一遍：

- **攒帧**：快照按 32 条一组分片，`word_count` 帧才算一批。`word_count` **不能写死**
  ——报警表随固件增长，当前 66 条 CONDITION 要 3 帧，而
  `doc/主控板串口通信协议 - 信息报警命令详表.csv` 的注释还停在 2，所以一律读帧里的值。
  按 `word_index` 填槽、bitmask 记已收，不依赖到达顺序；重复 index、`word_count` 变了、
  或攒超 2s，任一命中就整批作废重来——**半套位图会把没收到那几组的报警误判成已解除**；
- **对齐**：快照亮着而本地没有的补进来，本地有而快照说不亮的转历史。补进来的那条只有
  code 可用，`severity` / `clear_mode` 走字典兜底（`报警&事件表.csv` 生成的
  `severity_fallback` / `clear_mode_fallback`），`detail` 记 0、时刻记补齐时刻——
  这几样快照里根本没有。字典查不到就按最保守的一档：warning 且不给复位钮；
- 反向清理**只认受快照管辖的 code**（`ui_alarm_snapshot_slot_of()` 查得到位图槽位）。
  EVENT 不占 bit、字典比固件旧时新码也没有槽位，这些"快照里没提"是信息缺失不是状态变化，
  照清会把真报警抹掉；
- 已在表里的条目只校正生命周期（ACTIVE ↔ RTN_LATCHED），
  `severity` / `clear_mode` / `detail` 保持单条上报给的值——那才是权威数据。

⚠ 协议里**没有 SOC→MCU 的快照查询命令**（`信息报警命令详表.csv` 只写了「MCU 主动上报
快照后 SOC/HMI 应以最新快照刷新显示」），所以 UI 这边纯被动收。补齐能不能真的发生，
取决于 MCU 重连后会不会主动推一轮——这条待 MCU 组确认；若确认不推，得请他们加一条
查询命令，UI 侧再在 `ui_alarm_state_init()` 和串口重连回调里各发一次。

打印融合三处（1124/1125 未复位时 MCU 拒绝新打印）：

- **入口拦截**：`control.cpp` 的 `on_detail_start_print()` 先查
  `ui_alarm_print_blocker()`，有阻断报警弹「Unable to start print」，可复位的给
  Reset 直达键；
- **结果页失败原因条**：终止态浮层顶部横条（`page_user_home.cpp` 的
  `build_result_alarm_bar()` / `sync_result_alarm_bar()`），Reset 钮三态随
  `ui_alarm_add_observer()` 回调刷新；
- **会话期抑制**：进/出打印态调 `ui_alarm_set_print_session()`，会话内 1124/1125
  不上浮层卡（结果页已给原因 + Reset），退出时仍未解除的补回浮层。

消息页视觉基线是稿 3033:5247（列表）/ 3063:7338（左滑删除）/ 3201:9570（详情抽屉），
几处稿→工程的换算与取舍记在这里，改版前先看：

- 卡片填充稿上是白 4%，实现用**等效的不透明底**（`user_color_panel`，同历史页）：
  半透明会让垫在卡片下层的删除圆钮直接透出来（它靠卡片挡着，左滑才该露），
  且每帧与背景混合、滚动时开销大。1px 白 10% 描边保留，玻璃边的观感还在。
  按下态改**底色**抬一档（`user_color_panel_alt`）而不是调 `bg_opa`——调透明度
  同样会透出删除钮。内边距与列间距取 `USER_PAD`（稿 24），圆角 `USER_BTN_RADIUS`（稿 8）；
- 左侧徽标是**实色圆角方块 + 白图标**（稿 32 见方 / 18 图标），与左下通知卡
  `ui_notice_stack::build_card()` 同款，两处观感必须一致；
- 右缘留 `content_right_pad()` 给悬浮翻页钮，取值与历史页同源，两页卡片右缘对齐；
- 左滑删除钮是红色渐变（`#FC3D3E→#FF6667`）圆钮，右缘与卡片右缘齐平，
  卡片左移 `swipe_reveal_px()` = 钮直径 + `USER_PAD`；
- 左滑机制见下方「左滑删除：跟手拖动」——消息页与历史页共用同一套写法；
- **删除后不跳回顶部**：`rebuild_list()` 重建前记 `lv_obj_get_scroll_y()`、重建后
  `lv_obj_update_layout()` 再按 `lv_obj_get_scroll_bottom()` 夹着滚回去。
  `lv_obj_clean` 会把滚动位置清零，不接这一手删一条就弹回顶部；夹取也不能省——
  删掉末尾几条后列表变矮，旧滚动量越界会停在空白上。两页都是这么做的；
- **三 Tab + Manage 行**（稿 3033:5247 新版，结构与打印历史页同构）：`All` / `Device` /
  `System`，选中项白字 + 文字半宽的主色下划线，行下一条通栏分隔线。分档判据是**码段**：
  `DeviceEventCode::UiWearMask`(512) 以上是「UI 本地事件，后端不应发送」，正好是天然
  分界——**Device** = MCU 报警（码 ≥1000）+ 后端设备事件（码 <512），**System** = 前端
  本地提示（512..999）。稿上 System 那栏画的是 OTA 升级、账号共享，**本工程还没有这类
  数据源**，所以该 Tab 目前只有打印结束那两条本地提示，多数时候是空的；也因为只有报警
  历史可删、而报警属于 Device，**System Tab 下不会出现 Manage 按钮**。
  切 Tab 会退出管理态并回到列表顶部。稿上 Manage 摆在 x=1068，既不对齐卡片右缘也不对齐
  翻页钮，这里按工程惯例用 `content_right_pad()` 对齐卡片右缘；
- **分页：初始只建最近 10 条 + 底部「Load More」**（稿 5380:28971 的 `查看更多`，节点
  5380:29045）。`collect_items()` 已按 `sort_ts` 降序，所以取前 `g_visible_limit` 条就是
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

  稿 5389:29103 那个**加载圈有意未实现**：消息数据全在内存（三路 store 合并 + 排序，
  无 IO、无异步），点开就是同步多建几张卡；而 LVGL 单线程，同步重建期间画面根本不刷新，
  转圈一帧都不会转。将来消息真接了云端，把展开改成回调式再启用它。

  分页之前进页有肉眼可见的卡顿：`switch_to()` → `refresh()` → `rebuild_list()` 全程同步，
  页面要等整张列表建完 + 首帧渲染才显示。列表长度当时没有上限，三路上限相加约 300 条
  （当前报警 `kCurrentCap` 80 + store `kCapacity` 24 + 历史 `UI_ALARM_HISTORY_CAP` 200，
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
- **管理（多选）模式**：入口是 Tab 行右侧的 Manage。
  只有历史条目可删，所以不可删的卡片不出复选框、点了没反应（判据 `MsgItem::deletable`），
  管理态期间左滑让位给勾选。批量删除按下标**从大到小**执行——
  `ui_alarm_history_delete_at()` 会把后面的条目前移，正序删第二条就删错人了。
  注意：管理态下若有新消息到达会触发列表重建，勾选状态随之丢失（同历史页）；
  另外**全选/批量删除只覆盖已展开的卡片**——两者都遍历 `g_cards`，而折叠状态下
  `g_cards` 里只有可见的那 10 条。排查「全选怎么没选全」先看这条；
- 详情抽屉建在 **`lv_layer_top()`** 上、宽 49%（稿 632/1280，整屏为基准）：稿上抽屉
  是满高的，它自带的顶栏顶替右半屏那段状态栏。**不能挂在页面根节点上**——那样上边会
  被 `content_panel_` 的 `pad_top` 顶下来，且常驻设备条画在页面之上会压住抽屉头。
  代价是遮罩连侧栏一起盖住，开着抽屉点侧栏是先关抽屉（标准模态，已确认）。
  layer_top 不随页面隐藏销毁，靠 `page_user_message_exit()` 里的 `drawer_close()` 收尾；
  抽屉顶栏高度取 `USER_TOPBAR_H`，标题才与左半屏设备条上的设备名同一基线；
- 稿是中文的，工程 UI 统一英文（Montserrat 字库），文案不照抄（"通知详情" →
  "Notification Details"，"确认解障" → Reset 三态）；
- 稿上抽屉里 448×280 那块是处理步骤图，**暂无图片资源故不画**；正文字号维持比标题
  小一档（稿是同号 20，1024 屏上同号一屏只放得下一条半）。

**模拟数据一律带 `[SIM] ` 前缀**：调试页两个注入开关造出来的 9 条假消息，标题前面
都加 `[SIM] `，真消息一个字不改，联调时一眼分得清。报警侧在 `compose_text()` 里加
（浮层卡 / 消息页列表 / 详情抽屉 / 历史读的都是这一份文案，一处加完四处都有）；
设备事件侧走 `ui_message_debug_push()`（文案仍取自码表，只是套上前缀）。
**假报警还不落盘**：`history_save()` 跳过 `simulated` 条目，重启即净，不用再手删
`/home/messages/alarm_history.jsonl`。同一个码后来收到真报文会自动摘掉 `[SIM]`。

联调：MCU 不发真报警时用调试页 Other → Other Switches → `Alarm Inject`
（`ui_alarm_debug_inject()` 绕过总线直走上报路径，造 5 条假报警：覆盖阻断 /
RTN_LATCHED / 自动清除 / EVENT 四条显示路径，外加一条 critical 把 information/
warning/error/critical 四档等级配色凑齐——等级不进字典、以报文为准，不造就看不到；
关掉开关注入 NORMAL 帧模拟解除）。
注意注入条目按 Reset 会真发 `UI_Alarm_Reset_Req` 到串口，MCU 不回帧时 10s 后
pending 自动解除——正好顺带验超时路径。

### 左滑删除：跟手拖动（消息页 + 历史页共用）

两页各有一份实现（`page_user_message.cpp` / `page_user_history.cpp` 的「左滑删除」
一节），写法必须保持一致——**都不要改回 `LV_EVENT_GESTURE`**，那条路踩过两个坑：

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
- 命中目标要唯一：消息页把卡内子容器的 `CLICKABLE` 全清掉；历史页的缩略图是独立播放
  入口不能清，改为把三个拖动回调在卡片和缩略图上各挂一份（`attach_card_drag()`），
  且 `on_thumb_click()` 同样要判 `just_dragged`。

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
重采样——RGA 只做色彩转换不做缩放。

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
| **Local** | UI 直接读 `/home/Gcode/config.json`（`common/fs_manifest/gcode_manifest.*`），**不发** `Fs_DirOpen_Req` |
| **USB** | `send_fs_dir_open(Usb)` → Glavo 扫盘 → `Fs_DirOpen_Rsp` |

删除 Local / Prepare / Start 仍走 IPC。

### 选文件页详情面板：打印板厚度（稿 4014:16631）

右侧详情列自上而下：缩略图 → 文件名 → meta 行 → 分隔线 → `Plate Thickness` 标题 →
`5mm / 10mm` 分段 → 未选提示 → 撑开 → `Confirm Print` → `Delete File`。

- **必须显式选择**：`g_plate_thickness`（-1 未选 / 0=5mm / 1=10mm）。未选时分段全灰、
  提示行「Select a plate thickness to continue」可见、`Confirm Print` 置灰。置灰要同时
  `add_state(DISABLED)` **和** `remove_flag(CLICKABLE)`——`DISABLED` 只换样式，照样发
  `LV_EVENT_CLICKED`。三处联动都在 `apply_plate_thickness_ui()` 里，别散着写。
- **逐文件**：改选文件 / 切 Local↔USB / 删除后都 `reset_plate_thickness()`。
  `apply_files_list()` 的目录刷新按文件名保住选中项，那里**不清**厚度。
- **无选中态的分段控件**：`ui_user_make_segmented(..., UI_USER_SEG_NONE)`，读回同样是
  这个哨兵。老调用方传的都是合法索引，行为不变。
- **后端**：契约还没有厚度字段，值目前只停在 UI。接上之后在 `on_print_confirm_ok()` 随
  `send_print_prepare()` 下发，改契约走 `common/COMMON.md` 四步。
- **1024 档的高度预算**：详情列 `pad_row` 用 `USER_PAD_SM`（不是 `USER_PAD`），且耗时/耗材
  由两行压成「图标 + caption + value」一行，腾出的空间刚够放厚度区。再往这一列加东西前
  先算 600px 屏。
- **按钮圆角**：稿里这套按钮组件是 8px@1280 的圆角矩形，不是全圆胶囊，用
  `USER_BTN_RADIUS`（1024=6 / 1280=8 / 1920=12）。`Confirm Print` 借 `user_style_btn_primary()`
  取主色与外发光，但要就地覆盖它的 `LV_RADIUS_CIRCLE`。

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
| `user_control` | `page_user_control.*` | skeleton 占位 |
| `user_settings` | `page_user_settings.*` | 设置列表 + 开发者密码弹窗 |
| `user_message` | `page_user_message.*` | 消息中心：日期分组混排列表（MCU 报警 + 设备事件）+ 初始 10 条与底部 Load More + 左滑删除历史 + 管理态批量删除 + 详情抽屉（Reset 三态）。按稿 3033:5247 / 3063:7338 / 3201:9570 / 5380:28971 实现 |
| `user_timelapse` | `page_user_timelapse.*` | 延时摄影播放器（隐藏页，历史缩略图进入；全屏 `lv_layer_top`） |

注册仅在 `ui_app.cpp`；打印流程走 `control_*`，pages 不直接订阅 MsgBus。详见 `lib/pages/README.md`。

### 首页待机布局（稿 5277:11571）

`g_normal_area` 一层浮在整机背景大图之上，四角各摆一块，全是 `FLOATING`：

| 位置 | 控件 | 说明 |
|------|------|------|
| 左上 | HOME 空气质量卡 `build_home_card()` | PM2.5/PM10 可切换 + 四档色条 + 游标。顶距 `USER_TOPBAR_H + USER_PAD_LG*2`（三档 320/216/168，对应稿上等比折算的 312/208/166），不再垂直居中——左下要给入口胶囊腾位 |
| 右侧 | Cabin condition `build_cabin_panel()` | 温度/氧浓度/湿度/气压**四行**（稿上舱内 PM2.5 那行已删，它只在打印态那条 5 列环境小条里保留一列） |
| 左下 | 入口胶囊行 `build_action_tiles()` | `Monitor` + `Start Print`，见下 |
| 右下 | 开门滑块 `build_door_floating()` | `SLIDE TO UNLOCK`（稿 5339:27265 起改的文案；三处开门滑块仍统一同一个串：首页、监控页、放版弹窗，控件的默认兜底值也是它；取件那条仍是 `SWIPE TO COLLECT`）。到底那帧的 `RELEASE TO OPEN` 是控件内写死的，四条滑块共用，没跟着改 |

**右侧面板的引线**：稿上分隔线比标题和数据行的左缘再往左探出一截（三档 104/72/58 =
`USER_PAD_LG + USER_PAD`），左端坐一颗空心圆环（`USER_STATUS_DOT_SZ * 4/3`，白 30% 描边、
无填充，与线同色）。做法是面板整体加宽到 31%（文字区仍是原来的 28%），分隔线独占全宽，
标题靠 `margin_left` 缩回去、四行数据套一层 `pad_left` 容器缩回去——数据行是 `LV_PCT(100)`
宽的，直接加左边距会连宽度一起推出右边缘。环与线在一条 flex 行里紧挨着排，线用 `flex_grow`
吃掉剩余宽度；**别改用「`PCT(100)` + `pad_left`」**，`lv_obj_align()` 和百分比都是相对内容区
算的，加了 padding 连圆环一起被推走。

**整机背景大图**（资源 `home_bg`，由 `tools/gen_home_bg.py` 生成，源节点 3752:9081「背景图2」）：

资源固定 1024×640（比例 1.6，与稿 1280×800 一致），运行时 `LV_IMAGE_ALIGN_COVER` 铺满
`build_main_area` 那层。三档倍率：1920 档放大 1.758×、1280 档 1.172×、1024 档 0.9375×
（缩成 960×600、左右各裁 8px）。只有 1024 档是下采样、清晰，另两档会发软；当前只按
1024 出货，未处理——要治就把源图出到 1800×1125，三档全变下采样。

**为什么用背景图2 而不是背景图1**（3752:8895）：两者是同一张渲染，背景图2 把场景连同渐变层
往左平移了 55px@1280 = 44px@1024。新首页稿 5339:27207 自己那层背景用的是背景图1 的构图
（拼贴组 x=-1065），我们改用背景图2，是为了抵消下面两处实现偏差。

**`SHIFT_LEFT_PX = 20` 是补偿，不是设计值**。两处偏差叠起来约 59px：

1. 稿里背景铺满整屏 1280、左侧导航是 20% 黑的浮层压在背景之上；而 `build_main_area` 是
   本页根节点的子节点、被 `pad_left` 挡在侧栏右边，背景只铺内容区 944。同一张 1.6 的图
   塞进更窄的框、再从 x=80 起画，等效把设备往右推约 26px。（同文件的打印态/监控态摄像头层
   反而是对的，用 `lv_obj_set_pos(area, -USER_SIDEBAR_W, 0)` 跨回去铺满整屏。）
2. CABIN 面板取内容宽 28%，稿是 294/1152 = 25.5%、左缘落在 946/1280，我们的左缘比稿偏左
   约 33px。

真去修这两处的话，`SHIFT_LEFT_PX` 应该回到 0。实测数据（1024 档，均换算到 1024×640 资源
坐标，只统计面板占据的那段高度 y 176..465）：面板左缘 x≈695.5；背景图2 原始设备右缘 x=683
（间隙 12 屏幕px）、背景图1 是 x=727（**压进文字 30px**）；补 20px 后设备右缘 x=663、间隙
30 屏幕px，与稿的 36px@1280 ≈ 29px 对齐。

左移后右侧空出的 20px 用**主体末尾同宽的水平镜像**补上。**不要**改回「取最右一列 resize 成
该宽度」——旧版就是那么做的，结果第 951–1023 列逐字节完全相同，每行退化成一根纯色横杠，
上屏就是右边缘一片竖向条纹（已在实机复现）。镜像能延续横向纹理，接缝逐行连续。

**左下两枚入口胶囊**（稿 5339:27275。这两个入口先随稿 5277:11454 从顶栏的「查看监控」圆钮
与「Start Print」胶囊搬到左下变成方块，又随稿 5339:27275 从方块改成一行等高胶囊）：

```
┌───────────────────┐ ┌──────────────────────────┐
│ [摄像机] Monitor    │ │ [文件夹]      Start Print  │
└───────────────────┘ └──────────────────────────┘
```

| 项 | 取值 |
|---|---|
| 整行宽 | `USER_HOME_ACTION_ROW_W`（636/424/339）。灰钮 `SIZE_CONTENT` 撑宽，蓝钮 `flex_grow=1` 吃掉剩余——稿上蓝钮就是 `flex-1` |
| 高 / 圆角 | 两枚都是 `USER_BTN_PRIMARY_H × 5/4`（90/65/55）+ `LV_RADIUS_CIRCLE`。这个高度算法照抄 `ui_user_make_primary_pill_btn`，**不要换成别的常量**：同一行两枚差几像素就看得出来 |
| 图标 | `ic_camera` / `ic_folder`，边长 `USER_BTN_PRIMARY_H × 5/8`（稿上 32@1280 = 胶囊高的一半） |
| 灰钮 | 页内 `make_glass_pill_btn()`：`0x757575` + `LV_OPA_20` + 白 20% 描边，与 `make_glass_round_btn` 同一份材质，只是形状从圆钮变胶囊。图标 + 单行文字水平居中 |
| 蓝钮 | 公共组件 `ui_user_make_primary_pill_btn(..., width=0, spread=true)`，与历史页空态那枚是同一枚按钮；`spread` 让图标贴左缘、文字贴右缘 |
| 内边距 | 稿上左 24 / 右 32@1280 不对称，实现两侧统一 `USER_PAD`（28/22）——差不到 4px，换来三档不用各写一组 |
| 描边 | 稿上是渐变描边，LVGL 画不了，各取中间值当单色：灰钮白 20%、蓝钮白 40%。灰钮那圈 inset 阴影与背景模糊同样画不出来，略去 |

旧方块上那行静态提示语 `Place the build plate first` 在新稿里**已删除**。

`USER_HOME_ACTION_ROW_W` 实取比稿宽 10%（稿 386@1280 → 实取 424）：稿排 Poppins、工程用
Montserrat，同字号更宽。按稿值折到 1024 档只有 309，此时蓝钮内可用宽 140，装不下
图标(27) + `Start Print`(约 85) 还留出间距；放到 424 后 1024 档是 339、蓝钮内余量约 30px。
同一套「按稿宽补 10%」的处理见 `USER_PRINT_CARD_W`。改两枚文案前先量一遍：label 是
`LONG_WRAP`，超了会折成两行把胶囊撑变形，而不是截断。

### 打印态浮层（六态一套）

打印中与打印结果不是两套界面，而是 `page_user_home` 打印浮层 `g_print_area`
**内部**的同一层 `g_print_overlay`（稿 3588:8735 / 4045:17582 / 3589:9992 /
3588:8907 / 4234:6385 / 4045:18253）。摄像头背景与 `CameraPreviewPlayer` 全程
不断流，避免黑屏闪烁。

浮层里的常驻控件：

| 位置 | 控件 | 说明 |
|------|------|------|
| 左下 | stats-card `g_pc_card` | 缩略图 + 徽标行（图标/状态词/百分比）+ 进度条 + TIME/CONSUMABLES 两列 |
| 右上（环境区下方） | 通知浮层 | **不在本层**，是全局的 `ui_notice_stack`；进出打印态时按环境区高度把它往下压 |
| 右下 | 运行组 `g_ctl_run` | 暂停·恢复钮 + 终止钮；与取件组互斥 |
| 右下 | 取件组 `g_ctl_collect` | 吹粉钮 + `SWIPE TO COLLECT` 胶囊；与运行组互斥 |
| 右中 | 拍照钮 `g_result_photo_btn` | 六态都有；`make_glass_round_btn()` + `ic_photo` |
| 右上 | HOME PM2.5 卡 + 5 列环境条 | 六态都有；`build_env_wrap()`，右边缘与拍照钮同齐 |

两个状态量决定外观，改哪个都走 `apply_print_phase()` 一处刷新：

- `g_print_phase`（徽标四档）：`PRINTING...` 蓝 / `PAUSE` 蓝 / `PRINT TERMINATED` 橙 /
  `PRINT COMPLETE` 绿。徽标色同时是百分比色与进度条填充色。
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

`build_env_wrap()` = HOME PM2.5 卡（稿 4231:5740）+ 5 列环境条（稿 4231:5749）。
五列 caption 全是文字（`Pm 2.5` / `Temp.` / `O2` / `Humidity` / `Pressure`），不是图标。
整体贴右上、退 `USER_PAD`，与右侧中部的拍照钮右边缘齐。

打印态浮层与待机监控层是**同时建、靠显隐切**的两层，所以这一块的 label 句柄不能是
单例——后建的会覆盖先建的，先建那层从此收不到刷新。八个句柄收进 `env_block_t`，
`g_env_print` / `g_env_monitor` 各一份，都登记在 `kEnvBlocks[]` 里；
`result_pm_card_update()` 和快照回调遍历这个数组，两份一起写。

### 待机「查看监控」（稿 3122:13224）

右上环境区、右侧中部拍照钮都与打印态共用上面那套构造函数，几何配色天然一致。
左下角是同款玻璃圆钮（图标换 `ic_home`）退出监控，右下角保留 `SLIDE TO UNLOCK` 滑块。

拍照钮的图标是 `ic_photo`（照相机，稿 3847:9440），不是首页顶栏「查看监控」入口那枚
`ic_camera`（摄像机）——两处语义不同，别图省事复用。

时序：

```
Glavo Print_Terminate_Done / 进度 100% → control.cpp finish_printing()
  → enter_state(CONTROL_STATE_PRINT_DONE)   // 只停看门狗，预览保活，界面维持打印中原样
  → （开门 finalize：promote 目录；`UPC_TIMELAPSE_PLAYBACK_PNG=1` 时不编 timelapse.mp4，保留 PNG 即可播；`=0` 时后台编 mp4）
  → APP_Print_Video_Manifest_Ntf (Camera→UI)
  → control.cpp on_print_video_manifest()：按 frames_dir 主键读 /home/media/video/config.json
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
`Other` → `Other Switches` → `Transfer Box` 打开（`control_set_transfer_box_flow()`，纯 UI
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
rongsu_camera 经主 IPC（/tmp/upc_camera.sock）SCM_RIGHTS 附带 dma_buf fd 表
  → CameraPreviewPlayer::on_stream_start_rsp 调用 MsgPort::take_peer_dma_fds(APP_CAMERA)
  → SHM /upc_camera_preview 只读 slot 状态/seq/buffer_idx
  → 解码线程 acquire_latest_frame 得到 buffer_idx
  → [RGA] importbuffer_fd(src_fd) → imcvtcolor → slots_[3] BGRX
  → [SW 回退] mmap(src_fd) → nv12_to_bgrx_sw → slots_
  → lv_timer(33ms) 换 img_dsc_ 指针 → lv_image (XRGB8888)
```

`CameraStreamInfoPayload.dma_socket_path` 为 ABI 保留字段；fd 不经独立 socket 传递。

- **开流**：`page_user_home` 打印模式 → `CameraPreviewPlayer::start()` → IPC Start → `apply_stream_start_ui` 开 SHM 元数据环 + `take_peer_dma_fds` + `setup_frame_buffers` + 解码线程。
- **关流/换流**：UI 仅请求解码线程退役；后台回收线程等待退出，LVGL 定时器确认退出后才关闭旧 SHM、DMA fd 与显示缓冲，因此主线程不阻塞在 `join()`。
- **恢复职责**：Camera 侧采集看门狗是唯一的自动重连发起者；UI 只检测并记录预览卡帧，等待 Camera 重建 SHM 与重推 `Stream_Start_Rsp`，不再自行 Stop/Start 流。
- **RGA 路径**：`setup_frame_buffers` 时 `RgaDmaBuf` 分配成功则 `rga_path=1`；失败退化为 `std::vector` 软件转换。无进程级预热，首帧到达即转换。
- **DMA 约束**：RK3568 RGA2 仅支持 4G 以下物理地址；源 fd 必须来自 V4L2 / dma-heap（dma32），UI 直接用 `importbuffer_fd`。
- **Stride 对齐**：RGA 源 stride 使用后端传来的实际 `stride_y`，不再假设 `stride_y == width`。
- **Cache 同步**：RGA 读源前 `DMA_BUF_IOCTL_SYNC(READ)`，写目的后 `DMA_BUF_IOCTL_SYNC(WRITE)`；目的缓冲优先 uncached。
- **线程**：解码在独立线程；`on_stream_start_rsp` 与换帧须 `lv_async_call` / `lv_timer` 回到 UI 线程。
- **CMake**：找到 `librga` 时 `-DUPC_UI_HAS_RGA=1`（当前源码仍硬依赖 RGA 头文件）。

## 打印历史 + 延时摄影播放器

| 组件 | 文件 | 职责 |
|------|------|------|
| `page_user_history` | `pages/page_user_history.*` | 读 `/home/media/video/config.json`（`common/fs_manifest/print_history_manifest`）→ 按 `start_unix` 倒序、按自然日分组的卡片列表；缩略图 `A:/home/media/video/{thumb}`（RTHM 解码，`LV_IMAGE_ALIGN_COVER`）；**Album** 扫 `/home/media/photo/*.png`，缩略图优先同名 `.rthm`；顶部三 Tab + Storage 容量条；管理（多选）模式批量删除；上下翻页；**左滑删除**（跟手拖动，见「左滑删除：跟手拖动」一节）→ History/Timelapse **按类型独立删除**后重写 config.json；Album 删 PNG + 配对 `.rthm` |
| `page_user_timelapse` | `pages/page_user_timelapse.*` | 全屏播放页（建于 `lv_layer_top()`）：受 `UPC_TIMELAPSE_PLAYBACK_PNG` 控制——默认 PNG 序列；`=0` 时优先 `timelapse.mp4`（失败回退 PNG）；拖动预览为后台小图 scrub，松手后再 settle 主画面 |

- **进入/退出**：历史页缩略图点击 → `page_user_timelapse_set_source(目录,mp4路径,标题,时间)` + `switch_to("user_timelapse")`；关闭 → `switch_to("user_history")`。
- **LVGL 线程规则**：PNG 序列播放由主线程 `lv_timer` 换主画面；拖动预览解码在后台线程，经 `lv_async_call` 贴预览框。

### 打印历史页布局（稿 4366:5400 一套六态）

对应稿：列表常态 `4366:5400`、管理态 `4470:6682`、左滑删除 `4470:6970`、空态 `4531:6938`、
存储将满 `4536:7333`、存储已满 `4550:8303`。全页文案统一英文（CJK 字库只有调试页那点子集）。

- **顶部 Tab 行**（高 `USER_HIST_TAB_H`）：`Print History` / `Timelapse` / `Album`，选中项白字 +
  主色下划线（宽度取文字一半，与稿一致；Tab 项宽度是 `lv_obj_update_layout` 量出的文字实宽而
  非 `LV_SIZE_CONTENT`，否则下划线的百分比宽与容器内容宽互相依赖）。右侧常态是 `Manage`，管理态换成
  `N selected + Delete + Select All + Cancel`（都是 `USER_HIST_BTN_W × USER_HIST_TAB_H` 胶囊）。
  两组按钮都上抬 `manage_btn_lift()`（`USER_PAD_SM / 2`）比 Tab 文字略高；按钮高度本就等于行高，
  所以 Tab 行也要 `OVERFLOW_VISIBLE` + `tab_row_ext_draw_cb` 才不被裁掉顶部。
  行下一条通栏分隔线。**Album** 数据源为 `/home/media/photo`（PNG 原图 + 可选同名 `.rthm` 缩略图）。
- **右缘对齐**：Tab 行 / 告警横幅 / 卡片列表 / Storage 条统一用 `content_right_pad()`
  （= 翻页钮直径 + `USER_PAD × 2`）内缩，四者右缘齐平；调翻页钮尺寸只需改这个函数。
- **Storage 容量条**：稿里与设备名同排，而页面内容区从常驻条下沿才开始，所以做成 `FLOATING`
  并用负 y 偏移（`-(USER_TOPBAR_H + h) / 2`）抬回那一行右侧。抬出页面范围要两件事配合：页根加
  `LV_OBJ_FLAG_OVERFLOW_VISIBLE`，再在 `LV_EVENT_REFR_EXT_DRAW_SIZE` 里把额外绘制区撑开
  `USER_TOPBAR_H`（`page_ext_draw_cb`），少一样就会被裁掉。常驻设备条本身透明且不可点，
  所以既不遮挡也不抢触摸。轨道 `USER_HIST_BAR_W ×
  USER_HIST_BAR_H`，填充色三态：<90% 主色蓝 / ≥90% 橙 `#FF7843` / =100% 危险红。
- **存储告警横幅**：≥90% 出现（橙），=100% 转红，同色 10% 底 + 20% 描边 + `ic_alert`。
  纯提示，不拦截任何操作。
- **卡片**（高 `USER_HIST_CARD_H`，缩略图 `8/5 × 高`）：左缩略图（`Timelapse` 角标 + 右下
  `ic_expand` 角标）+ 右信息区（上：时间段 `HH:MM:SS - HH:MM:SS`、文件名 + 状态胶囊
  `Completed`/`Interrupted`；下：`Duration | Filament | Layers`，图标 `ic_clock` / `ic_cube` /
  `ic_layers` 24px 1:1）。卡片底色用不透明 `user_color_panel` 代替稿里的 4% 白，且缩略图不裁
  圆角——`clip_corner` 每帧建掩膜层是列表滚动卡顿主因，这里选性能。
- **状态胶囊紧跟文件名**（不是右对齐到行尾）：文件名标签不设 `flex_grow`，改用
  `max_width: LV_PCT(65)` 夹住过长的名字（否则胶囊被挤出卡片），并把高度锁成一行
  ——v9 的 `LONG_MODE_DOTS` 是「先按宽度换行、再截末行」，不锁高度只会换行、出不来省略号。
- **管理模式**：卡片左侧插入 `USER_HIST_CHECK_SZ` 复选框（隐藏时 flex 自动跳过，缩进天然
  跟着走）；点卡片任意位置或缩略图切换勾选；期间禁用左滑；`Delete` 走
  `ui_user_make_confirm_footer_btn` 那套二次确认弹窗，确认后按当前 Tab **独立删除**：
  Timelapse 只清延时资产（`*.png` / `thumb.rthm` / `timelapse.mp4`）；Print History 只清
  打印录像（`print_h264.mp4` / `print_thumb` / `print.h264`）。另一侧仍保留；两侧都空
  才移除清单条目并删会话目录。`video_pending`（延时摄影仍在合成）/
  `print_video_pending`（连续 H264 仍在 remux）的条目不可勾选、也挡住左滑删除，
  避免打断落盘。关「Print H264 Rec」时不写 `print_video_pending`，Print History
  不展示对应项，也不会卡在 Generating。
- **空态**：80px 圆底 + `ic_printer` + 双行文案 + `Start Print`。按钮直接调 `control_start()`——
  放版预确认弹窗挂在整屏 overlay 上，与当前页面无关，所以不必先切首页；`control_is_printing()`
  为真时只弹 toast。文案与 `Start Print` 显隐只由 `apply_empty_copy()` 维护（Timelapse / Album
  空态不带按钮），所以**任何显示空态的路径都必须走 `show_empty(true)`**：左滑删到最后一条时
  曾自己拼过一份只显示占位盒子的简化版，逛过另两个 Tab 后按钮的 `HIDDEN` 不会被清掉，
  表现为删空后不出 `Start Print`、换 Tab 再回来才出现。

**三处等后端补数据**（其余字段 manifest 已齐）：

| 缺口 | 现状 | 补法 |
|------|------|------|
| `Layers` | 显示 `--` | manifest 加层数字段（Camera 可取换层 Snap 的最大 `layer_index`） |
| Storage 容量 | `page_user_history.cpp` 顶部 `kStorageUsedMB/kStorageTotalMB` 常量占位 | 接上报后改 `storage_used_mb()/storage_total_mb()` 两个取数函数即可 |
| Timelapse / Album | 已按 Tab 分支：Timelapse 读 manifest；Album 扫 `/home/media/photo/*.png`（缩略图优先 `.rthm`） | — |

### LVGL 配置依赖（`3rdparty/.../configs/rongsu_ui.defaults`）
- `LV_USE_TJPGD=1`：历史缩略图 baseline JPEG 解码
- `LV_USE_FS_POSIX=1` + `LV_FS_POSIX_LETTER='A'`：按路径加载 `/home/media/video`、`/home/media/photo` 图片（`"A:/home/media/video/x.rthm"` / `"A:/home/media/photo/x.rthm"`）
- lvgl 目标加 `_DEFAULT_SOURCE`（`3rdparty/.../CMakeLists.txt`）：修 FS 驱动 `DT_DIR` 在 `_POSIX_C_SOURCE` 下不可见的编译错误
- 缩略图 `toast` 与播放器同建于 `lv_layer_top()`，全屏时提示可见

## 多后端 IPC

- UI = **Hub**；`add_ipc_peer(ModuleId, path, required)`
- 单播：`target_id = APP_*`；广播：`target_id = 0`
- 启动顺序见 `scripts/rongsu.sh`
- 细节：[common/COMMON.md](../../common/COMMON.md) § IPC

当前 peer：`APP_GLAVO`（required）、`APP_CAMERA`（optional，预览 + 打印录像转发）。

## CMake

`ui/CMakeLists.txt` GLOB `lib/*/src`；新页面 `.cpp` 放入 `pages/src/` 即可。

## 常见错误

| 现象 | 处理 |
|------|------|
| 收不到 Glavo 回复 | 主循环是否 `process()` |
| 切页后仍收旧回调 | `on_exit` 清空 register |
| IPC 发错后端 | 检查 `target_id` |

## 相关文档

- [glavo/GLAVO.md](../glavo/GLAVO.md) · [common/COMMON.md](../../common/COMMON.md)
