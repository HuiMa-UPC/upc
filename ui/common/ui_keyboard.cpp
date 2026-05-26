#include "ui_keyboard.hpp"
#include "ui_theme.hpp"

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
    return true;
}

static void show_keyboard_for_ta(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    if(!s_kb || !ta || !ta_input_allowed(ta)) {
        return;
    }

    s_current_ta = ta;
    lv_keyboard_set_mode(s_kb, mode);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);

    /*
     * 等当前触摸抬起后再处理下一次点击，避免抬起落在刚弹出的键盘上导致输入框失焦。
     * 与 LVGL demos/widgets ta_event_cb 一致。
     */
    lv_indev_t *indev = lv_indev_active();
    if(indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_indev_wait_release(indev);
    }
}

/** 单行输入框：关闭聚焦自动滚动，避免文字在框内上下跳动 */
static void style_single_line_ta(lv_obj_t *ta)
{
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scroll_dir(ta, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_line_space(ta, 0, 0);

    const lv_font_t *font = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
    if(!font) {
        font = ui_font_body();
        lv_obj_set_style_text_font(ta, font, 0);
    }

    int32_t h = lv_obj_get_height(ta);
    int32_t lh = lv_font_get_line_height(font);
    int32_t border = lv_obj_get_style_border_width(ta, LV_PART_MAIN);
    if(h > 0 && lh > 0) {
        int32_t pad_v = (h - 2 * border - lh) / 2;
        if(pad_v < 2) {
            pad_v = 2;
        }
        lv_obj_set_style_pad_top(ta, pad_v, 0);
        lv_obj_set_style_pad_bottom(ta, pad_v, 0);
    }
}

static void hide_keyboard_core(lv_obj_t *ta)
{
    if(s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, nullptr);
    }
    if(ta) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        lv_indev_reset(nullptr, ta);
    }
    s_current_ta = nullptr;
}

static void kb_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        hide_keyboard_core(s_current_ta);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_FOCUSED) {
        return;
    }

    lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const lv_keyboard_mode_t mode =
        static_cast<lv_keyboard_mode_t>((uintptr_t)lv_event_get_user_data(e));

    if(!ta_input_allowed(ta)) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        if(s_current_ta == ta) {
            hide_keyboard_core(ta);
        }
        return;
    }

    show_keyboard_for_ta(ta, mode);
}

/*
 * 键盘打开期间若被其它逻辑误触发 DEFOCUSED，立即恢复焦点，避免键盘闪退。
 * 真正关闭仅通过 OK/Cancel、ui_keyboard_hide 或禁用输入框。
 */
static void ta_defocus_guard_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_DEFOCUSED) {
        return;
    }

    lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if(s_current_ta == ta && ui_keyboard_is_open()) {
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
    }
}

void ui_keyboard_init(lv_obj_t *parent)
{
    s_kb = lv_keyboard_create(parent);
    lv_obj_set_size(s_kb, LV_PCT(100), LV_PCT(42));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_font(s_kb, ui_font_body(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_kb, ui_font_body(), LV_PART_ITEMS);

    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, nullptr);
}

void ui_keyboard_attach(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    style_single_line_ta(ta);
    void *ud = reinterpret_cast<void *>(static_cast<uintptr_t>(mode));
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, ud);
    lv_obj_add_event_cb(ta, ta_defocus_guard_cb, LV_EVENT_DEFOCUSED, nullptr);
}

void ui_keyboard_hide(void)
{
    hide_keyboard_core(s_current_ta);
}

void ui_keyboard_release_if_focused(lv_obj_t *ta)
{
    if(!ta || s_current_ta != ta) {
        return;
    }
    hide_keyboard_core(ta);
}

bool ui_keyboard_is_bound(const lv_obj_t *ta)
{
    if(!ta || !s_kb || s_current_ta != ta) {
        return false;
    }
    return !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

bool ui_keyboard_is_open(void)
{
    return s_kb && s_current_ta != nullptr &&
           !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}
