#ifndef PAGE_USER_CONTROL_H
#define PAGE_USER_CONTROL_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file page_user_control.hpp
 * @brief 控制页（稿 5782:12911 主面板 / 5814:16936 运动控制 / 5815:17390 设备控制
 *        / 5788:14284+14635 舱内 PM2.5 弹窗 / 5788:15012 激光功率弹窗）。
 *
 * 一个 Page 两个视图，靠显隐切换而**不是**两个 Page：
 *   主面板  ← 「More Controls」→  更多控制（Motion / Device 两 Tab）
 *
 * 之所以不把「更多控制」做成隐藏页：`PageManager::apply_device_bar()` 按「侧栏有
 * 菜单按钮」判显隐，隐藏页会连常驻设备条一起隐掉并把 `pad_top` 收回 0——而稿
 * 5814:16936 / 5815:17390 顶上明明画着设备条。做成页内视图则设备条与侧栏天然照稿
 * 保留，`ui_app.cpp` 也不必新注册一个页。代价是右上角的通知浮层会压住传感器条，
 * 由本页在切视图时用 `ui_notice_stack_set_visible()` 自己关开（见 .cpp）。
 */
lv_obj_t *page_user_control_create(lv_obj_t *parent);

void page_user_control_refresh(bool force);

/**
 * 离页收尾。**必须注册到 SimplePage**，两件事都不能省：
 *   1. 清掉 `DebugMsgProxy` 的 sensor/axis/device 回调——它们是**单槽**的，调试页
 *      也在用（`page_user_debug_exit()` 同样清），不清则从本页切去调试页后那边的
 *      传感器条不再刷新；
 *   2. 把通知浮层的显隐还给 `apply_device_bar()` 的默认规则。
 */
void page_user_control_exit(void);

#ifdef __cplusplus
}
#endif

#endif
