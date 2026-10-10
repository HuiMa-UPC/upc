# Toast 样式归类清单

2026-10-10 新稿把操作反馈 toast 统一成三种样式，全部 toast 都改走新样式、按语义归类。
本文是那次改动的归类依据，也是以后新增 toast 时选类型的规则。

接口（`app/ui/lib/components/inc/ui_feedback.hpp`）：

```cpp
void ui_toast_show(const char *msg, ui_toast_type_t type, uint32_t duration_ms);
/* type: UI_TOAST_SUCCESS（常规）/ UI_TOAST_ERROR（错误）/ UI_TOAST_INFORM（注意） */
```

改版前的接口传的是颜色（`lv_color_t`），浅色档里只画成左侧一道色条；新稿去掉了色条，
颜色改由类型决定、同时作用于图标和文字。

## 样式规格

稿：常规反馈 6448:54033、错误提示 6448:54043、注意 6448:54059（图标组件集
「状态icon-顶部toast」6448:53995 的 success / error / inform 三个变体）。

三种版式完全一致，只差颜色与图标：

- 白底、1px `#333145` 5% 描边、圆角 16、阴影 0 2 6 黑 4%；宽度随内容
- 内距上下 16、左右 32；20 的圆形状态图标，图标与文字间距 8
- 文字 SemiBold 18 / 行高 24，与图标同色（工程字库只有一档字重，落地为 `user_font_text()`）

| 类型 | 图标 | 文字色 |
|---|---|---|
| 常规 `UI_TOAST_SUCCESS` | `#485DFF` 圆 + 白对勾 | `#485DFF` |
| 错误 `UI_TOAST_ERROR` | `#FF3131` 圆 + 白叉 | `#FF4242` |
| 注意 `UI_TOAST_INFORM` | `#FFAC26` 圆 + 白色 i | `#FFAC26` |

错误那一档稿上圆是 `#FF3131`、字是 `#FF4242`，差一点点，照稿两个值都用。

## 位置

- 顶部 y=48（改版前是贴屏底 24）
- 有侧栏的页面在侧栏右侧的内容区里水平居中（1280 档中心 x=704）
- 没有侧栏的两个全屏页（延时摄影播放器 `user_timelapse`、相册大图 `user_photo`）按整屏居中
- 深色档不再保留：白卡压在全屏视频上同样清楚

## 归类规则

| 场景 | 类型 |
|---|---|
| 操作完成、已生效、已开始（含「已下发」） | 常规 |
| 操作失败、需要用户重试或处理 | 错误 |
| 被拦下 / 当前不可用、请稍等、进行中、输入不合法、说明类提示、失败但系统会自动处理 | 注意 |

## 清单（76 处）

行号以改动前为准。「原颜色」是改版前调用时传的那个颜色；标 ★ 的是归类与原颜色不一致的
地方，共 22 处（按颜色机械映射会出错的就是这些）。

### 错误（19 处）

| 文件:行 | 文案 | 原颜色 | 备注 |
|---|---|---|---|
| core/src/ui_app.cpp:167 | Couldn't check for updates. Try again later. | `user_color_status_err` | |
| core/src/ui_app.cpp:177 | 后端给的原因，缺省 Update didn't start | `user_color_status_err` | |
| core/src/ui_app.cpp:294 | OTA failed: … | `user_color_status_err` | |
| core/src/ui_app.cpp:388 | Update failed: … | `user_color_status_err` | |
| core/src/control.cpp:159 | Transfer demo timed out | `user_color_status_err` | |
| core/src/control.cpp:670 | Door did not open | `user_color_status_err` | |
| core/src/control.cpp:718 | Transfer demo failed | `user_color_status_err` | |
| core/src/control.cpp:762 | Delete failed | `user_color_status_err` | 选文件页删除失败 |
| core/src/control.cpp:776 | Prepare failed | `user_color_status_err` | |
| core/src/control.cpp:893 | Transfer failed | `user_color_status_err` | |
| core/src/control.cpp:2001 | Resume failed, please retry | `user_color_status_err` | |
| core/src/control.cpp:2885 | 后端动作失败的原因文案（`on_backend_action_fail`） | `user_color_status_err` | |
| pages/src/page_user_history.cpp:664 | Delete failed, retry later | `user_color_status_err` | |
| pages/src/page_user_monitor.cpp:2598 | Pause failed, please retry / Resume failed, please retry | `ui_color_danger` | |
| pages/src/page_user_monitor.cpp:3392 | Photo save failed | `ui_color_danger` | |
| pages/src/page_user_settings.cpp:2615 | Failed to save setting | `user_color_status_err` | |
| pages/src/page_user_settings.cpp:3261 | Format failed | `ui_color_danger` | |
| pages/src/page_user_settings.cpp:3272 | Connect failed | `ui_color_danger` | |
| pages/src/page_user_debug.cpp:3137 | O2 校准失败（`o2_calib_err_text()`） | `user_light_notice_dot` | ★ 校准结果失败，归错误 |

### 注意（30 处）

| 文件:行 | 文案 | 原颜色 | 细分 / 备注 |
|---|---|---|---|
| core/src/page_manager.cpp:462 | Printing in progress | `user_color_accent` | ★ 被拦下（打印中点文件页） |
| core/src/ui_app.cpp:312 | Printing in progress, cannot reboot | `user_color_status_err` | ★ 被拦下 |
| core/src/control.cpp:760 | Cannot delete while printing | `user_color_status_err` | ★ 被拦下（见待议项） |
| core/src/control.cpp:2230 | Test mode active | `user_color_status_err` | ★ 被拦下 |
| pages/src/page_user_monitor.cpp:2417 | Retrieve unavailable | `ui_color_danger` | ★ 被拦下 |
| pages/src/page_user_monitor.cpp:3117 | Powder cleaning is busy | `ui_color_danger` | ★ 被拦下 |
| pages/src/page_user_debug.cpp:2366 | Transfer demo unavailable | `user_color_status_err` | ★ 被拦下 |
| pages/src/page_user_monitor.cpp:2665 | Air purifying, please wait | `user_color_accent` | ★ 请稍等（吹粉中点取件） |
| core/src/ui_app.cpp:162 | An update is already in progress / Already checking for updates | `user_color_accent` | ★ 请稍等 |
| pages/src/page_user_monitor.cpp:1775 | Step must be at least 1 mm | `ui_color_danger` | ★ 输入不合法 |
| pages/src/page_user_monitor.cpp:1881 | Step must be a non-zero whole number | `ui_color_danger` | ★ 输入不合法 |
| pages/src/page_user_debug.cpp:1010 | Cannot save while printing | `user_light_notice_dot` | 被拦下 |
| pages/src/page_user_debug.cpp:1015 | No center offset loaded | `user_light_notice_dot` | 被拦下 |
| pages/src/page_user_debug.cpp:1072 | Print or test mode active | `user_light_notice_dot` | 被拦下 |
| pages/src/page_user_debug.cpp:1123 | Cannot calib while printing | `user_light_notice_dot` | 被拦下 |
| pages/src/page_user_debug.cpp:1129 | Ref O2 must be 0–100% | `user_light_notice_dot` | 输入不合法 |
| pages/src/page_user_debug.cpp:3325 | Test file missing or busy | `user_light_notice_dot` | 被拦下 |
| pages/src/page_user_debug.cpp:3338 | Point test unavailable | `user_light_notice_dot` | 被拦下 |
| pages/src/page_user_debug_test.cpp:377 | 点动任务需填写>0的运行时间 → **Jog task needs a run time > 0** | `user_light_notice_dot` | 输入不合法；文案改英文，见下 |
| pages/src/page_user_debug_test.cpp:384 | 任务表为空，请先新增任务 → **Task table is empty, add a task first** | `user_light_notice_dot` | 输入不合法；文案改英文，见下 |
| core/src/ui_message_store.cpp:259 | Reference capture failed, retrying... | `user_color_status_err` | ★ 失败但自动处理（见待议项） |
| core/src/ui_message_store.cpp:265 | First layer check failed, recoating... | `user_color_status_err` | ★ 失败但自动处理（见待议项） |
| pages/src/page_user_timelapse.cpp:712 | MP4 open failed, fallback JPEG | `user_color_accent` | ★ 失败但自动处理（见待议项） |
| core/src/control.cpp:907 | USB removed | `user_color_status_err` | ★ 事件说明 |
| pages/src/page_user_settings.cpp:632 | Coming soon | `user_color_text_dim` | ★ 说明 |
| pages/src/page_user_settings.cpp:2313 | Format requested (backend pending) | `user_color_accent` | ★ 说明（后端未实现） |
| pages/src/page_user_monitor.cpp:606 | Capturing photo... | `user_color_accent` | ★ 进行中（见待议项） |
| pages/src/page_user_settings.cpp:1811 | Disconnecting... | `user_color_accent` | ★ 进行中（见待议项） |
| pages/src/page_user_settings.cpp:2722 | Checking for updates... | `user_color_accent` | ★ 进行中（见待议项） |
| pages/src/page_user_settings.cpp:3250 | Formatting... N% | `user_color_accent` | ★ 进行中（见待议项） |

### 常规（27 处）

| 文件:行 | 文案 | 原颜色 | 备注 |
|---|---|---|---|
| core/src/ui_app.cpp:157 | You're up to date | `user_color_status_ok` | |
| core/src/ui_app.cpp:287 | OTA files ready | `user_color_status_ok` | |
| core/src/ui_app.cpp:381 | Update package removed | `user_color_status_ok` | |
| core/src/control.cpp:864 | File received: … | `user_color_status_ok` | |
| core/src/control.cpp:2666 | Transfer complete. Remove the box | `user_color_status_ok` | 原为 `ui_toast_show_icon` + `ic_badge_check`，图标现由类型决定 |
| core/src/control.cpp:2772 | Print complete | `user_color_status_ok` | |
| core/src/ui_message_store.cpp:262 | Reference captured, starting first-layer check | `user_color_status_ok` | |
| core/src/ui_message_store.cpp:268 | First layer check passed | `user_color_status_ok` | |
| pages/src/page_user_history.cpp:667 | Deleted | `user_color_accent` | |
| pages/src/page_user_monitor.cpp:3390 | Photo saved to … | `user_color_accent` | |
| pages/src/page_user_settings.cpp:1699 | Wi-Fi On / Wi-Fi Off | `user_color_accent` | |
| pages/src/page_user_settings.cpp:1804 | Network forgotten | `user_color_accent` | |
| pages/src/page_user_settings.cpp:1820 | Network forgotten | `user_color_accent` | |
| pages/src/page_user_settings.cpp:2618 | Slicer will auto-start print / Transfer only, no auto-start | `user_color_accent` | |
| pages/src/page_user_settings.cpp:2632 | Height calibration started | `user_light_accent` | |
| pages/src/page_user_settings.cpp:3255 | Format complete | `user_color_status_ok` | |
| pages/src/page_user_settings.cpp:3270 | Connected | `user_color_status_ok` | |
| pages/src/page_user_debug.cpp:431 | Work zero set | `user_light_accent` | |
| pages/src/page_user_debug.cpp:1025 | Saved | `user_light_accent` | |
| pages/src/page_user_debug.cpp:1040 | Calibration started | `user_light_accent` | |
| pages/src/page_user_debug.cpp:1050 | Height calibration started | `user_light_accent` | |
| pages/src/page_user_debug.cpp:1060 | Galvo move sent | `user_light_accent` | |
| pages/src/page_user_debug.cpp:1080 | Galvo point test started | `user_light_accent` | |
| pages/src/page_user_debug.cpp:1143 | O2 calib started | `user_light_accent` | |
| pages/src/page_user_debug.cpp:2631 | Power to external | `user_light_accent` | |
| pages/src/page_user_debug.cpp:2638 | Alarm reset sent | `user_light_accent` | |
| pages/src/page_user_debug.cpp:3135 | O2 calib OK K=… | `user_light_accent` | |

### 本次新增

| 位置 | 文案 | 类型 |
|---|---|---|
| 消息页管理态批量删除成功 | Deleted | 常规 |
| 消息页左滑删除单条成功 | Deleted | 常规 |

## 待议项

以下几组按规则归了类，但存在另一种合理归法，后续若有调整改这里与对应调用点即可：

- **「进行中」4 处**（Capturing photo... / Disconnecting... / Checking for updates... /
  Formatting... N%）：按规则归注意。它们是中性的进度提示，若觉得橙色 i 偏重，可改归常规。
- **「失败但自动处理」3 处**（Reference capture failed, retrying... / First layer check
  failed, recoating... / MP4 open failed, fallback JPEG）：归注意而不是错误，因为用户不需要做什么。
- **Cannot delete while printing**：是删除被拦下，归注意；真正的删除失败（Delete failed）才是错误。

调试测试页那两条原是中文：toast 用的是 Montserrat，主题里没有配中文回退字体，中文子集的
生成脚本也已不在 `tools/` 里，原文在屏上显示不出来，所以这次改成英文。

## 维护约定

- 新增 toast 必须按上面的「归类规则」选类型，不要按想要的颜色反推类型。
- 新增或改动调用点时，同步更新本文的清单（行号可不追，文件与文案要对得上）。
