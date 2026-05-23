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

/* 横屏控件尺寸（按 1920x1080 适配，相对 1024x600 放大约 1.75x） */
#define UI_SIDEBAR_W        320
#define UI_NAV_ITEM_H       72
#define UI_PAD              20
#define UI_PAD_SM           14
#define UI_RADIUS           10
/** 表单标签列宽（Connection / Control 对齐用） */
#define UI_FORM_LABEL_W     96
#define UI_FORM_LABEL2_W    76
/** G-code 解析增益行（左对齐标签，避免右对齐裁切） */
#define UI_GAIN_LABEL_W     88
#define UI_GAIN_TA_W        180
#define UI_INPUT_H          56
#define UI_TA_IP_W          260
#define UI_TA_SHORT_W       120
#define UI_BTN_CONN_W       156
/** 进度条行左侧标题宽度（避免 CmdCache/DataCache 换行） */
#define UI_BAR_TITLE_W      120
#define UI_BAR_VALUE_W      96

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
