#include "ui_user_skeleton.hpp"
#include "ui_user_widgets.hpp"
#include "ui_theme.hpp"

lv_obj_t *ui_user_skeleton_create(lv_obj_t *parent,
                                  const char *title,
                                  const char *hint)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, USER_PAD, 0);
    lv_obj_set_style_pad_row(page, USER_PAD, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(page);
    lv_label_set_text(title_lbl, title ? title : "");
    lv_obj_set_style_text_color(title_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(title_lbl, user_font_value(), 0);

    lv_obj_t *card = ui_user_make_frosted_card(page, true);
    lv_obj_t *hint_lbl = lv_label_create(card);
    lv_label_set_text(hint_lbl, hint ? hint : "");
    lv_obj_set_style_text_color(hint_lbl, user_color_text_dim, 0);
    lv_obj_set_style_text_font(hint_lbl, user_font_text(), 0);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hint_lbl);

    return page;
}
