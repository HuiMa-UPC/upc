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
/* 稿值 #FF4242（Figma 变量 fill_5685905e）：历史页多选条与删除确认弹窗那两枚 Delete
 * 都取它。原先填的 #FF4D4D 是取近似值时的残留，实机比稿浅了一档（量到 255,77,77）。 */
const lv_color_t user_color_status_err = LV_COLOR_MAKE(0xff, 0x42, 0x42);

/* 浅色稿一组（稿 6039:65949 / 6064:71609 / 6064:71936）。与深色那套并存，
 * 目前只有首页待机态、侧栏和通知三态在用，详见 ui_theme.hpp 的说明。 */
/* 稿值 #FAFAFA。首版浅色稿（6039:65949 那批）写的是 #F5F5F5，新稿三处页面帧
 * （历史 6251:94118 / 消息 6230:89616 / 6091:13847）一致改成了 #FAFAFA，跟新的。
 * 两者只差 5/255，首页与历史页翻上去看不出变化。 */
const lv_color_t user_light_bg         = LV_COLOR_MAKE(0xfa, 0xfa, 0xfa);
const lv_color_t user_light_card       = LV_COLOR_MAKE(0xff, 0xff, 0xff);
const lv_color_t user_light_text       = LV_COLOR_MAKE(0x33, 0x31, 0x45);
const lv_color_t user_light_value      = LV_COLOR_MAKE(0x11, 0x11, 0x11);
const lv_color_t user_light_accent     = LV_COLOR_MAKE(0x48, 0x5d, 0xff);
const lv_color_t user_light_icon_off   = LV_COLOR_MAKE(0x9e, 0x9e, 0x9e);
const lv_color_t user_light_nav_on     = LV_COLOR_MAKE(0x33, 0x31, 0x45);
const lv_color_t user_light_shadow     = LV_COLOR_MAKE(0x0c, 0x1c, 0x61);
const lv_color_t user_light_divider    = LV_COLOR_MAKE(0xd9, 0xd9, 0xd9);
const lv_color_t user_light_row_bg     = LV_COLOR_MAKE(0x33, 0x31, 0x45);
const lv_color_t user_light_more_bg    = LV_COLOR_MAKE(0xf5, 0xf5, 0xf6);
const lv_color_t user_light_ok         = LV_COLOR_MAKE(0x00, 0xc7, 0x4e);
const lv_color_t user_light_dev_name   = LV_COLOR_MAKE(0x1d, 0x1d, 0x24);
const lv_color_t user_light_notice_dot = LV_COLOR_MAKE(0xfe, 0x68, 0x68);
const lv_color_t user_light_rail_mid   = LV_COLOR_MAKE(0xf4, 0xf4, 0xf4);
const lv_color_t user_light_row_sel    = LV_COLOR_MAKE(0xeb, 0xea, 0xec);
const lv_color_t user_light_glass      = LV_COLOR_MAKE(0xfc, 0xfc, 0xfc);

/* ============================================================================
 *  两套 profile 数据
 *
 *  user_topbar_h 取屏高的 16%：稿 3540:14473（1280×800）里状态栏那条带子等效高
 *  128（设备信息行 y=48 高 32、两颗按钮 y=32 高 64，三者中心都落在 y=64）。
 *  1024×600 档按 600/800 折算 96，1920 档按 1280 档 ×1.5 给 192。
 *  调试页那条传感器卡原先借用本字段，band 抬高后改用 user_sensor_strip_h，
 *  值即旧的 user_topbar_h，排版不受影响。
 *
 *  user_content_top 是页面内容区的上偏移，与 user_topbar_h 拆开给值：稿
 *  4366:5400 里设备行是 48..80（心 64，对应条高 128），而 Tab 文字顶在 112，
 *  两者只隔 32。原先共用 user_topbar_h，页面内容被压到 128 起、比稿低 16；
 *  单把该字段改成 112 又会让设备名那行跟着上移 8。故 128 留给设备条，
 *  112 独立成本字段。1920 档 ×1.5 给 168，1024 档按 600/800 折算 84。
 *
 *  ---- 触摸尺寸的刻意偏稿 ----
 *  下面这些值为触摸可用性上调，**已不再是稿值**，别按稿又调回去。
 *
 *  仅 1280 档：
 *    user_hist_tab_h  40 → 52   Tab 行高 = Tab 项命中高
 *  实测 40 在 1280×800 上偏小：Tab 项宽被写死成文字实宽、左右零内距，且文字
 *  ALIGN_TOP_MID 贴顶使命中区中心比文字视觉中心偏下。
 *  （同处曾有个 user_hist_btn_w 100 → 128，服务消息页管理态那几枚按钮；
 *  消息页改新稿后多选模式整体去掉，该字段已删。）
 *
 *  三档全体：
 *    user_media_btn_h   96 / 66 / 60   全屏媒体页的关闭钮与相册翻页箭头
 *  稿上无此项，原先各档沿用 user_back_btn_h（64/44/40），实机确认这几颗钮叠在
 *  视频/照片上又贴屏幕边缘，太难找也太难点，统一按 user_back_btn_h 的 1.5 倍给。
 *  副作用：ui_user_make_overlay_round_btn() 里那段「不足 UI_USER_MIN_TOUCH(48)
 *  就补命中区」的兜底，三档都超过 48 后不再触发。
 *
 *    user_media_play_h 128 / 112 / 80  延时播放器中央的播放/暂停钮
 *  它原先写成 user_media_btn_size() * 2，关闭钮一放大就跟着涨到 120~192，在
 *  600~1080 的屏高上占到两成，过于抢眼。拆成独立字段后两者互不牵动，这三个值
 *  即放大关闭钮之前播放钮的实际渲染尺寸，等于维持原样。
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
    /* user_settings_seg_h    */ 0,
    /* user_nav_icon_sz       */ 52,   /* 让上屏字形保持改版前的 49：49 ÷ (30/32) */
    /* user_nav_icon_pad      */ 12,
    /* user_topbar_h          */ 192,
    /* user_content_top       */ 168,  /* 稿 112@1280 × 1.5 */
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
    /* user_home_card_w       */ 360,  /* 浅色稿 240 ×1.5 */
    /* user_home_card_top     */ 430,  /* 浅色稿 287 ×1.5 */
    /* user_metric_icon_sz    */ 48,   /* 稿 32@1280 × 1.5 */
    /* user_back_btn_w        */ 180,
    /* user_back_btn_h        */ 64,
    /* user_media_btn_h       */ 96,   /* 触摸可用性上调：user_back_btn_h × 3/2 */
    /* user_media_play_h      */ 128,  /* 放大关闭钮前的实际渲染值，维持原样 */
    /* user_sensor_strip_h    */ 120,
    /* user_empty_ring_sz     */ 0,    /* 无稿，沿用旧式 72*10/9 = 80 */

    /* 打印流程 */
    /* user_dlg_w                 */ 720,
    /* user_dlg_pad               */ 40,
    /* user_dlg_pad_lg            */ 56,
    /* user_dlg_title_pad_bottom  */ 24,
    /* user_dlg_radius            */ 28,
    /* user_dlg_body_min_h        */ 260,
    /* user_dlg_footer_btn_w      */ 260,
    /* user_dlg_footer_btn_h      */ 96,
    /* user_dlg_title_lead        */ 15,   /* 稿 39×1.5=59 − montserrat_40 的 44 */
    /* user_dlg_body_lead         */ 9,    /* 稿 24×1.5=36 − montserrat_24 的 27 */
    /* user_file_dlg_w            */ 1100,
    /* user_file_dlg_preview_w    */ 340,
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
    /* user_hist_tab_h            */ 90,
    /* user_hist_card_h           */ 240,  /* 仅照片浏览器缩略图带在用 */
    /* user_hist_check_sz         */ 36,
    /* user_hist_pad_l            */ 72,
    /* user_hist_grid_gap         */ 48,
    /* user_hist_card_radius      */ 24,
    /* user_hist_selbar_h         */ 120,  /* 稿 80×1.5 */
    /* user_hist_del_btn_w        */ 210,
    /* user_hist_del_btn_h        */ 96,

    /* 浅色稿几何（1280 稿值 ×1.5，本档未逐个核） */
    /* user_light_radius      */ 36,
    /* user_light_btn_sz      */ 108,
    /* user_action_btn_sz     */ 126,
    /* user_home_card_h       */ 450,
    /* user_home_grid_w       */ 510,
    /* user_home_greet_top    */ 240,
    /* user_home_dev_img_w    */ 788,
    /* user_home_dev_img_h    */ 1187,
    /* user_home_dev_img_x    */ 1158,
    /* user_home_dev_img_y    */ 134,
    /* user_home_print_btn_w  */ 324,
    /* user_home_slide_w      */ 680,
    /* user_home_slide_h      */ 141,
    /* user_home_edge_gap     */ 72,
    /* user_panel_w           */ 480,
    /* user_panel_row_h       */ 72,

    /* 监控页：1280 档取稿值，本档按 ×1.5 折算 */
    /* user_mon_panel_w       */ 630,
    /* user_mon_row_h         */ 96,
    /* user_mon_radius        */ 24,
    /* user_mon_thumb_h       */ 315,
    /* user_mon_stat_h        */ 147,
    /* user_mon_bar_h         */ 36,

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
    /* font_user_display  */ &lv_font_montserrat_48,  /* 稿 40×1.5=60，字体表到 48 封顶 */
    /* font_user_metric   */ &lv_font_montserrat_48,  /* 稿 32×1.5=48 */
    /* font_user_body     */ &lv_font_montserrat_32,  /* 稿 24×1.5=36，取最近的 32 */
    /* font_user_dlg_title*/ &lv_font_montserrat_40,  /* 稿 28×1.5=42，取最近的 40 */
    /* font_user_dlg_btn  */ &lv_font_montserrat_32,  /* 稿 22×1.5=33，取最近的 32 */
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
    /* user_settings_seg_h    */ 72,   /* 稿值 */
    /* user_nav_icon_sz       */ 32,   /* 稿值（图标盒边长，字形在里面占 30） */
    /* user_nav_icon_pad      */ 8,
    /* user_topbar_h          */ 128,
    /* user_content_top       */ 112,  /* 稿值：Tab 文字顶 y=112 */
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
    /* user_home_card_w       */ 240,  /* 浅色稿值（上一版浅色稿是 300） */
    /* user_home_card_top     */ 287,  /* 浅色稿值（上一版浅色稿是 275） */
    /* user_metric_icon_sz    */ 32,   /* 稿值 */
    /* user_back_btn_w        */ 128,
    /* user_back_btn_h        */ 44,
    /* user_media_btn_h       */ 66,   /* 触摸可用性上调（稿无此项）：user_back_btn_h × 3/2 */
    /* user_media_play_h      */ 112,  /* 放大关闭钮前的实际渲染值，维持原样 */
    /* user_sensor_strip_h    */ 84,
    /* user_empty_ring_sz     */ 80,   /* 稿值；旧式在本档只算得 57 */

    /* 打印流程 */
    /* user_dlg_w                 */ 496,  /* 稿值（6251:94239；原 500 是近似值）*/
    /* user_dlg_pad               */ 28,
    /* user_dlg_pad_lg            */ 40,
    /* user_dlg_title_pad_bottom  */ 16,
    /* user_dlg_radius            */ 20,
    /* user_dlg_body_min_h        */ 180,
    /* user_dlg_footer_btn_w      */ 180,
    /* user_dlg_footer_btn_h      */ 64,
    /* user_dlg_title_lead        */ 9,    /* 稿 39 − montserrat_28 的 30 */
    /* user_dlg_body_lead         */ 2,    /* 稿 24 − montserrat_20 的 22 */
    /* user_file_dlg_w            */ 760,
    /* user_file_dlg_preview_w    */ 240,
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
    /* user_hist_tab_h            */ 60,   /* 稿值 */
    /* user_hist_card_h           */ 160,  /* 仅照片浏览器缩略图带在用 */
    /* user_hist_check_sz         */ 24,   /* 稿值（圆角 8 的蓝块，与 ic_hist_check 同边长） */
    /* user_hist_pad_l            */ 48,   /* 稿：Tab 文字与卡片同在 x=176 − 侧栏 128 */
    /* user_hist_grid_gap         */ 32,   /* 稿值 */
    /* user_hist_card_radius      */ 16,   /* 稿值 */
    /* user_hist_selbar_h         */ 80,   /* 稿值（6251:94204 是 1088×80）*/
    /* user_hist_del_btn_w        */ 140,  /* 稿值 */
    /* user_hist_del_btn_h        */ 64,   /* 稿值 */

    /* 浅色稿几何（稿 6201:85911 原生 1280×800，本档逐个量的稿值） */
    /* user_light_radius      */ 24,   /* 稿值 */
    /* user_light_btn_sz      */ 72,   /* 稿值（右上铃铛，与下面那枚已不同值） */
    /* user_action_btn_sz     */ 84,   /* 稿值（左下方钮 / PRINT 高 / 开门滑块旋钮） */
    /* user_home_card_h       */ 300,  /* 稿值（HOME 卡与四宫格等高） */
    /* user_home_grid_w       */ 340,  /* 稿值（扣 8 的间距后单卡 166 宽 146 高，与稿一致） */
    /* user_home_greet_top    */ 160,  /* 稿值 */
    /* user_home_dev_img_w    */ 525,  /* 稿值 */
    /* user_home_dev_img_h    */ 791,  /* 稿值 */
    /* user_home_dev_img_x    */ 772,  /* 稿值（右侧出血，超出部分由父节点裁） */
    /* user_home_dev_img_y    */ 89,   /* 稿值（下方同样出血） */
    /* user_home_print_btn_w  */ 216,  /* 稿值 */
    /* user_home_slide_w      */ 453,  /* 稿值 */
    /* user_home_slide_h      */ 94,   /* 稿值（旋钮 84 上下各内缩 5） */
    /* user_home_edge_gap     */ 48,   /* 稿值（横向仍是 USER_PAD_LG 的 32，稿就不对称） */
    /* user_panel_w           */ 320,  /* 稿值 */
    /* user_panel_row_h       */ 48,   /* 稿值 */

    /* 监控页（稿 6338:27240 一族），本档全取稿值 */
    /* user_mon_panel_w       */ 420,  /* 屏上 860..1280，内容区 420−2×32 = 356 */
    /* user_mon_row_h         */ 64,
    /* user_mon_radius        */ 16,
    /* user_mon_thumb_h       */ 210,
    /* user_mon_stat_h        */ 98,
    /* user_mon_bar_h         */ 24,

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
    /* font_user_display  */ &lv_font_montserrat_40,  /* 稿值 */
    /* font_user_metric   */ &lv_font_montserrat_32,  /* 稿值 */
    /* font_user_body     */ &lv_font_montserrat_24,  /* 稿值 */
    /* font_user_dlg_title*/ &lv_font_montserrat_28,  /* 稿值 */
    /* font_user_dlg_btn  */ &lv_font_montserrat_22,  /* 稿值 */
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
    /* user_settings_seg_h    */ 0,
    /* user_nav_icon_sz       */ 50,   /* 让上屏字形保持改版前的 47：47 ÷ (30/32) */
    /* user_nav_icon_pad      */ 8,
    /* user_topbar_h          */ 96,
    /* user_content_top       */ 84,   /* 稿 112@1280 × 600/800 */
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
    /* user_home_card_w       */ 192,  /* 浅色稿 240 ×0.8 */
    /* user_home_card_top     */ 230,  /* 浅色稿 287 ×0.8 */
    /* user_metric_icon_sz    */ 26,   /* 稿 32@1280 × 0.8 = 25.6，进位；本档横杠只剩
                                        * 约 1.6px 偏软，但放大到 28 会比周围 0.8 折算
                                        * 的字号显大，仍按折算值 */
    /* user_back_btn_w        */ 102,
    /* user_back_btn_h        */ 40,
    /* user_media_btn_h       */ 60,   /* 触摸可用性上调：user_back_btn_h × 3/2 */
    /* user_media_play_h      */ 80,   /* 放大关闭钮前的实际渲染值，维持原样 */
    /* user_sensor_strip_h    */ 68,
    /* user_empty_ring_sz     */ 0,    /* 无稿，沿用旧式 44*10/9 = 48 */

    /* 打印流程 */
    /* user_dlg_w                 */ 400,
    /* user_dlg_pad               */ 22,
    /* user_dlg_pad_lg            */ 32,
    /* user_dlg_title_pad_bottom  */ 12,
    /* user_dlg_radius            */ 16,
    /* user_dlg_body_min_h        */ 144,
    /* user_dlg_footer_btn_w      */ 144,
    /* user_dlg_footer_btn_h      */ 51,
    /* user_dlg_title_lead        */ 4,    /* 稿 39×0.8=31 − montserrat_24 的 27 */
    /* user_dlg_body_lead         */ 1,    /* 稿 24×0.8=19 − montserrat_16 的 18 */
    /* user_file_dlg_w            */ 600,
    /* user_file_dlg_preview_w    */ 192,
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
    /* user_hist_tab_h            */ 48,
    /* user_hist_card_h           */ 128,  /* 仅照片浏览器缩略图带在用 */
    /* user_hist_check_sz         */ 19,
    /* user_hist_pad_l            */ 38,
    /* user_hist_grid_gap         */ 26,
    /* user_hist_card_radius      */ 13,
    /* user_hist_selbar_h         */ 64,   /* 稿 80×0.8 */
    /* user_hist_del_btn_w        */ 112,
    /* user_hist_del_btn_h        */ 51,

    /* 浅色稿几何（1280 稿值 ×0.8，本档未逐个核） */
    /* user_light_radius      */ 19,
    /* user_light_btn_sz      */ 58,
    /* user_action_btn_sz     */ 67,
    /* user_home_card_h       */ 240,
    /* user_home_grid_w       */ 272,
    /* user_home_greet_top    */ 128,
    /* user_home_dev_img_w    */ 420,
    /* user_home_dev_img_h    */ 633,
    /* user_home_dev_img_x    */ 618,
    /* user_home_dev_img_y    */ 71,
    /* user_home_print_btn_w  */ 173,
    /* user_home_slide_w      */ 362,
    /* user_home_slide_h      */ 75,
    /* user_home_edge_gap     */ 38,
    /* user_panel_w           */ 256,
    /* user_panel_row_h       */ 38,

    /* 监控页：1280 档取稿值，本档按 ×0.8 折算。
     * panel_w 特意留得比 336（420×0.8）宽一点：本档 USER_PAD_LG 是 36（比 1280 档的 32
     * 还大，见「间距梯度」那一节的非单调说明），内容区会被多扣 8，344−72 = 272 已经很窄。 */
    /* user_mon_panel_w       */ 344,
    /* user_mon_row_h         */ 51,
    /* user_mon_radius        */ 13,
    /* user_mon_thumb_h       */ 168,
    /* user_mon_stat_h        */ 78,
    /* user_mon_bar_h         */ 19,

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
    /* font_user_display  */ &lv_font_montserrat_32,  /* 稿 40×0.8=32 */
    /* font_user_metric   */ &lv_font_montserrat_24,  /* 稿 32×0.8=25.6，取最近的 24 */
    /* font_user_body     */ &lv_font_montserrat_20,  /* 稿 24×0.8=19.2，取最近的 20 */
    /* font_user_dlg_title*/ &lv_font_montserrat_24,  /* 稿 28×0.8=22.4，取最近的 24 */
    /* font_user_dlg_btn  */ &lv_font_montserrat_18,  /* 稿 22×0.8=17.6，取最近的 18 */
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

int32_t user_media_btn_size(void)
{
    return g_ui->user_media_btn_h > 0 ? g_ui->user_media_btn_h
                                      : g_ui->user_back_btn_h;
}

int32_t user_media_play_size(void)
{
    return g_ui->user_media_play_h > 0 ? g_ui->user_media_play_h
                                       : user_media_btn_size() * 2;
}

const lv_font_t *ui_font_body(void)        { return g_ui->font_body; }
const lv_font_t *ui_font_title(void)       { return g_ui->font_title; }
const lv_font_t *user_font_value(void)     { return g_ui->font_user_value; }
const lv_font_t *user_font_text(void)      { return g_ui->font_user_text; }
const lv_font_t *user_font_text_sm(void)   { return g_ui->font_user_text_sm; }
const lv_font_t *user_font_title(void)     { return g_ui->font_user_title; }
const lv_font_t *user_font_caption(void)   { return g_ui->font_user_caption; }
const lv_font_t *user_font_caption_sm(void){ return g_ui->font_user_caption_sm; }
const lv_font_t *user_font_display(void)   { return g_ui->font_user_display; }
const lv_font_t *user_font_metric(void)    { return g_ui->font_user_metric; }
const lv_font_t *user_font_body(void)      { return g_ui->font_user_body; }
const lv_font_t *user_font_dlg_title(void) { return g_ui->font_user_dlg_title; }
const lv_font_t *user_font_dlg_btn(void)   { return g_ui->font_user_dlg_btn; }
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
