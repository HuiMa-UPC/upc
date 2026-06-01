#include "ui_modal_dialog.hpp"
#include "ui_theme.hpp"

#include <cstdlib>

/* 屏蔽点击穿透：遮罩自己就是按钮，但点击不做任何事。 */
static void backdrop_eat_click_cb(lv_event_t *e)
{
    lv_event_stop_processing(e);
}

ui_modal_dialog_t *ui_modal_dialog_open(lv_obj_t *parent,
                                        const ui_modal_dialog_cfg_t *cfg)
{
    if(!parent || !cfg) {
        return nullptr;
    }

    auto *dlg = static_cast<ui_modal_dialog_t *>(
        std::calloc(1, sizeof(ui_modal_dialog_t)));
    if(!dlg) {
        return nullptr;
    }

    /* ---- 背景遮罩 ---- */
    lv_obj_t *backdrop = lv_obj_create(parent);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, USER_DLG_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    /* 让遮罩自身可点（吞掉穿透到底层 home 的事件） */
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, backdrop_eat_click_cb, LV_EVENT_CLICKED, nullptr);
    dlg->root = backdrop;

    /* ---- 弹窗主体卡片 ---- */
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    const int32_t w = cfg->card_width > 0 ? cfg->card_width : USER_DLG_W;
    lv_obj_set_width(card, w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_DLG_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_shadow_width(card, 60, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(card, USER_DLG_PAD, 0);
    lv_obj_set_style_pad_row(card, USER_DLG_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    /* ---- 标题 ---- */
    if(cfg->title_text && cfg->title_text[0]) {
        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, cfg->title_text);
        lv_obj_set_style_text_color(title, user_color_text, 0);
        lv_obj_set_style_text_font(title, user_font_value(), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(title, LV_PCT(100));
        dlg->title = title;

        if(cfg->accent_underline) {
            lv_obj_t *line = lv_obj_create(card);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, USER_PAD_LG, 4);
            lv_obj_set_style_bg_color(line, user_color_accent, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(line, 2, 0);
            lv_obj_set_style_margin_bottom(line, USER_PAD_SM, 0);
        }
    }

    /* ---- body slot ---- */
    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, USER_PAD, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    dlg->body = body;

    /* ---- footer slot ---- */
    if(cfg->with_footer) {
        lv_obj_t *footer = lv_obj_create(card);
        lv_obj_remove_style_all(footer);
        lv_obj_set_width(footer, LV_PCT(100));
        lv_obj_set_height(footer, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_top(footer, USER_PAD_SM, 0);
        lv_obj_set_style_pad_column(footer, USER_PAD, 0);
        lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
        dlg->footer = footer;
    }

    /* 弹窗刚出现时做一个 fade-in，避免突兀 */
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(backdrop, 180, 0);
    lv_obj_move_foreground(backdrop);

    return dlg;
}

void ui_modal_dialog_close(ui_modal_dialog_t *dlg)
{
    if(!dlg) {
        return;
    }
    if(dlg->root) {
        lv_obj_delete(dlg->root);
    }
    std::free(dlg);
}

void ui_modal_dialog_set_title(ui_modal_dialog_t *dlg, const char *text)
{
    if(!dlg || !dlg->title) {
        return;
    }
    lv_label_set_text(dlg->title, text ? text : "");
}
