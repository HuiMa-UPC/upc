#include "ui_keyboard.hpp"
#include "ui_theme.hpp"
#include "job_status.hpp"

static lv_obj_t *s_kb = nullptr;
static lv_obj_t *s_current_ta = nullptr;

static bool ta_input_allowed(lv_obj_t *ta)
{
    if(!ta) {
        return false;
    }
    if(lv_obj_has_state(ta, LV_STATE_DISABLED)) {
        return false;
    }
    if(!lv_obj_has_flag(ta, LV_OBJ_FLAG_CLICKABLE)) {
        return false;
    }
    JOB_LOCK();
    if(g_job_status.is_sending) {
        return false;
    }
    return true;
}

/** 单行输入框：关闭聚焦自动滚动，避免文字在框内上下跳动 */
static void style_single_line_ta(lv_obj_t *ta)
{
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scroll_dir(ta, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_line_space(ta, 0, 0);

    const lv_font_t *font = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
    if (!font) {
        font = ui_font_body();
        lv_obj_set_style_text_font(ta, font, 0);
    }

    int32_t h = lv_obj_get_height(ta);
    int32_t lh = lv_font_get_line_height(font);
    int32_t border = lv_obj_get_style_border_width(ta, LV_PART_MAIN);
    if (h > 0 && lh > 0) {
        int32_t pad_v = (h - 2 * border - lh) / 2;
        if (pad_v < 2) {
            pad_v = 2;
        }
        lv_obj_set_style_pad_top(ta, pad_v, 0);
        lv_obj_set_style_pad_bottom(ta, pad_v, 0);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (s_current_ta) {
            lv_obj_remove_state(s_current_ta, LV_STATE_FOCUSED);
        }
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        s_current_ta = nullptr;
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if(!ta_input_allowed(ta)) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        if(s_current_ta == ta) {
            ui_keyboard_hide();
        }
        return;
    }

    lv_keyboard_mode_t mode = (lv_keyboard_mode_t)(uintptr_t)lv_event_get_user_data(e);

    s_current_ta = ta;
    lv_keyboard_set_mode(s_kb, mode);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);

    lv_obj_update_layout(s_kb);

    /* 复位内部滚动，避免光标/聚焦时带动文字上下移动 */
    lv_obj_scroll_to(ta, 0, 0, LV_ANIM_OFF);

    /* 仅当输入框会被键盘挡住时才滚动父容器，且不用动画 */
    lv_area_t ta_coords;
    lv_area_t kb_coords;
    lv_obj_get_coords(ta, &ta_coords);
    lv_obj_get_coords(s_kb, &kb_coords);
    if (ta_coords.y2 > kb_coords.y1) {
        lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
    }
}

void ui_keyboard_init(lv_obj_t *parent)
{
    s_kb = lv_keyboard_create(parent);
    lv_obj_set_size(s_kb, LV_PCT(100), LV_PCT(42));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_kb, ui_font_body(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_kb, ui_font_body(), LV_PART_ITEMS);

    /* 监听 OK 与 Close 按钮 */
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);
}

void ui_keyboard_attach(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    style_single_line_ta(ta);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, (void *)(uintptr_t)mode);
}

void ui_keyboard_hide(void)
{
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    s_current_ta = nullptr;
}
