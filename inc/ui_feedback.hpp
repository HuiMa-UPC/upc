#ifndef UI_FEEDBACK_H
#define UI_FEEDBACK_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 底部 Toast 提示（自动消失） */
void ui_toast_show(const char *msg, lv_color_t color, uint32_t duration_ms);

/** 设置状态条文字与颜色（页面内常驻反馈） */
void ui_status_set(lv_obj_t *lbl, const char *msg, lv_color_t color);

#ifdef __cplusplus
}
#endif

#endif
