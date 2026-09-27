#ifndef PAGE_USER_DEBUG_H
#define PAGE_USER_DEBUG_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 调试页（开发者/工厂手动控制台）。
 * 当前为占位骨架：仅返回按钮 + 占位文字；
 * 完整内容（传感器条 / 设备行 / 移动控制 / 其他控制）见后续 P2。
 * 入口：设置页 About 连点 Software Version 三次 → 密码（"888"）→ 左栏出现 Debug →
 * switch_to("user_debug")。解锁仅本次运行有效。
 */
lv_obj_t *page_user_debug_create(lv_obj_t *parent);

void page_user_debug_refresh(bool force);

void page_user_debug_exit(void);

#ifdef __cplusplus
}
#endif

#endif
