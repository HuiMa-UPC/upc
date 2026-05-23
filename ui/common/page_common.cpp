#include "page_common.hpp"
#include "ui_theme.hpp"

lv_obj_t *ui_make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, ui_style_panel(), 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, UI_PAD, 0);
    lv_obj_set_style_pad_row(card, UI_PAD_SM, 0);

    if(title) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, title);
        lv_obj_add_style(lbl, ui_style_subtitle(), 0);
    }
    return card;
}

lv_obj_t *page_common_create_placeholder(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(page);
    lv_obj_add_style(panel, ui_style_panel(), 0);
    lv_obj_set_width(panel, LV_PCT(72));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, UI_PAD, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_title = lv_label_create(panel);
    lv_label_set_text(lbl_title, title);
    lv_obj_add_style(lbl_title, ui_style_title(), 0);

    lv_obj_t *lbl_hint = lv_label_create(panel);
    lv_label_set_text(lbl_hint, hint);
    lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(lbl_hint, ui_style_subtitle(), 0);

    return page;
}
