#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* KlipperScreen 风格配色 */
extern const lv_color_t ui_color_bg;
extern const lv_color_t ui_color_sidebar;
extern const lv_color_t ui_color_panel;
extern const lv_color_t ui_color_panel_alt;
extern const lv_color_t ui_color_accent;
extern const lv_color_t ui_color_accent_dim;
extern const lv_color_t ui_color_text;
extern const lv_color_t ui_color_text_dim;
extern const lv_color_t ui_color_border;
extern const lv_color_t ui_color_danger;

/* 横屏大控件尺寸（按 1920x1080 设计，小屏可缩放） */
#define UI_SIDEBAR_W        280
#define UI_NAV_ITEM_H       96
#define UI_PAD              20
#define UI_PAD_SM           12
#define UI_RADIUS           10

void ui_theme_init(void);

const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_title(void);
const lv_font_t *ui_font_icon(void);

lv_style_t *ui_style_nav_item(void);
lv_style_t *ui_style_nav_item_active(void);
lv_style_t *ui_style_panel(void);
lv_style_t *ui_style_title(void);
lv_style_t *ui_style_subtitle(void);
lv_style_t *ui_style_btn_primary(void);

#ifdef __cplusplus
}
#endif

#endif
