#ifndef UI_NOTICE_STACK_H
#define UI_NOTICE_STACK_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_notice_stack.hpp
 * @brief 屏幕右上角的通知浮层：消息中心里最要紧的两条 + 「查看剩余 N 条」。
 *
 * 数据来自 `ui_message_store`（观察者驱动，自己不订阅总线）。整栈建在 shell_ 上、
 * 与常驻设备信息条同级，所以它压在所有页面之上、又在弹窗那层（`lv_layer_top`）之下。
 *
 * 位置按稿 5277:11778 贴右上角，右缘退一个 USER_PAD——与首页打印态/监控态右上角那块
 * 环境区（`build_env_wrap`）同一个退让量，两者上下相接看着是一栏。这个退让量对所有
 * 页面、所有状态都一样，不做按页特例。
 *
 * 右上角本来就有东西的页面分两种处理：首页打印态/监控态是环境区，用
 * `ui_notice_stack_set_top_offset()` 把整栈压到它下面；历史页与设置页那个位置是
 * Storage 容量条和 Wi-Fi 开关，直接整栈不显示（见 `apply_device_bar()`）。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 建栈（单例，重复调用返回同一个）。须在 `ui_message_store_init()` 之后。 */
lv_obj_t *ui_notice_stack_create(lv_obj_t *parent);

/** 按页显隐：规则同常驻设备信息条，另外排除消息页自己、历史页与设置页。 */
void ui_notice_stack_set_visible(bool visible);

/**
 * 把整栈往下让（像素）。默认 0 = 贴屏顶内缩一个 USER_PAD。
 * 首页打印态/监控态传右上角环境区的高度 + 间距，让两者上下相接。
 */
void ui_notice_stack_set_top_offset(int32_t px);

#ifdef __cplusplus
}
#endif

#endif
