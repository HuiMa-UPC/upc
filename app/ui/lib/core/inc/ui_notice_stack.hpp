#ifndef UI_NOTICE_STACK_H
#define UI_NOTICE_STACK_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_notice_stack.hpp
 * @brief 屏幕左下角的通知浮层：消息中心里最要紧的两条 + 「查看剩余 N 条」。
 *
 * 数据来自 `ui_message_store`（观察者驱动，自己不订阅总线）。整栈建在 shell_ 上、
 * 与常驻设备信息条同级，所以它压在所有页面之上、又在弹窗那层（`lv_layer_top`）之下。
 *
 * 打印态下这栈要落在 stats-card 上方而不是贴屏底，靠
 * `ui_notice_stack_set_bottom_offset()` 抬起来——首页进出打印态时调一次。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 建栈（单例，重复调用返回同一个）。须在 `ui_message_store_init()` 之后。 */
lv_obj_t *ui_notice_stack_create(lv_obj_t *parent);

/** 按页显隐：规则同常驻设备信息条，另外排除消息页自己。 */
void ui_notice_stack_set_visible(bool visible);

/**
 * 抬高整栈的底边（像素）。默认 0 = 贴屏底内缩一个 USER_PAD。
 * 打印态传 stats-card 的高度 + 间距，让两者上下相接。
 */
void ui_notice_stack_set_bottom_offset(int32_t px);

#ifdef __cplusplus
}
#endif

#endif
