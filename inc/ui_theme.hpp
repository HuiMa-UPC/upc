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

/*
 * 5.5 寸触屏（约 1920×1080 横屏）尺寸基准。
 * 相对旧版约放大 1.35×：更大字体、更高触控目标，标签列宽预留英文单行。
 */
#define UI_SIDEBAR_W        380
#define UI_NAV_ITEM_H       88
#define UI_NAV_ICON_W       80
#define UI_PAD              28
#define UI_PAD_SM           18
#define UI_RADIUS           12
/** 表单标签列宽（Connection / Control 对齐用） */
#define UI_FORM_LABEL_W     120
#define UI_FORM_LABEL2_W    96
/** G-code 解析增益行（左对齐标签） */
#define UI_GAIN_LABEL_W     112
#define UI_GAIN_TA_W        220
#define UI_INPUT_H          72
#define UI_TA_IP_W          320
#define UI_TA_SHORT_W       152
#define UI_BTN_CONN_W       196
/** 进度条行左侧标题宽度（TCP TX / Cal Apply 等单行） */
#define UI_BAR_TITLE_W       148
#define UI_BAR_VALUE_W        120
#define UI_BAR_H              28
#define UI_INDICATOR_SZ       20
/** 开关行标签宽（Enable Layer Change / Enable Calibration 单行） */
#define UI_SWITCH_LABEL_W     248
#define UI_SWITCH_W           88
#define UI_SWITCH_H           44
/** Settings 参数名标签列 */
#define UI_SETTINGS_LABEL_W   168
#define UI_LIST_ITEM_H        72
#define UI_BTN_OPEN_W         256
#define UI_BTN_ACTION_W       168
#define UI_BTN_JOB_W          148

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

/** 统一开关尺寸，便于触控 */
void ui_theme_apply_switch(lv_obj_t *sw);

#ifdef __cplusplus
}
#endif

#endif
