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

/** 设置操作按钮可见/可用；enabled_bg 非 NULL 时同步背景色 */
void ui_action_btn_set_state(lv_obj_t *btn, bool visible, bool enabled,
                              const lv_color_t *enabled_bg);

typedef void (*ui_confirm_cb)(void *user_data);

/** 模态二次确认；on_confirm 在用户点确认后调用，on_cancel 可选 */
void ui_confirm_show(const char *title, const char *message,
                     const char *confirm_text, const char *cancel_text,
                     ui_confirm_cb on_confirm, ui_confirm_cb on_cancel,
                     void *user_data);

#ifdef __cplusplus
}
#endif

#endif
