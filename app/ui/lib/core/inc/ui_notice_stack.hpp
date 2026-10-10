#ifndef UI_NOTICE_STACK_H
#define UI_NOTICE_STACK_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_notice_stack.hpp
 * @brief 屏幕右上角的通知区：toast / 铃铛 / 面板三态互斥。
 *
 * 数据来自 `ui_message_store`（观察者驱动，自己不订阅总线）。整块建在 shell_ 上、
 * 与常驻设备信息条同级，所以它压在所有页面之上、又在弹窗那层（`lv_layer_top`）之下。
 *
 * ---- 三态（浅色稿 6064:71609 的 toast 与 6578:41549 的面板）----
 *
 *   TOAST  有还挂在 toast 上的消息（store 里 visual == SHOWN）→ 摆最多两张白卡。
 *          停留到时限由 store 按等级计时收起（INFO 5s / 告警级 15s）。
 *   BELL   没有 toast 时的常态。有「还需留意的事」就带一颗红点，没有就是干净的铃铛。
 *   PANEL  点铃铛展开，列最近两条需要留意的 + 「查看剩余 N 条」（2026-10-10 版稿：面板
 *          加宽到 460、每条 72 高）。点任一条或「查看剩余」都进消息页 All Tab 顶部；
 *          点面板外任意处收回。
 *
 * 三者占的是同一块地方（稿上它们的右缘都在 1232），任一时刻只有一个可见——稿里那几层
 * 图层互相重叠不是画错了，就是这个意思。
 *
 * 「需要留意」的判据与两颗红点共用 `ui_message_needs_attention()`，所以**红点亮与面板
 * 里有东西恒等价**，不会出现点开是空的。没红点时点铃铛直接跳消息中心。
 *
 * ---- 位置与例外 ----
 *
 * 贴右上角，右缘退一个 USER_HOME_EDGE_GAP（稿 48）——与首页打印态/监控态右上角那块环境区
 * （`build_env_wrap`）、右下开门滑块同一个退让量，两者上下相接看着是一栏。容器四周另有
 * 一圈投影余量（不然 LVGL 按容器范围把投影裁掉），对齐时已扣掉，不影响上面这些位置。
 *
 * 作用域只在首页（见 `apply_device_bar()`）。首页的打印态/监控态是整屏摄像头预览，
 * 那两个态仍用改版前那摞黑玻璃卡（见 `ui_notice_stack_set_dark()`），并用
 * `ui_notice_stack_set_top_offset()` 压到环境区下面。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 建（单例，重复调用返回同一个）。须在 `ui_message_store_init()` 之后。 */
lv_obj_t *ui_notice_stack_create(lv_obj_t *parent);

/** 按页显隐：只有首页显示。 */
void ui_notice_stack_set_visible(bool visible);

/**
 * 把整块往下让（像素）。默认 0 = 贴屏顶内缩一个 USER_HOME_EDGE_GAP。
 * 首页打印态/监控态传右上角环境区的高度 + 间距，让两者上下相接。
 */
void ui_notice_stack_set_top_offset(int32_t px);

/**
 * 切到深色档（首页打印态 / 监控态）。
 *
 * 那两个态背后是整屏摄像头画面、页面本身还是暗色的，浅色稿那套白 toast 与铃铛都不合适，
 * 所以仍走改版前那摞黑玻璃卡：没有铃铛也没有面板，消息一直摆着直到被收起或撤回。
 * 等打印态的新稿到位再统一。
 */
void ui_notice_stack_set_dark(bool dark);

/** 铃铛红点开关。与侧栏那颗共用判据，由 `ui_app.cpp` 的 sync_message_badge 一处驱动。 */
void ui_notice_stack_set_badge(bool on);

#ifdef __cplusplus
}
#endif

#endif
