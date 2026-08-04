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

### 常驻设备信息条（`ui_device_bar` + `ui_device_state`）

顶部「WiFi + 设备名 + 状态点 + 状态字」那一组不属于任何页面，由 `PageManager::init()`
建在 `shell_` 上（`content_panel_` 之后 → 恒定压在所有页面之上，含首页的整屏摄像头层；
`menu_panel_` 仍在它之上）。它同样是 `FLOATING` + `pos(0,0)`，否则会占掉 `shell_`
横向 flex 的一个格位把内容区挤走。条自身 `remove_flag(CLICKABLE)`，不然整屏宽的带子
会吃掉底下控件的触摸——首页那两颗按钮正在它下面。

- **让位**：`content_panel_` 的 `pad_top = USER_TOPBAR_H`，页面按 `LV_PCT(100)` 建出来
  的内容区自然落在条下面。
- **显隐规则**：`apply_device_bar()` 按「侧栏有菜单按钮 = 显示」判定，即五个标签页显示，
  隐藏页（选文件 / 调试 / 延时摄影）不显示且 `pad_top` 收回 0。以后新增隐藏页默认不显示。
- **弹窗**：条在 `shell_` 内，打印流程/传输弹窗的全屏遮罩建在 `screen_` / `lv_layer_top()`
  上，会连它一起压暗，与页面表现一致。
- **首页例外**：首页要整屏出血，根节点用 `pos(-USER_SIDEBAR_W, -USER_TOPBAR_H)` 跨过两个
  方向的 padding，尺寸取 `LV_HOR_RES` × `LV_VER_RES`（`LV_PCT(100)` 算的是父内容区，会被
  `pad_top` 扣掉一条）。首页自己那条 `USER_TOPBAR_H` 高的透明行只剩右侧两颗按钮，正好落在
  常驻条下方，两者合成稿 3540:14473 里的一整条带子。
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

## 页面（`lib/pages/`）

| Page ID | 文件 | 说明 |
|---------|------|------|
| `user_home` | `page_user_home.*` | 首页：环境、打印模式、预览、打印结果页 |
| `user_history` | `page_user_history.*` | 打印历史：三 Tab + Storage 条 + 日期分组卡片 + 管理态批量删除 + 左滑删除 |
| `user_control` | `page_user_control.*` | skeleton 占位 |
| `user_settings` | `page_user_settings.*` | 设置列表 + 开发者密码弹窗 |
| `user_message` | `page_user_message.*` | skeleton 占位 |
| `user_timelapse` | `page_user_timelapse.*` | 延时摄影播放器（隐藏页，历史缩略图进入；全屏 `lv_layer_top`） |

注册仅在 `ui_app.cpp`；打印流程走 `control_*`，pages 不直接订阅 MsgBus。详见 `lib/pages/README.md`。

### 打印态浮层（六态一套）

打印中与打印结果不是两套界面，而是 `page_user_home` 打印浮层 `g_print_area`
**内部**的同一层 `g_print_overlay`（稿 3588:8735 / 4045:17582 / 3589:9992 /
3588:8907 / 4234:6385 / 4045:18253）。摄像头背景与 `CameraPreviewPlayer` 全程
不断流，避免黑屏闪烁。

浮层里的常驻控件：

| 位置 | 控件 | 说明 |
|------|------|------|
| 左下 | stats-card | 缩略图 + 徽标行（图标/状态词/百分比）+ 进度条 + TIME/CONSUMABLES 两列 |
| 左下（卡上方） | 提示条 `g_pn_root` | 仅结束态出现，两个变体：催吹粉（蓝）/ 提醒戴口罩（橙） |
| 右下 | 运行组 `g_ctl_run` | 暂停·恢复钮 + 终止钮；与取件组互斥 |
| 右下 | 取件组 `g_ctl_collect` | 吹粉钮 + `SWIPE TO COLLECT` 胶囊；与运行组互斥 |
| 右中 | 拍照钮 `g_result_photo_btn` | 六态都有 |
| 右上 | HOME PM2.5 卡 + 5 列环境条 | 六态都有 |

两个状态量决定外观，改哪个都走 `apply_print_phase()` 一处刷新：

- `g_print_phase`（徽标四档）：`PRINTING...` 蓝 / `PAUSE` 蓝 / `PRINT TERMINATED` 橙 /
  `PRINT COMPLETE` 绿。徽标色同时是百分比色与进度条填充色。
- `g_purify_stage`（结束态下的吹粉子阶段，走 `purify_stage_set()`）：`IDLE` / `RUNNING`
  （吹粉钮图标 2s 一圈逆时针转、胶囊文案换 `AIR PURIFYING...`）/ `DONE`（提示条换口罩提醒）。
  按稿 4234:6385，吹粉中**不改徽标**——吹粉态只靠状态栏与胶囊文案表达。

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
注意 btn 同时还是右侧中部拍照钮的直径（稿上两者同尺寸），以及右上环境条为让开它
所留的右边距。

时序：

```
Glavo Print_Terminate_Done / 进度 100% → control.cpp finish_printing()
  → enter_state(CONTROL_STATE_PRINT_DONE)   // 只停看门狗，预览保活，界面维持打印中原样
  → （Camera 后台把 PNG 序列编成 timelapse.mp4 并 upsert manifest，可能数秒到数分钟）
  → APP_Print_Video_Manifest_Ntf (Camera→UI)
  → control.cpp on_print_video_manifest()：按 frames_dir 主键读 /home/video/manifest.json
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
- **占位定时器**：契约里没有「转运盒到位 / 转移完成 / 闸门到位 / 解锁」任何一条信号，所以
  ① 的到位靠 `BOX_READY_MS` 定时器翻 `g_box_ready`，② 的完成靠 `BOX_MOVE_MS`，③ 稿上本就没有
  到位检测。信号就位后把这两处换成轮询/应答即可，UI 结构不用动。三步都复用
  `g_door_advance_timer`（互斥使用，`enter_state → stop_door_timers` 统一清理）。

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
- **关流**：`stop()` → join 解码线程、`clear_frame_buffers()` 释放 DMA / 关闭 fd 表。
- **RGA 路径**：`setup_frame_buffers` 时 `RgaDmaBuf` 分配成功则 `rga_path=1`；失败退化为 `std::vector` 软件转换。无进程级预热，首帧到达即转换。
- **DMA 约束**：RK3568 RGA2 仅支持 4G 以下物理地址；源 fd 必须来自 V4L2 / dma-heap（dma32），UI 直接用 `importbuffer_fd`。
- **Stride 对齐**：RGA 源 stride 使用后端传来的实际 `stride_y`，不再假设 `stride_y == width`。
- **Cache 同步**：RGA 读源前 `DMA_BUF_IOCTL_SYNC(READ)`，写目的后 `DMA_BUF_IOCTL_SYNC(WRITE)`；目的缓冲优先 uncached。
- **线程**：解码在独立线程；`on_stream_start_rsp` 与换帧须 `lv_async_call` / `lv_timer` 回到 UI 线程。
- **CMake**：找到 `librga` 时 `-DUPC_UI_HAS_RGA=1`（当前源码仍硬依赖 RGA 头文件）。

## 打印历史 + 延时摄影播放器

| 组件 | 文件 | 职责 |
|------|------|------|
| `page_user_history` | `pages/page_user_history.*` | 读 `/home/video/manifest.json`（`common/fs_manifest/print_history_manifest`）→ 按 `start_unix` 倒序、按自然日分组的卡片列表；缩略图 `A:/home/video/{thumb}`（RTHM 解码，`LV_IMAGE_ALIGN_COVER`）；**Album** 扫 `/home/photo/*.png`，缩略图优先同名 `.rthm`；顶部三 Tab + Storage 容量条；管理（多选）模式批量删除；上下翻页；**左滑删除**（清除卡片默认 `GESTURE_BUBBLE` 使手势落到卡片）→ History/Timelapse 按 `frames_dir` 整条删目录 + 重写 manifest；Album 删 PNG + 配对 `.rthm` |
| `page_user_timelapse` | `pages/page_user_timelapse.*` | 全屏播放页（建于 `lv_layer_top()` 覆盖侧边栏）：按序播放 `/home/video/{job_ts}/1.png...N.png`，顶部标题/时间、右上关闭、中央播放/暂停、底部进度条、点画面显隐控件 |

- **进入/退出**：历史页缩略图点击 → `page_user_timelapse_set_source(目录,mp4路径,标题,时间)` + `switch_to("user_timelapse")`；关闭 → `switch_to("user_history")`。
- **LVGL 线程规则**：序列播放由主线程 `lv_timer` 驱动换图。

### 打印历史页布局（稿 4366:5400 一套六态）

对应稿：列表常态 `4366:5400`、管理态 `4470:6682`、左滑删除 `4470:6970`、空态 `4531:6938`、
存储将满 `4536:7333`、存储已满 `4550:8303`。全页文案统一英文（CJK 字库只有调试页那点子集）。

- **顶部 Tab 行**（高 `USER_HIST_TAB_H`）：`Print History` / `Timelapse` / `Album`，选中项白字 +
  主色下划线（宽度取文字一半，与稿一致；Tab 项宽度是 `lv_obj_update_layout` 量出的文字实宽而
  非 `LV_SIZE_CONTENT`，否则下划线的百分比宽与容器内容宽互相依赖）。右侧常态是 `Manage`，管理态换成
  `N selected + Delete + Select All + Cancel`（都是 `USER_HIST_BTN_W × USER_HIST_TAB_H` 胶囊）。
  两组按钮都上抬 `manage_btn_lift()`（`USER_PAD_SM / 2`）比 Tab 文字略高；按钮高度本就等于行高，
  所以 Tab 行也要 `OVERFLOW_VISIBLE` + `tab_row_ext_draw_cb` 才不被裁掉顶部。
  行下一条通栏分隔线。**Album** 数据源为 `/home/photo`（PNG 原图 + 可选同名 `.rthm` 缩略图）。
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
  `ui_user_make_confirm_footer_btn` 那套二次确认弹窗，确认后按 `frames_dir` **整条删除**
  （会话目录内 Timelapse + Print Video + 缩略图/PNG 一并清掉，并从 manifest 移除），再整表重建。
  任一 Tab 删除都会让另一 Tab 同步消失。`video_pending`（Camera 还在 remux）的条目不可勾选、
  也挡住左滑删除，避免打断落盘。
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
| Timelapse / Album | 已按 Tab 分支：Timelapse 读 manifest；Album 扫 `/home/photo/*.png`（缩略图优先 `.rthm`） | — |

### LVGL 配置依赖（`3rdparty/.../configs/rongsu_ui.defaults`）
- `LV_USE_TJPGD=1`：历史缩略图 baseline JPEG 解码
- `LV_USE_FS_POSIX=1` + `LV_FS_POSIX_LETTER='A'`：按路径加载 `/home/video`、`/home/photo` 图片（`"A:/home/video/x.rthm"` / `"A:/home/photo/x.rthm"`）
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
