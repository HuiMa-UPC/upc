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
