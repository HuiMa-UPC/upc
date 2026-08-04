#include "ui_feedback.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"

#include <cstdint>

static lv_obj_t *s_toast = nullptr;
static lv_timer_t *s_toast_timer = nullptr;
static lv_timer_t *s_toast_fade_timer = nullptr;

static void toast_hide_now(void)
{
    if(s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_toast, LV_OPA_COVER, 0);
    }
}

static void toast_fade_out_cb(lv_timer_t *t)
{
    if(t) {
        lv_timer_delete(t);
    }
    s_toast_fade_timer = nullptr;
    toast_hide_now();
}

static void toast_dismiss_cb(lv_timer_t *t)
{
    if(t) {
        lv_timer_delete(t);
    }
    s_toast_timer = nullptr;

    if(!s_toast) {
        return;
    }

    if(s_toast_fade_timer) {
        lv_timer_delete(s_toast_fade_timer);
        s_toast_fade_timer = nullptr;
    }

    lv_obj_fade_out(s_toast, 400, 0);
    s_toast_fade_timer = lv_timer_create(toast_fade_out_cb, 450, nullptr);
    lv_timer_set_repeat_count(s_toast_fade_timer, 1);
}

void ui_toast_show(const char *msg, lv_color_t color, uint32_t duration_ms)
{
    ui_toast_show_icon(msg, color, duration_ms, nullptr);
}

void ui_toast_show_icon(const char *msg, lv_color_t color, uint32_t duration_ms,
                        const lv_image_dsc_t *icon)
{
    if(!msg || !msg[0]) {
        return;
    }

    if(s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = nullptr;
    }
    if(s_toast_fade_timer) {
        lv_timer_delete(s_toast_fade_timer);
        s_toast_fade_timer = nullptr;
    }

    /* 顶层图层：toast 始终显示在最上层（含全屏播放器等顶层覆盖之上） */
    lv_obj_t *scr = lv_layer_top();
    if(!s_toast) {
        s_toast = lv_obj_create(scr);
        lv_obj_remove_style_all(s_toast);
        lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_toast, ui_color_panel_alt, 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_toast, ui_color_border, 0);
        lv_obj_set_style_border_width(s_toast, 1, 0);
        lv_obj_set_style_radius(s_toast, UI_RADIUS, 0);
        lv_obj_set_style_pad_hor(s_toast, UI_PAD, 0);
        lv_obj_set_style_pad_ver(s_toast, UI_PAD_SM, 0);
        lv_obj_set_style_pad_column(s_toast, UI_PAD_SM, 0);
        lv_obj_set_style_shadow_width(s_toast, 12, 0);
        lv_obj_set_style_shadow_color(s_toast, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(s_toast, LV_OPA_40, 0);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING);
        /* 行布局承载「可选图标 + 文字」；图标有无因调用而异，故每次重建子节点。 */
        lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    lv_obj_clean(s_toast);
    if(icon) {
        ui_user_make_icon_img(s_toast, icon,
                              lv_font_get_line_height(ui_font_body()), color);
    }
    lv_obj_t *lbl = lv_label_create(s_toast);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    if(icon) {
        /* 带图标时按内容收窄（图标+文字要居中成一条），长文案仍在 480 处折行 */
        lv_obj_set_width(lbl, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(lbl, 480, 0);
    } else {
        lv_obj_set_width(lbl, 480);
    }
    lv_obj_set_style_text_font(lbl, ui_font_body(), 0);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_update_layout(s_toast);

    const int32_t sw = lv_obj_get_width(scr);
    const int32_t sh = lv_obj_get_height(scr);
    const int32_t tw = lv_obj_get_width(s_toast);
    const int32_t th = lv_obj_get_height(s_toast);
    lv_obj_set_pos(s_toast, (sw - tw) / 2, sh - th - 24);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_obj_set_style_opa(s_toast, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(s_toast, 200, 0);

    if(duration_ms < 800) {
        duration_ms = 800;
    }
    s_toast_timer = lv_timer_create(toast_dismiss_cb, duration_ms, nullptr);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}
