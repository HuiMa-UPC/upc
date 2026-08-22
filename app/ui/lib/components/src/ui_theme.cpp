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
 *
 *  user_topbar_h 取屏高的 16%：稿 3540:14473（1280×800）里状态栏那条带子等效高
 *  128（设备信息行 y=48 高 32、两颗按钮 y=32 高 64，三者中心都落在 y=64）。
 *  1024×600 档按 600/800 折算 96，1920 档按 1280 档 ×1.5 给 192。
 *  调试页那条传感器卡原先借用本字段，band 抬高后改用 user_sensor_strip_h，
 *  值即旧的 user_topbar_h，排版不受影响。
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
    /* user_nav_item_sz       */ 80,   /* == 旧的 sidebar_w - 2*pad_sm，观感不变 */
    /* user_nav_gap           */ 100,  /* 复现旧 SPACE_EVENLY 在 1080 高下的项距 180 */
    /* user_nav_item_h        */ 96,
    /* user_settings_nav_w    */ 0,    /* 0 = 沿用旧式，本档观感不变 */
    /* user_settings_gap      */ 0,
    /* user_settings_gutter_r */ 0,
    /* user_settings_seg_h    */ 0,
    /* user_nav_icon_sz       */ 56,
    /* user_nav_icon_pad      */ 12,
    /* user_topbar_h          */ 192,
    /* user_topbar_pad_hor    */ 40,
    /* user_topbar_pad_ver    */ 32,
    /* user_pad               */ 40,
    /* user_pad_sm            */ 20,
    /* user_pad_lg            */ 64,
    /* user_radius            */ 20,
    /* user_btn_radius        */ 12,
    /* user_btn_primary_w     */ 220,
    /* user_btn_primary_h     */ 72,
    /* user_btn_door_sz       */ 360,
    /* user_btn_door_icon_sz  */ 100,
    /* user_value_label_w     */ 280,
    /* user_status_dot_sz     */ 18,
    /* user_home_device_w     */ 560,
    /* user_home_device_h     */ 660,
    /* user_home_action_row_w */ 636,
    /* user_home_card_w       */ 540,  /* == 旧的 LV_PCT(30) × 内容区 1800，观感不变 */
    /* user_home_card_top     */ 320,  /* == 旧的 USER_TOPBAR_H + USER_PAD_LG*2 */
    /* user_home_cabin_w      */ 504,  /* == 旧的 LV_PCT(28) × 内容区 1800 */
    /* user_home_cabin_lead_w */ 270,  /* 稿 180@1280 × 1.5 */
    /* user_home_cabin_pad_right */ 64,   /* == 旧的 USER_PAD_LG */
    /* user_back_btn_w        */ 180,
    /* user_back_btn_h        */ 64,
    /* user_sensor_strip_h    */ 120,

    /* 打印流程 */
    /* user_dlg_w                 */ 720,
    /* user_dlg_pad               */ 40,
    /* user_dlg_pad_lg            */ 56,
    /* user_dlg_title_pad_bottom  */ 24,
    /* user_dlg_radius            */ 28,
    /* user_dlg_body_min_h        */ 260,
    /* user_dlg_footer_btn_w      */ 260,
    /* user_dlg_footer_btn_h      */ 96,
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
    /* user_door_slide_w          */ 693,  /* == 旧的 user_slide_w*4/3，观感不变 */
    /* user_door_slide_h          */ 133,  /* == 旧的 user_btn_door_icon_sz*4/3 */
    /* user_door_slide_knob       */ 121,  /* == 旧的派生结果（内缩 6） */
    /* user_spinner_sz            */ 104,
    /* user_spinner_stroke        */ 8,
    /* user_progress_w            */ 520,
    /* user_progress_h            */ 20,
    /* user_print_bar_h           */ 140,
    /* user_print_bar_pad_hor     */ 32,
    /* user_print_bar_pad_ver     */ 20,
    /* user_print_bar_gap         */ 32,
    /* user_print_terminate_w     */ 180,
    /* user_print_terminate_h     */ 64,

    /* 打印态浮层（稿 1280 档 ×1.5） */
    /* user_print_card_w          */ 686,
    /* user_notice_card_w         */ 686,  /* == 拆分前的共用值，观感不变 */
    /* user_print_thumb_sz        */ 135,
    /* user_print_ctl_btn         */ 96,
    /* user_print_pill_w          */ 600,
    /* user_print_badge_sz        */ 24,
    /* user_print_prog_h          */ 12,
    /* user_env_col_w             */ 0,    /* 0 = 自适应：本档装不下稿的等比列宽 */

    /* 打印历史页（稿 1280 档 ×1.5） */
    /* user_hist_tab_h            */ 60,
    /* user_hist_card_h           */ 240,
    /* user_hist_banner_h         */ 63,
    /* user_hist_bar_w            */ 240,
    /* user_hist_bar_h            */ 12,
    /* user_hist_check_sz         */ 36,
    /* user_hist_btn_w            */ 150,

    /* 字体（dev） */
    /* font_body          */ &lv_font_montserrat_32,
    /* font_form          */ &lv_font_montserrat_24,
    /* font_title         */ &lv_font_montserrat_48,
    /* font_icon          */ &lv_font_montserrat_32,
    /* 字体（user） */
    /* font_user_value    */ &lv_font_montserrat_48,
    /* font_user_text     */ &lv_font_montserrat_24,
    /* font_user_text_sm  */ &lv_font_montserrat_24,  /* == font_user_text，本档观感不变 */
    /* font_user_title    */ &lv_font_montserrat_24,  /* == font_user_text，本档观感不变 */
    /* font_user_caption  */ &lv_font_montserrat_18,
    /* font_user_caption_sm */ &lv_font_montserrat_16,
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
    /* user_sidebar_w         */ 128,  /* 稿 5339:27264：16 + 96 + 16 */
    /* user_nav_item_sz       */ 96,   /* 稿值 */
    /* user_nav_gap           */ 16,   /* 稿值 */
    /* user_nav_item_h        */ 80,   /* 稿 24(padding) + 32(图标) + 24 */
    /* user_settings_nav_w    */ 300,  /* 稿值 */
    /* user_settings_gap      */ 36,   /* 稿值（左内距与列间距同值） */
    /* user_settings_gutter_r */ 176,  /* 稿值（翻页钮落在这一段） */
    /* user_settings_seg_h    */ 72,   /* 稿值 */
    /* user_nav_icon_sz       */ 59,   /* 令字形上屏 48（稿值）：48 ÷ (52/64) */
    /* user_nav_icon_pad      */ 8,
    /* user_topbar_h          */ 128,
    /* user_topbar_pad_hor    */ 28,
    /* user_topbar_pad_ver    */ 20,
    /* 间距梯度取稿上的 16/24/32（多点交叉验证：16 = 侧栏项距/胶囊间距/CABIN 行距/
     * 卡片内 gap；24 = HOME 卡行距/CABIN 标题→线→数据/消息卡内边距；32 = 首页左边距/
     * 历史页 Tab 间距/各浮层的屏幕右下边距）。1024/1920 两档保持原值不动。 */
    /* user_pad               */ 24,
    /* user_pad_sm            */ 16,
    /* user_pad_lg            */ 32,
    /* user_radius            */ 14,
    /* user_btn_radius        */ 8,
    /* user_btn_primary_w     */ 152,
    /* user_btn_primary_h     */ 52,
    /* user_btn_door_sz       */ 248,
    /* user_btn_door_icon_sz  */ 68,
    /* user_value_label_w     */ 188,
    /* user_status_dot_sz     */ 12,
    /* user_home_device_w     */ 380,
    /* user_home_device_h     */ 440,
    /* user_home_action_row_w */ 386,  /* 稿值，不再补 10% */
    /* user_home_card_w       */ 354,  /* 稿值 */
    /* user_home_card_top     */ 241,  /* 稿值 */
    /* user_home_cabin_w      */ 294,  /* 稿值 */
    /* user_home_cabin_lead_w */ 180,  /* 稿值 */
    /* user_home_cabin_pad_right */ 40,   /* 稿值 */
    /* user_back_btn_w        */ 128,
    /* user_back_btn_h        */ 44,
    /* user_sensor_strip_h    */ 84,

    /* 打印流程 */
    /* user_dlg_w                 */ 500,
    /* user_dlg_pad               */ 28,
    /* user_dlg_pad_lg            */ 40,
    /* user_dlg_title_pad_bottom  */ 16,
    /* user_dlg_radius            */ 20,
    /* user_dlg_body_min_h        */ 180,
    /* user_dlg_footer_btn_w      */ 180,
    /* user_dlg_footer_btn_h      */ 64,
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
    /* user_door_slide_w          */ 400,  /* 稿值 */
    /* user_door_slide_h          */ 80,   /* 稿值 */
    /* user_door_slide_knob       */ 64,   /* 稿值（内缩 8） */
    /* user_spinner_sz            */ 70,
    /* user_spinner_stroke        */ 6,
    /* user_progress_w            */ 360,
    /* user_progress_h            */ 14,
    /* user_print_bar_h           */ 96,
    /* user_print_bar_pad_hor     */ 22,
    /* user_print_bar_pad_ver     */ 14,
    /* user_print_bar_gap         */ 22,
    /* user_print_terminate_w     */ 128,
    /* user_print_terminate_h     */ 44,

    /* 打印态浮层（稿 3588:8735 原生 1280 稿；横向直接取稿值，
     * 纵向按屏高 720/800 折 0.9） */
    /* user_print_card_w          */ 416,  /* 稿值，不再补 10% */
    /* user_notice_card_w         */ 400,  /* 稿值（与 stats-card 的 416 不同） */
    /* user_print_thumb_sz        */ 100,  /* 稿值（原 90 是 720/800 折算的残留） */
    /* user_print_ctl_btn         */ 64,
    /* user_print_pill_w          */ 400,
    /* user_print_badge_sz        */ 16,
    /* user_print_prog_h          */ 8,
    /* user_env_col_w             */ 64,   /* 稿值（caption 降到 caption_sm 后四列都进） */

    /* 打印历史页（稿 4366:5400 原生 1280 档，直接取稿值） */
    /* user_hist_tab_h            */ 40,
    /* user_hist_card_h           */ 160,
    /* user_hist_banner_h         */ 42,
    /* user_hist_bar_w            */ 160,
    /* user_hist_bar_h            */ 8,
    /* user_hist_check_sz         */ 24,
    /* user_hist_btn_w            */ 100,

    /* 字体（dev） */
    /* font_body          */ &lv_font_montserrat_24,
    /* font_form          */ &lv_font_montserrat_18,
    /* font_title         */ &lv_font_montserrat_32,
    /* font_icon          */ &lv_font_montserrat_24,
    /* 字体（user） */
    /* font_user_value    */ &lv_font_montserrat_32,
    /* font_user_text     */ &lv_font_montserrat_18,
    /* font_user_text_sm  */ &lv_font_montserrat_16,  /* 稿值 */
    /* font_user_title    */ &lv_font_montserrat_20,  /* 稿「一级标题」 */
    /* font_user_caption  */ &lv_font_montserrat_14,
    /* font_user_caption_sm */ &lv_font_montserrat_12,
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
    /* user_nav_item_sz       */ 58,   /* == 旧的 sidebar_w - 2*pad_sm，观感不变 */
    /* user_nav_gap           */ 44,   /* 复现旧 SPACE_EVENLY 在 600 高下的项距 102 */
    /* user_nav_item_h        */ 72,
    /* user_settings_nav_w    */ 0,    /* 0 = 沿用旧式，本档观感不变 */
    /* user_settings_gap      */ 0,
    /* user_settings_gutter_r */ 0,
    /* user_settings_seg_h    */ 0,
    /* user_nav_icon_sz       */ 54,
    /* user_nav_icon_pad      */ 8,
    /* user_topbar_h          */ 96,
    /* user_topbar_pad_hor    */ 22,
    /* user_topbar_pad_ver    */ 16,
    /* user_pad               */ 22,
    /* user_pad_sm            */ 11,
    /* user_pad_lg            */ 36,
    /* user_radius            */ 11,
    /* user_btn_radius        */ 6,
    /* user_btn_primary_w     */ 122,
    /* user_btn_primary_h     */ 44,
    /* user_btn_door_sz       */ 200,
    /* user_btn_door_icon_sz  */ 54,
    /* user_value_label_w     */ 150,
    /* user_status_dot_sz     */ 10,
    /* user_home_device_w     */ 304,
    /* user_home_device_h     */ 352,
    /* user_home_action_row_w */ 339,
    /* user_home_card_w       */ 283,  /* == 旧的 LV_PCT(30) × 内容区 944，观感不变 */
    /* user_home_card_top     */ 168,  /* == 旧的 USER_TOPBAR_H + USER_PAD_LG*2 */
    /* user_home_cabin_w      */ 264,  /* == 旧的 LV_PCT(28) × 内容区 944 */
    /* user_home_cabin_lead_w */ 144,  /* 稿 180@1280 × 0.8 */
    /* user_home_cabin_pad_right */ 36,   /* == 旧的 USER_PAD_LG */
    /* user_back_btn_w        */ 102,
    /* user_back_btn_h        */ 40,
    /* user_sensor_strip_h    */ 68,

    /* 打印流程 */
    /* user_dlg_w                 */ 400,
    /* user_dlg_pad               */ 22,
    /* user_dlg_pad_lg            */ 32,
    /* user_dlg_title_pad_bottom  */ 12,
    /* user_dlg_radius            */ 16,
    /* user_dlg_body_min_h        */ 144,
    /* user_dlg_footer_btn_w      */ 144,
    /* user_dlg_footer_btn_h      */ 51,
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
    /* user_door_slide_w          */ 384,  /* == 旧的 user_slide_w*4/3，观感不变 */
    /* user_door_slide_h          */ 72,   /* == 旧的 user_btn_door_icon_sz*4/3 */
    /* user_door_slide_knob       */ 64,   /* == 旧的派生结果（内缩 4） */
    /* user_spinner_sz            */ 57,
    /* user_spinner_stroke        */ 5,
    /* user_progress_w            */ 288,
    /* user_progress_h            */ 12,
    /* user_print_bar_h           */ 76,
    /* user_print_bar_pad_hor     */ 18,
    /* user_print_bar_pad_ver     */ 11,
    /* user_print_bar_gap         */ 18,
    /* user_print_terminate_w     */ 102,
    /* user_print_terminate_h     */ 40,

    /* 打印态浮层（稿 1280 档 ×0.8） */
    /* user_print_card_w          */ 366,
    /* user_notice_card_w         */ 366,  /* == 拆分前的共用值，观感不变 */
    /* user_print_thumb_sz        */ 76,
    /* user_print_ctl_btn         */ 51,
    /* user_print_pill_w          */ 320,
    /* user_print_badge_sz        */ 13,
    /* user_print_prog_h          */ 6,
    /* user_env_col_w             */ 0,    /* 0 = 自适应：本档 51 装不下 Humidity/101.3 kpa */

    /* 打印历史页（稿 1280 档 ×0.8） */
    /* user_hist_tab_h            */ 32,
    /* user_hist_card_h           */ 128,
    /* user_hist_banner_h         */ 34,
    /* user_hist_bar_w            */ 128,
    /* user_hist_bar_h            */ 6,
    /* user_hist_check_sz         */ 20,
    /* user_hist_btn_w            */ 80,

    /* 字体（dev） */
    /* font_body          */ &lv_font_montserrat_20,
    /* font_form          */ &lv_font_montserrat_16,
    /* font_title         */ &lv_font_montserrat_28,
    /* font_icon          */ &lv_font_montserrat_20,
    /* 字体（user） */
    /* font_user_value    */ &lv_font_montserrat_24,
    /* font_user_text     */ &lv_font_montserrat_16,
    /* font_user_text_sm  */ &lv_font_montserrat_16,  /* == font_user_text，本档观感不变 */
    /* font_user_title    */ &lv_font_montserrat_16,  /* == font_user_text，本档观感不变 */
    /* font_user_caption  */ &lv_font_montserrat_14,
    /* font_user_caption_sm */ &lv_font_montserrat_12,
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
const lv_font_t *user_font_text_sm(void)   { return g_ui->font_user_text_sm; }
const lv_font_t *user_font_title(void)     { return g_ui->font_user_title; }
const lv_font_t *user_font_caption(void)   { return g_ui->font_user_caption; }
const lv_font_t *user_font_caption_sm(void){ return g_ui->font_user_caption_sm; }
const lv_font_t *user_font_icon(void)      { return g_ui->font_user_icon; }

/* 中文字体：思源黑体子集 TTF，经 LVGL TinyTTF 运行时渲染（字号 16）。
 * 数据源 lib/assets/src/lv_font_cjk_data.c 由 tools/gen_cjk_font.py 生成
 *   （从 page_user_debug_test.cpp 自动采集用字并子集化），新增中文后重跑该脚本即可。
 * 需 LV_USE_TINY_TTF=1（见 configs/rongsu_ui.defaults）。首次调用惰性创建并缓存；
 * 若创建失败则回退到 Montserrat（中文不显示）以保证不崩。 */
 extern "C" {
    extern const unsigned char lv_font_cjk_ttf[];
    extern const unsigned int  lv_font_cjk_ttf_len;
    }
    const lv_font_t *user_font_cjk(void)
    {
        static lv_font_t *s_cjk = nullptr;
        if(!s_cjk) {
            /* 字号 18（比 Montserrat 常用 16 大一号）；改此值即整体调节测试页中文字号 */
            s_cjk = lv_tiny_ttf_create_data(lv_font_cjk_ttf,
                                            static_cast<size_t>(lv_font_cjk_ttf_len), 18);
        }
        return s_cjk ? s_cjk : user_font_text();
    }

lv_style_t *user_style_btn_primary(void)   { return &s_user_btn_primary; }
lv_style_t *user_style_btn_ghost(void)     { return &s_user_btn_ghost; }
