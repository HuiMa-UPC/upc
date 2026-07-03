#include "ui_theme.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ============================================================================
 *  颜色
 * ============================================================================ */
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

const lv_color_t user_color_bg         = LV_COLOR_MAKE(0x0a, 0x09, 0x09);
const lv_color_t user_color_panel      = LV_COLOR_MAKE(0x1c, 0x1c, 0x1c);
const lv_color_t user_color_panel_alt  = LV_COLOR_MAKE(0x26, 0x26, 0x26);
const lv_color_t user_color_accent     = LV_COLOR_MAKE(0x42, 0x58, 0xff);
const lv_color_t user_color_accent_dim = LV_COLOR_MAKE(0x31, 0x44, 0xd6);
const lv_color_t user_color_text       = LV_COLOR_MAKE(0xff, 0xff, 0xff);
const lv_color_t user_color_text_dim   = LV_COLOR_MAKE(0x9a, 0x9a, 0x9a);
const lv_color_t user_color_icon_off   = LV_COLOR_MAKE(0x55, 0x55, 0x55);
const lv_color_t user_color_divider    = LV_COLOR_MAKE(0x33, 0x33, 0x33);
const lv_color_t user_color_status_ok  = LV_COLOR_MAKE(0x52, 0xff, 0x92);
const lv_color_t user_color_status_err = LV_COLOR_MAKE(0xff, 0x4d, 0x4d);

/* ============================================================================
 *  两套 profile 数据
 * ============================================================================ */
static const ui_theme_t s_theme_1920 = {
    /* 开发者 Shell */
    /* ui_sidebar_w           */ 380,
    /* ui_nav_item_h          */ 88,
    /* ui_nav_icon_w          */ 80,
    /* ui_pad                 */ 28,
    /* ui_pad_sm              */ 18,
    /* ui_radius              */ 12,
    /* ui_form_label_w        */ 120,
    /* ui_form_label2_w       */ 96,
    /* ui_gain_row_label_w    */ 120,
    /* ui_gain_row_ta_w       */ 130,
    /* ui_jog_btn_sz          */ 72,
    /* ui_jog_home_sz         */ 64,
    /* ui_jog_step_w          */ 100,
    /* ui_jog_step_btn_h      */ 48,
    /* ui_jog_block_min_h     */ 260,
    /* ui_input_h             */ 72,
    /* ui_ta_ip_w             */ 320,
    /* ui_ta_short_w          */ 152,
    /* ui_btn_conn_w          */ 196,
    /* ui_bar_title_w         */ 148,
    /* ui_bar_value_w         */ 120,
    /* ui_bar_h               */ 28,
    /* ui_indicator_sz        */ 20,
    /* ui_switch_label_w      */ 130,
    /* ui_switch_w            */ 88,
    /* ui_switch_h            */ 44,
    /* ui_settings_row_h      */ 56,
    /* ui_settings_label_w    */ 208,
    /* ui_settings_ta_w       */ 128,
    /* ui_list_item_h         */ 72,
    /* ui_btn_open_w          */ 256,
    /* ui_btn_action_w        */ 168,
    /* ui_btn_job_w           */ 152,
    /* ui_confirm_box_w       */ 640,
    /* ui_confirm_text_w      */ 580,
    /* ui_confirm_btn_w       */ 200,
    /* ui_confirm_btn_h       */ 72,   /* == ui_input_h */
    /* ui_page_anim_ms        */ 320,
    /* ui_page_anim_slide     */ 28,
    /* ui_btn_pad_hor         */ 24,
    /* ui_btn_pad_ver         */ 16,
    /* ui_nav_active_border   */ 8,

    /* 用户 Shell */
    /* user_sidebar_w         */ 120,
    /* user_nav_item_h        */ 96,
    /* user_nav_icon_sz       */ 56,
    /* user_nav_icon_pad      */ 12,
    /* user_topbar_h          */ 120,
    /* user_topbar_pad_hor    */ 40,
    /* user_topbar_pad_ver    */ 32,
    /* user_pad               */ 40,
    /* user_pad_sm            */ 20,
    /* user_pad_lg            */ 64,
    /* user_radius            */ 20,
    /* user_btn_primary_w     */ 220,
    /* user_btn_primary_h     */ 72,
    /* user_btn_door_sz       */ 360,
    /* user_btn_door_icon_sz  */ 100,
    /* user_value_label_w     */ 280,
    /* user_status_dot_sz     */ 18,
    /* user_home_device_w     */ 560,
    /* user_home_device_h     */ 660,
    /* user_back_btn_w        */ 180,
    /* user_back_btn_h        */ 64,

    /* 打印流程 */
    /* user_dlg_w                 */ 720,
    /* user_dlg_pad               */ 40,
    /* user_dlg_pad_lg            */ 56,
    /* user_dlg_title_pad_bottom  */ 24,
    /* user_dlg_radius            */ 28,
    /* user_dlg_body_min_h        */ 260,
    /* user_dlg_btn_ghost_h       */ 56,
    /* user_dlg_footer_btn_w      */ 260,
    /* user_dlg_footer_btn_h      */ 72,
    /* user_file_dlg_w            */ 1100,
    /* user_file_dlg_preview_w    */ 340,
    /* user_file_dlg_preview_h    */ 260,
    /* user_file_sel_w            */ 1500,
    /* user_file_sel_h            */ 780,
    /* user_file_card_w           */ 320,
    /* user_file_card_h           */ 260,
    /* user_file_thumb_h          */ 160,
    /* user_slide_w               */ 520,
    /* user_slide_h               */ 80,
    /* user_slide_knob_sz         */ 68,
    /* user_spinner_sz            */ 160,
    /* user_spinner_stroke        */ 12,
    /* user_progress_w            */ 520,
    /* user_progress_h            */ 20,
    /* user_print_bar_h           */ 140,
    /* user_print_bar_pad_hor     */ 32,
    /* user_print_bar_pad_ver     */ 20,
    /* user_print_bar_gap         */ 32,
    /* user_print_terminate_w     */ 180,
    /* user_print_terminate_h     */ 64,

    /* 字体（dev） */
    /* font_body          */ &lv_font_montserrat_32,
    /* font_form          */ &lv_font_montserrat_24,
    /* font_title         */ &lv_font_montserrat_48,
    /* font_icon          */ &lv_font_montserrat_32,
    /* 字体（user） */
    /* font_user_value    */ &lv_font_montserrat_48,
    /* font_user_text     */ &lv_font_montserrat_24,
    /* font_user_caption  */ &lv_font_montserrat_18,
    /* font_user_icon     */ &lv_font_montserrat_32,

    /* profile_id */ UI_RES_1920,
};

static const ui_theme_t s_theme_1280 = {
    /* 开发者 Shell */
    /* ui_sidebar_w           */ 240,
    /* ui_nav_item_h          */ 64,
    /* ui_nav_icon_w          */ 56,
    /* ui_pad                 */ 18,
    /* ui_pad_sm              */ 10,
    /* ui_radius              */ 8,
    /* ui_form_label_w        */ 80,
    /* ui_form_label2_w       */ 64,
    /* ui_gain_row_label_w    */ 84,
    /* ui_gain_row_ta_w       */ 88,
    /* ui_jog_btn_sz          */ 52,
    /* ui_jog_home_sz         */ 44,
    /* ui_jog_step_w          */ 70,
    /* ui_jog_step_btn_h      */ 36,
    /* ui_jog_block_min_h     */ 176,
    /* ui_input_h             */ 48,
    /* ui_ta_ip_w             */ 220,
    /* ui_ta_short_w          */ 104,
    /* ui_btn_conn_w          */ 132,
    /* ui_bar_title_w         */ 100,
    /* ui_bar_value_w         */ 80,
    /* ui_bar_h               */ 20,
    /* ui_indicator_sz        */ 14,
    /* ui_switch_label_w      */ 96,
    /* ui_switch_w            */ 60,
    /* ui_switch_h            */ 32,
    /* ui_settings_row_h      */ 44,
    /* ui_settings_label_w    */ 148,
    /* ui_settings_ta_w       */ 92,
    /* ui_list_item_h         */ 48,
    /* ui_btn_open_w          */ 172,
    /* ui_btn_action_w        */ 116,
    /* ui_btn_job_w           */ 104,
    /* ui_confirm_box_w       */ 440,
    /* ui_confirm_text_w      */ 400,
    /* ui_confirm_btn_w       */ 140,
    /* ui_confirm_btn_h       */ 48,   /* == ui_input_h */
    /* ui_page_anim_ms        */ 240,
    /* ui_page_anim_slide     */ 20,
    /* ui_btn_pad_hor         */ 16,
    /* ui_btn_pad_ver         */ 10,
    /* ui_nav_active_border   */ 6,

    /* 用户 Shell */
    /* user_sidebar_w         */ 80,
    /* user_nav_item_h        */ 68,
    /* user_nav_icon_sz       */ 40,
    /* user_nav_icon_pad      */ 8,
    /* user_topbar_h          */ 84,
    /* user_topbar_pad_hor    */ 28,
    /* user_topbar_pad_ver    */ 20,
    /* user_pad               */ 28,
    /* user_pad_sm            */ 14,
    /* user_pad_lg            */ 44,
    /* user_radius            */ 14,
    /* user_btn_primary_w     */ 152,
    /* user_btn_primary_h     */ 52,
    /* user_btn_door_sz       */ 248,
    /* user_btn_door_icon_sz  */ 68,
    /* user_value_label_w     */ 188,
    /* user_status_dot_sz     */ 12,
    /* user_home_device_w     */ 380,
    /* user_home_device_h     */ 440,
    /* user_back_btn_w        */ 128,
    /* user_back_btn_h        */ 44,

    /* 打印流程 */
    /* user_dlg_w                 */ 500,
    /* user_dlg_pad               */ 28,
    /* user_dlg_pad_lg            */ 40,
    /* user_dlg_title_pad_bottom  */ 16,
    /* user_dlg_radius            */ 20,
    /* user_dlg_body_min_h        */ 180,
    /* user_dlg_btn_ghost_h       */ 40,
    /* user_dlg_footer_btn_w      */ 180,
    /* user_dlg_footer_btn_h      */ 52,
    /* user_file_dlg_w            */ 760,
    /* user_file_dlg_preview_w    */ 240,
    /* user_file_dlg_preview_h    */ 180,
    /* user_file_sel_w            */ 1040,
    /* user_file_sel_h            */ 540,
    /* user_file_card_w           */ 220,
    /* user_file_card_h           */ 180,
    /* user_file_thumb_h          */ 108,
    /* user_slide_w               */ 360,
    /* user_slide_h               */ 56,
    /* user_slide_knob_sz         */ 48,
    /* user_spinner_sz            */ 108,
    /* user_spinner_stroke        */ 8,
    /* user_progress_w            */ 360,
    /* user_progress_h            */ 14,
    /* user_print_bar_h           */ 96,
    /* user_print_bar_pad_hor     */ 22,
    /* user_print_bar_pad_ver     */ 14,
    /* user_print_bar_gap         */ 22,
    /* user_print_terminate_w     */ 128,
    /* user_print_terminate_h     */ 44,

    /* 字体（dev） */
    /* font_body          */ &lv_font_montserrat_24,
    /* font_form          */ &lv_font_montserrat_18,
    /* font_title         */ &lv_font_montserrat_32,
    /* font_icon          */ &lv_font_montserrat_24,
    /* 字体（user） */
    /* font_user_value    */ &lv_font_montserrat_32,
    /* font_user_text     */ &lv_font_montserrat_18,
    /* font_user_caption  */ &lv_font_montserrat_14,
    /* font_user_icon     */ &lv_font_montserrat_24,

    /* profile_id */ UI_RES_1280,
};


/* ----------------------------------------------------------------------------
 *  1024×600 物理横屏 profile
 *  以 1280 套为基准按 ≈0.8 线性缩小（1024/1280=0.8, 600/720=0.83），
 *  并对触摸关键控件（按钮/输入框高度）做 ~40px 下限保护，保证可点。
 * -------------------------------------------------------------------------- */
static const ui_theme_t s_theme_1024 = {
    /* 开发者 Shell */
    /* ui_sidebar_w           */ 192,
    /* ui_nav_item_h          */ 52,
    /* ui_nav_icon_w          */ 44,
    /* ui_pad                 */ 14,
    /* ui_pad_sm              */ 8,
    /* ui_radius              */ 6,
    /* ui_form_label_w        */ 64,
    /* ui_form_label2_w       */ 52,
    /* ui_gain_row_label_w    */ 68,
    /* ui_gain_row_ta_w       */ 72,
    /* ui_jog_btn_sz          */ 44,
    /* ui_jog_home_sz         */ 40,
    /* ui_jog_step_w          */ 56,
    /* ui_jog_step_btn_h      */ 32,
    /* ui_jog_block_min_h     */ 142,
    /* ui_input_h             */ 40,
    /* ui_ta_ip_w             */ 176,
    /* ui_ta_short_w          */ 84,
    /* ui_btn_conn_w          */ 106,
    /* ui_bar_title_w         */ 80,
    /* ui_bar_value_w         */ 64,
    /* ui_bar_h               */ 16,
    /* ui_indicator_sz        */ 12,
    /* ui_switch_label_w      */ 76,
    /* ui_switch_w            */ 48,
    /* ui_switch_h            */ 26,
    /* ui_settings_row_h      */ 36,
    /* ui_settings_label_w    */ 118,
    /* ui_settings_ta_w       */ 74,
    /* ui_list_item_h         */ 40,
    /* ui_btn_open_w          */ 138,
    /* ui_btn_action_w        */ 94,
    /* ui_btn_job_w           */ 84,
    /* ui_confirm_box_w       */ 352,
    /* ui_confirm_text_w      */ 320,
    /* ui_confirm_btn_w       */ 112,
    /* ui_confirm_btn_h       */ 40,   /* == ui_input_h */
    /* ui_page_anim_ms        */ 220,
    /* ui_page_anim_slide     */ 16,
    /* ui_btn_pad_hor         */ 13,
    /* ui_btn_pad_ver         */ 8,
    /* ui_nav_active_border   */ 5,

    /* 用户 Shell */
    /* user_sidebar_w         */ 80,
    /* user_nav_item_h        */ 72,
    /* user_nav_icon_sz       */ 54,
    /* user_nav_icon_pad      */ 8,
    /* user_topbar_h          */ 68,
    /* user_topbar_pad_hor    */ 22,
    /* user_topbar_pad_ver    */ 16,
    /* user_pad               */ 22,
    /* user_pad_sm            */ 11,
    /* user_pad_lg            */ 36,
    /* user_radius            */ 11,
    /* user_btn_primary_w     */ 122,
    /* user_btn_primary_h     */ 44,
    /* user_btn_door_sz       */ 200,
    /* user_btn_door_icon_sz  */ 54,
    /* user_value_label_w     */ 150,
    /* user_status_dot_sz     */ 10,
    /* user_home_device_w     */ 304,
    /* user_home_device_h     */ 352,
    /* user_back_btn_w        */ 102,
    /* user_back_btn_h        */ 40,

    /* 打印流程 */
    /* user_dlg_w                 */ 400,
    /* user_dlg_pad               */ 22,
    /* user_dlg_pad_lg            */ 32,
    /* user_dlg_title_pad_bottom  */ 12,
    /* user_dlg_radius            */ 16,
    /* user_dlg_body_min_h        */ 144,
    /* user_dlg_btn_ghost_h       */ 36,
    /* user_dlg_footer_btn_w      */ 144,
    /* user_dlg_footer_btn_h      */ 44,
    /* user_file_dlg_w            */ 600,
    /* user_file_dlg_preview_w    */ 192,
    /* user_file_dlg_preview_h    */ 144,
    /* user_file_sel_w            */ 900,   /* < 1024，留边距 */
    /* user_file_sel_h            */ 440,   /* < 600，留边距 */
    /* user_file_card_w           */ 176,
    /* user_file_card_h           */ 144,
    /* user_file_thumb_h          */ 86,
    /* user_slide_w               */ 288,
    /* user_slide_h               */ 48,
    /* user_slide_knob_sz         */ 40,
    /* user_spinner_sz            */ 88,
    /* user_spinner_stroke        */ 6,
    /* user_progress_w            */ 288,
    /* user_progress_h            */ 12,
    /* user_print_bar_h           */ 76,
    /* user_print_bar_pad_hor     */ 18,
    /* user_print_bar_pad_ver     */ 11,
    /* user_print_bar_gap         */ 18,
    /* user_print_terminate_w     */ 102,
    /* user_print_terminate_h     */ 40,

    /* 字体（dev） */
    /* font_body          */ &lv_font_montserrat_20,
    /* font_form          */ &lv_font_montserrat_16,
    /* font_title         */ &lv_font_montserrat_28,
    /* font_icon          */ &lv_font_montserrat_20,
    /* 字体（user） */
    /* font_user_value    */ &lv_font_montserrat_24,
    /* font_user_text     */ &lv_font_montserrat_16,
    /* font_user_caption  */ &lv_font_montserrat_14,
    /* font_user_icon     */ &lv_font_montserrat_20,

    /* profile_id */ UI_RES_1024,
};

/** 当前生效的 profile，默认 1920；ui_theme_init() 中会按实际屏分辨率改指。 */
const ui_theme_t *g_ui = &s_theme_1920;

/* ---- 强制 profile（测试用） ---- */
static int8_t s_forced_profile = 0;

void ui_theme_force_profile(int8_t profile_id)
{
    s_forced_profile = profile_id;
}

int8_t ui_theme_get_profile(void)
{
    return g_ui ? g_ui->profile_id : 0;
}

/* ---- 字体/样式 ---- */
static lv_style_t s_user_btn_primary, s_user_btn_ghost;

static void pick_profile(void)
{
    /* 1) 强制覆盖优先 */
    if(s_forced_profile == UI_RES_1920) {
        g_ui = &s_theme_1920;
        return;
    }
    if(s_forced_profile == UI_RES_1280) {
        g_ui = &s_theme_1280;
        return;
    }
    if(s_forced_profile == UI_RES_1024) {
        g_ui = &s_theme_1024;
        return;
    }

    /* 2) 环境变量覆盖（方便板上 SSH 调试不重编） */
    const char *env = std::getenv("LVGL_UPC_PROFILE");
    if(env && env[0]) {
        if(std::strcmp(env, "1") == 0) {
            g_ui = &s_theme_1920;
            return;
        }
        if(std::strcmp(env, "2") == 0) {
            g_ui = &s_theme_1280;
            return;
        }
        if(std::strcmp(env, "3") == 0) {
            g_ui = &s_theme_1024;
            return;
        }
    }

    /* 3) 默认按当前 display 的最长边自动挑：
     *      ≥1600 → 1920 套尺寸
     *      ≥1100 → 1280 套尺寸
     *      <1100 → 1024 套尺寸（1024×600 物理横屏）
     *    选最长边而不是宽度，是为了兼容竖屏/旋转还未生效的极端情形。 */
    lv_display_t *disp = lv_display_get_default();
    int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 1920;
    int32_t h = disp ? lv_display_get_vertical_resolution(disp)   : 1080;
    int32_t max_dim = w > h ? w : h;

        if(max_dim >= 1600) {
        g_ui = &s_theme_1920;
    } else if(max_dim >= 1100) {
        g_ui = &s_theme_1280;
    } else {
        g_ui = &s_theme_1024;
    }

    const char *name = (g_ui->profile_id == UI_RES_1920) ? "1920"
                     : (g_ui->profile_id == UI_RES_1280) ? "1280"
                                                         : "1024";

    std::fprintf(stderr,
        "ui_theme: detected %dx%d → profile %s\n",
        (int)w, (int)h,name);
}

void ui_theme_init(void)
{
    pick_profile();

    lv_style_init(&s_user_btn_primary);
    lv_style_set_bg_color(&s_user_btn_primary, user_color_accent);
    lv_style_set_bg_opa(&s_user_btn_primary, LV_OPA_COVER);
    lv_style_set_radius(&s_user_btn_primary, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_user_btn_primary, 0);
    lv_style_set_text_color(&s_user_btn_primary, user_color_text);
    lv_style_set_text_font(&s_user_btn_primary, user_font_text());
    lv_style_set_shadow_width(&s_user_btn_primary, 24);
    lv_style_set_shadow_color(&s_user_btn_primary, user_color_accent);
    lv_style_set_shadow_opa(&s_user_btn_primary, LV_OPA_30);
    lv_style_set_pad_hor(&s_user_btn_primary, USER_PAD);
    lv_style_set_pad_ver(&s_user_btn_primary, USER_PAD_SM);

    lv_style_init(&s_user_btn_ghost);
    lv_style_set_bg_color(&s_user_btn_ghost, user_color_panel);
    lv_style_set_bg_opa(&s_user_btn_ghost, LV_OPA_COVER);
    lv_style_set_radius(&s_user_btn_ghost, USER_RADIUS);
    lv_style_set_border_width(&s_user_btn_ghost, 1);
    lv_style_set_border_color(&s_user_btn_ghost, user_color_divider);
    lv_style_set_text_color(&s_user_btn_ghost, user_color_text);
    lv_style_set_text_font(&s_user_btn_ghost, user_font_text());
    lv_style_set_shadow_width(&s_user_btn_ghost, 0);
    lv_style_set_pad_hor(&s_user_btn_ghost, USER_PAD);
    lv_style_set_pad_ver(&s_user_btn_ghost, USER_PAD_SM);
}

const lv_font_t *ui_font_body(void)        { return g_ui->font_body; }
const lv_font_t *ui_font_title(void)       { return g_ui->font_title; }
const lv_font_t *user_font_value(void)     { return g_ui->font_user_value; }
const lv_font_t *user_font_text(void)      { return g_ui->font_user_text; }
const lv_font_t *user_font_caption(void)   { return g_ui->font_user_caption; }
const lv_font_t *user_font_icon(void)      { return g_ui->font_user_icon; }

lv_style_t *user_style_btn_primary(void)   { return &s_user_btn_primary; }
lv_style_t *user_style_btn_ghost(void)     { return &s_user_btn_ghost; }
