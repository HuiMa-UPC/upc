#ifndef UI_FEEDBACK_H
#define UI_FEEDBACK_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

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
