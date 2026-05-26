#include "ui_feedback.hpp"
#include "ui_theme.hpp"

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

    lv_obj_t *scr = lv_screen_active();
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
        lv_obj_set_style_shadow_width(s_toast, 12, 0);
        lv_obj_set_style_shadow_color(s_toast, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(s_toast, LV_OPA_40, 0);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING);

        lv_obj_t *lbl = lv_label_create(s_toast);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(lbl, 480);
        lv_obj_set_style_text_font(lbl, ui_font_body(), 0);
        lv_obj_set_user_data(s_toast, lbl);
    }

    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(s_toast);
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

void ui_status_set(lv_obj_t *lbl, const char *msg, lv_color_t color)
{
    if(!lbl) {
        return;
    }
    lv_label_set_text(lbl, msg ? msg : "");
    lv_obj_set_style_text_color(lbl, color, 0);
}

void ui_action_btn_set_state(lv_obj_t *btn, bool visible, bool enabled,
                              const lv_color_t *enabled_bg)
{
    if(!btn) {
        return;
    }
    if(visible) {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    if(enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        if(enabled_bg) {
            lv_obj_set_style_bg_color(btn, *enabled_bg, 0);
        }
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        if(enabled_bg) {
            lv_obj_set_style_bg_color(btn, ui_color_panel_alt, 0);
        }
    }
}

static lv_obj_t *s_confirm_box = nullptr;
static ui_confirm_cb s_confirm_cb = nullptr;
static ui_confirm_cb s_cancel_cb = nullptr;
static void *s_confirm_user = nullptr;

static void confirm_dialog_finish(bool confirmed)
{
    ui_confirm_cb cb = confirmed ? s_confirm_cb : s_cancel_cb;
    void *user = s_confirm_user;

    s_confirm_cb = nullptr;
    s_cancel_cb = nullptr;
    s_confirm_user = nullptr;

    if(s_confirm_box) {
        lv_msgbox_close(s_confirm_box);
        s_confirm_box = nullptr;
    }

    if(cb) {
        cb(user);
    }
}

static void confirm_btn_click_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const bool confirmed = lv_event_get_user_data(e) != nullptr;
    confirm_dialog_finish(confirmed);
}

static void confirm_style_footer_btn(lv_obj_t *btn)
{
    lv_obj_set_size(btn, UI_CONFIRM_BTN_W, UI_CONFIRM_BTN_H);
    lv_obj_set_style_text_font(btn, ui_font_body(), 0);
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(btn, UI_PAD, 0);
}

void ui_confirm_show(const char *title, const char *message,
                     const char *confirm_text, const char *cancel_text,
                     ui_confirm_cb on_confirm, ui_confirm_cb on_cancel,
                     void *user_data)
{
    if(s_confirm_box) {
        lv_msgbox_close(s_confirm_box);
        s_confirm_box = nullptr;
    }

    s_confirm_cb = on_confirm;
    s_cancel_cb = on_cancel;
    s_confirm_user = user_data;

    s_confirm_box = lv_msgbox_create(nullptr);
    lv_obj_set_width(s_confirm_box, UI_CONFIRM_BOX_W);
    lv_obj_set_style_pad_all(s_confirm_box, UI_PAD, 0);
    lv_obj_set_style_pad_row(s_confirm_box, UI_PAD_SM, 0);

    lv_msgbox_add_title(s_confirm_box, title ? title : "Confirm");
    lv_obj_t *title_lbl = lv_msgbox_get_title(s_confirm_box);
    if(title_lbl) {
        lv_obj_set_style_text_font(title_lbl, ui_font_title(), 0);
    }

    if(message && message[0] != '\0') {
        lv_obj_t *txt = lv_msgbox_add_text(s_confirm_box, message);
        lv_obj_set_style_text_font(txt, ui_font_body(), 0);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(txt, UI_CONFIRM_TEXT_W);
    }

    lv_obj_t *content = lv_msgbox_get_content(s_confirm_box);
    if(content) {
        lv_obj_set_style_pad_all(content, UI_PAD_SM, 0);
    }

    lv_obj_t *btn_cancel = lv_msgbox_add_footer_button(
        s_confirm_box, cancel_text ? cancel_text : "Cancel");
    confirm_style_footer_btn(btn_cancel);
    lv_obj_add_event_cb(btn_cancel, confirm_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_ok = lv_msgbox_add_footer_button(
        s_confirm_box, confirm_text ? confirm_text : "OK");
    confirm_style_footer_btn(btn_ok);
    lv_obj_add_event_cb(btn_ok, confirm_btn_click_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(1)));
    lv_obj_set_style_bg_color(btn_ok, ui_color_danger, 0);
    lv_obj_set_style_bg_color(btn_ok, ui_color_danger, LV_STATE_PRESSED);

    lv_obj_t *footer = lv_msgbox_get_footer(s_confirm_box);
    if(footer) {
        lv_obj_set_style_pad_all(footer, UI_PAD_SM, 0);
        lv_obj_set_style_pad_column(footer, UI_PAD, 0);
    }
}
