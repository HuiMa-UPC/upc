#include "ui_theme.hpp"

const lv_color_t ui_color_bg         = LV_COLOR_MAKE(0x0d, 0x11, 0x17);
const lv_color_t ui_color_sidebar    = LV_COLOR_MAKE(0x16, 0x1b, 0x22);
const lv_color_t ui_color_panel      = LV_COLOR_MAKE(0x21, 0x26, 0x2d);
const lv_color_t ui_color_panel_alt  = LV_COLOR_MAKE(0x2d, 0x33, 0x3b);
const lv_color_t ui_color_accent     = LV_COLOR_MAKE(0x4d, 0xaa, 0x4d);
const lv_color_t ui_color_accent_dim = LV_COLOR_MAKE(0x3d, 0x8a, 0x3d);
const lv_color_t ui_color_text       = LV_COLOR_MAKE(0xe6, 0xed, 0xf3);
const lv_color_t ui_color_text_dim   = LV_COLOR_MAKE(0x8b, 0x94, 0x9e);
const lv_color_t ui_color_border     = LV_COLOR_MAKE(0x30, 0x36, 0x3d);
const lv_color_t ui_color_danger     = LV_COLOR_MAKE(0xf8, 0x51, 0x49);

static lv_style_t s_nav, s_nav_active, s_panel, s_title, s_sub, s_btn;

void ui_theme_init(void)
{
    lv_style_init(&s_nav);
    lv_style_set_bg_color(&s_nav, ui_color_sidebar);
    lv_style_set_bg_opa(&s_nav, LV_OPA_COVER);
    lv_style_set_border_width(&s_nav, 0);
    lv_style_set_radius(&s_nav, UI_RADIUS);
    lv_style_set_pad_all(&s_nav, UI_PAD_SM);
    lv_style_set_text_color(&s_nav, ui_color_text_dim);
    lv_style_set_text_font(&s_nav, ui_font_body());

    lv_style_init(&s_nav_active);
    lv_style_set_bg_color(&s_nav_active, ui_color_panel_alt);
    lv_style_set_bg_opa(&s_nav_active, LV_OPA_COVER);
    lv_style_set_border_side(&s_nav_active, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_nav_active, 8);
    lv_style_set_border_color(&s_nav_active, ui_color_accent);
    lv_style_set_radius(&s_nav_active, UI_RADIUS);
    lv_style_set_pad_all(&s_nav_active, UI_PAD_SM);
    lv_style_set_text_color(&s_nav_active, ui_color_text);
    lv_style_set_text_font(&s_nav_active, ui_font_body());

    lv_style_init(&s_panel);
    lv_style_set_bg_color(&s_panel, ui_color_panel);
    lv_style_set_bg_opa(&s_panel, LV_OPA_COVER);
    lv_style_set_border_width(&s_panel, 1);
    lv_style_set_border_color(&s_panel, ui_color_border);
    lv_style_set_radius(&s_panel, UI_RADIUS);
    lv_style_set_pad_all(&s_panel, UI_PAD);

    lv_style_init(&s_title);
    lv_style_set_text_color(&s_title, ui_color_text);
    lv_style_set_text_font(&s_title, ui_font_title());

    lv_style_init(&s_sub);
    lv_style_set_text_color(&s_sub, ui_color_text_dim);
    lv_style_set_text_font(&s_sub, ui_font_body());

    lv_style_init(&s_btn);
    lv_style_set_bg_color(&s_btn, ui_color_accent_dim);
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_btn, UI_RADIUS);
    lv_style_set_text_color(&s_btn, ui_color_text);
    lv_style_set_text_font(&s_btn, ui_font_body());
    lv_style_set_pad_hor(&s_btn, 24);
    lv_style_set_pad_ver(&s_btn, 16);
}

void ui_theme_apply_switch(lv_obj_t *sw)
{
    if(!sw) {
        return;
    }
    lv_obj_set_size(sw, UI_SWITCH_W, UI_SWITCH_H);
}

const lv_font_t *ui_font_body(void)  { return &lv_font_montserrat_32; }
const lv_font_t *ui_font_form(void)  { return &lv_font_montserrat_24; }
const lv_font_t *ui_font_title(void) { return &lv_font_montserrat_48; }
const lv_font_t *ui_font_icon(void)  { return &lv_font_montserrat_32; }

lv_style_t *ui_style_nav_item(void)        { return &s_nav; }
lv_style_t *ui_style_nav_item_active(void) { return &s_nav_active; }
lv_style_t *ui_style_panel(void)           { return &s_panel; }
lv_style_t *ui_style_title(void)           { return &s_title; }
lv_style_t *ui_style_subtitle(void)        { return &s_sub; }
lv_style_t *ui_style_btn_primary(void)     { return &s_btn; }
