#include "ui_feedback.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "home_metric_icons.h"

#include <cstdint>

static lv_obj_t *s_toast = nullptr;
static lv_timer_t *s_toast_timer = nullptr;
static lv_timer_t *s_toast_fade_timer = nullptr;
static int32_t s_content_left = 0;   /**< 见 ui_toast_set_content_left */

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

void ui_toast_set_content_left(int32_t px)
{
    s_content_left = px > 0 ? px : 0;
}

/** 一类 toast 的图标圆底色、文字色与白色字形（稿 6448:54033 / 54043 / 54059）。 */
struct ToastLook {
    lv_color_t dot;
    lv_color_t text;
    const lv_image_dsc_t *const *glyph;
};

static ToastLook toast_look(ui_toast_type_t type)
{
    switch(type) {
    case UI_TOAST_ERROR:
        /* 稿上圆是 #FF3131、字是 #FF4242，差一点点，照稿分开 */
        return { user_toast_err_dot, user_color_status_err, ic_toast_err_set };
    case UI_TOAST_INFORM:
        return { user_toast_inform, user_toast_inform, ic_toast_warn_set };
    case UI_TOAST_SUCCESS:
    default:
        return { user_light_accent, user_light_accent, ic_toast_ok_set };
    }
}

/** 气泡本体。三类 toast 是同一副版式：白底 + 1px #333145 5% 描边 + 圆角 16 +
 *  0 2 6 黑 4% 投影，内距上下 16、左右 32，图标与文字间距 8。
 *
 *  改版前分深浅两档、每次弹出都要按当前档重刷样式；新稿只剩这一档（全屏视频页上
 *  白卡同样清楚），所以建的时候给一次就够了。 */
static void toast_create(lv_obj_t *scr)
{
    s_toast = lv_obj_create(scr);
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING);
    /* 不吃触摸：挪到顶部之后它会压在 Tab 行、Manage 这类可点控件上，挡住的那一两秒
     * 点不动会被当成卡死。 */
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    /* 行布局承载「状态图标 + 文字」。 */
    lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_bg_color(s_toast, user_light_card, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, user_light_text, 0);
    lv_obj_set_style_border_opa(s_toast, 13, 0);                 /* 稿 5% */
    lv_obj_set_style_radius(s_toast, USER_HIST_CARD_RADIUS, 0);  /* 稿 16 */
    lv_obj_set_style_shadow_width(s_toast, 6, 0);
    lv_obj_set_style_shadow_ofs_y(s_toast, 2, 0);
    lv_obj_set_style_shadow_color(s_toast, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_toast, 10, 0);                 /* 稿 4% ≈ 10/255 */
    lv_obj_set_style_pad_ver(s_toast, USER_PAD_SM, 0);           /* 稿 16 */
    lv_obj_set_style_pad_hor(s_toast, USER_PAD_LG, 0);           /* 稿 32 */
    lv_obj_set_style_pad_column(s_toast, USER_PAD_SM / 2, 0);    /* 稿 8 */
}

void ui_toast_show(const char *msg, ui_toast_type_t type, uint32_t duration_ms)
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
        toast_create(scr);
    }

    /* 图标底色、字形与文字色都随类型变，子节点每次重建。 */
    lv_obj_clean(s_toast);
    const ToastLook look = toast_look(type);

    /* 实色圆 + 白色字形（稿上的组件「状态icon-顶部toast」，20 见方）。三档按
     * USER_METRIC_ICON_SZ * 5/8 折算成 30/20/16，与字形的生成尺寸一一对应、1:1 不缩放。 */
    const int32_t icon_sz = USER_METRIC_ICON_SZ * 5 / 8;
    lv_obj_t *dot = lv_obj_create(s_toast);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, icon_sz, icon_sz);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, look.dot, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(ui_user_make_icon_img(dot, ui_user_pick_icon(look.glyph, icon_sz),
                                        icon_sz, lv_color_white()));

    lv_obj_t *lbl = lv_label_create(s_toast);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    /* 按内容收窄、长文案仍在 480 处折行。定宽 480 会让 "Wi-Fi Off" 这种短提示
     * 撑出一条大半屏的空条，文字孤零零贴在左边。 */
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, 480, 0);
    /* 稿 SemiBold 18 / 行高 24；字库只有一档字重，落地为 user_font_text()（1280 档 18）。 */
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_set_style_text_color(lbl, look.text, 0);
    lv_label_set_text(lbl, msg);

    lv_obj_update_layout(s_toast);

    /* 贴顶 USER_HOME_EDGE_GAP（稿 48）；横向在内容区里居中——有侧栏的页是侧栏右侧那一段
     * （1280 档中心 x=704），全屏页是整屏，见 ui_toast_set_content_left()。 */
    const int32_t sw = lv_obj_get_width(scr);
    const int32_t left = (s_content_left < sw) ? s_content_left : 0;
    const int32_t tw = lv_obj_get_width(s_toast);
    lv_obj_set_pos(s_toast, left + (sw - left - tw) / 2, USER_HOME_EDGE_GAP);
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
