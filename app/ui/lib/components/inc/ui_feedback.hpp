#ifndef UI_FEEDBACK_H
#define UI_FEEDBACK_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Toast 切浅色档。由 PageManager 跟着整屏底色一起切（见 apply_shell_style()），
 * 页面自己不用管。
 *
 * **两档的 color 参数含义不同**：深色档里它是文字色；浅色档里文字固定深色，
 * color 只画成左侧那道语义色条。原因是现有调用方传的那几个色（#52ff92 / #ff4242
 * 一类）都是照深底挑的，压在白底上对比度只有 1.3~3.4，当正文根本读不出来。
 */
void ui_toast_set_light(bool on);

/** 底部 Toast 提示（自动消失） */
void ui_toast_show(const char *msg, lv_color_t color, uint32_t duration_ms);

/**
 * 带前置图标的 Toast（图标 recolor 成 color，与文字同色同高）。
 * @param icon A8 图标源，传 NULL 等价于 ui_toast_show()
 */
void ui_toast_show_icon(const char *msg, lv_color_t color, uint32_t duration_ms,
                        const lv_image_dsc_t *icon);

#ifdef __cplusplus
}
#endif

#endif
