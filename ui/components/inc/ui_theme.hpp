#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  显示分辨率 Profile —— 运行时自适应
 *  ----------------------------------------------------------------------------
 *  现在 profile 不再是编译期宏选择，而是在 ui_theme_init() 中根据 DRM 实际
 *  分辨率自动挑选（按最长边）：
 *    实际最长边 ≥ 1600 px  → UI_RES_1920 套尺寸
 *    1100 ≤ 最长边 < 1600  → UI_RES_1280 套尺寸
 *    实际最长边 <  1100 px → UI_RES_1024 套尺寸（1024×600 物理横屏）
 *
 *  一份固件同时支持三种屏：编译时不用 -D 任何参数，不用改 .defaults。
 *
 *  如果你确实想强制走某套 profile（比如测试 1280 套布局），可以：
 *    1) 在调用 ui_theme_init() 前 ui_theme_force_profile(UI_RES_1280)，或
 *    2) 设置环境变量 LVGL_UPC_PROFILE=1 / 2 / 3
 * ============================================================================ */
#define UI_RES_1920    1
#define UI_RES_1280    2
#define UI_RES_1024    3

/* ---- KlipperScreen 风格配色（与分辨率无关） ---- */
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

/* ---- 用户 UI（Snowpod 风格，深黑底 + 橙色高光） ---- */
extern const lv_color_t user_color_bg;
extern const lv_color_t user_color_panel;
extern const lv_color_t user_color_panel_alt;
extern const lv_color_t user_color_accent;
extern const lv_color_t user_color_accent_dim;
extern const lv_color_t user_color_text;
extern const lv_color_t user_color_text_dim;
extern const lv_color_t user_color_icon_off;
extern const lv_color_t user_color_divider;
extern const lv_color_t user_color_status_ok;
extern const lv_color_t user_color_status_err;

/* ============================================================================
 *  尺寸 / 字体集合
 *  ----------------------------------------------------------------------------
 *  把所有原本写成 #define 的尺寸宏统一收到一个只读结构体里。
 *  全局指针 g_ui 在 ui_theme_init() 中根据屏分辨率指向 s_theme_1920 或
 *  s_theme_1280（在 ui_theme.cpp 内）。
 *
 *  调用代码不需要任何改动：USER_PAD / USER_HOME_DEVICE_W 等宏会展开成
 *  g_ui-> 字段访问，运行时拿到当前 profile 的值。
 * ============================================================================ */
typedef struct {
    /* ---- 开发者 Shell 尺寸 ---- */
    int32_t ui_sidebar_w;
    int32_t ui_nav_item_h;
    int32_t ui_nav_icon_w;
    int32_t ui_pad;
    int32_t ui_pad_sm;
    int32_t ui_radius;
    int32_t ui_form_label_w;
    int32_t ui_form_label2_w;
    int32_t ui_gain_row_label_w;
    int32_t ui_gain_row_ta_w;
    int32_t ui_jog_btn_sz;
    int32_t ui_jog_home_sz;
    int32_t ui_jog_step_w;
    int32_t ui_jog_step_btn_h;
    int32_t ui_jog_block_min_h;
    int32_t ui_input_h;
    int32_t ui_ta_ip_w;
    int32_t ui_ta_short_w;
    int32_t ui_btn_conn_w;
    int32_t ui_bar_title_w;
    int32_t ui_bar_value_w;
    int32_t ui_bar_h;
    int32_t ui_indicator_sz;
    int32_t ui_switch_label_w;
    int32_t ui_switch_w;
    int32_t ui_switch_h;
    int32_t ui_settings_row_h;
    int32_t ui_settings_label_w;
    int32_t ui_settings_ta_w;
    int32_t ui_list_item_h;
    int32_t ui_btn_open_w;
    int32_t ui_btn_action_w;
    int32_t ui_btn_job_w;
    int32_t ui_confirm_box_w;
    int32_t ui_confirm_text_w;
    int32_t ui_confirm_btn_w;
    int32_t ui_confirm_btn_h;
    int32_t ui_page_anim_ms;
    int32_t ui_page_anim_slide;
    int32_t ui_btn_pad_hor;
    int32_t ui_btn_pad_ver;
    int32_t ui_nav_active_border;

    /* ---- 用户 Shell 尺寸 ---- */
    int32_t user_sidebar_w;
    int32_t user_nav_item_h;
    int32_t user_nav_icon_sz;
    int32_t user_nav_icon_pad;
    int32_t user_topbar_h;
    int32_t user_topbar_pad_hor;
    int32_t user_topbar_pad_ver;
    int32_t user_pad;
    int32_t user_pad_sm;
    int32_t user_pad_lg;
    int32_t user_radius;
    int32_t user_btn_primary_w;
    int32_t user_btn_primary_h;
    int32_t user_btn_door_sz;
    int32_t user_btn_door_icon_sz;
    int32_t user_value_label_w;
    int32_t user_status_dot_sz;
    int32_t user_home_device_w;
    int32_t user_home_device_h;
    int32_t user_back_btn_w;
    int32_t user_back_btn_h;

    /* ---- 打印流程：弹窗 / 滑动 / 旋转环 / 进度 / 打印中底条 ---- */
    int32_t user_dlg_w;
    int32_t user_dlg_pad;
    int32_t user_dlg_pad_lg;
    int32_t user_dlg_title_pad_bottom;
    int32_t user_dlg_radius;
    int32_t user_dlg_body_min_h;
    int32_t user_dlg_btn_ghost_h;
    int32_t user_dlg_footer_btn_w;
    int32_t user_dlg_footer_btn_h;
    int32_t user_file_dlg_w;
    int32_t user_file_dlg_preview_w;
    int32_t user_file_dlg_preview_h;
    int32_t user_file_sel_w;
    int32_t user_file_sel_h;
    int32_t user_file_card_w;
    int32_t user_file_card_h;
    int32_t user_file_thumb_h;
    int32_t user_slide_w;
    int32_t user_slide_h;
    int32_t user_slide_knob_sz;
    int32_t user_spinner_sz;
    int32_t user_spinner_stroke;
    int32_t user_progress_w;
    int32_t user_progress_h;
    int32_t user_print_bar_h;
    int32_t user_print_bar_pad_hor;
    int32_t user_print_bar_pad_ver;
    int32_t user_print_bar_gap;
    int32_t user_print_terminate_w;
    int32_t user_print_terminate_h;

    /* ---- 字体（开发者 Shell） ---- */
    const lv_font_t *font_body;
    const lv_font_t *font_form;
    const lv_font_t *font_title;
    const lv_font_t *font_icon;

    /* ---- 字体（用户 Shell） ---- */
    const lv_font_t *font_user_value;
    const lv_font_t *font_user_text;
    const lv_font_t *font_user_caption;
    const lv_font_t *font_user_icon;

    /* 标识本 profile 是哪一个（UI_RES_1920 / UI_RES_1280），便于运行时分支 */
    int8_t profile_id;
} ui_theme_t;

/** 当前生效的尺寸/字体集；ui_theme_init() 之前为默认 (1920)，之后跟随实际屏。 */
extern const ui_theme_t *g_ui;

/* ============================================================================
 *  原有大写宏的兼容转发（运行时变成 g_ui-> 字段访问）。
 *  调用代码继续写 USER_PAD / UI_RADIUS / USER_HOME_DEVICE_W ... 即可。
 * ============================================================================ */
#define UI_SIDEBAR_W                (g_ui->ui_sidebar_w)
#define UI_NAV_ITEM_H               (g_ui->ui_nav_item_h)
#define UI_NAV_ICON_W               (g_ui->ui_nav_icon_w)
#define UI_PAD                      (g_ui->ui_pad)
#define UI_PAD_SM                   (g_ui->ui_pad_sm)
#define UI_RADIUS                   (g_ui->ui_radius)
#define UI_FORM_LABEL_W             (g_ui->ui_form_label_w)
#define UI_FORM_LABEL2_W            (g_ui->ui_form_label2_w)
#define UI_GAIN_ROW_LABEL_W         (g_ui->ui_gain_row_label_w)
#define UI_GAIN_ROW_TA_W            (g_ui->ui_gain_row_ta_w)
#define UI_JOG_BTN_SZ               (g_ui->ui_jog_btn_sz)
#define UI_JOG_HOME_SZ              (g_ui->ui_jog_home_sz)
#define UI_JOG_STEP_W               (g_ui->ui_jog_step_w)
#define UI_JOG_STEP_BTN_H           (g_ui->ui_jog_step_btn_h)
#define UI_JOG_BLOCK_MIN_H          (g_ui->ui_jog_block_min_h)
#define UI_INPUT_H                  (g_ui->ui_input_h)
#define UI_TA_IP_W                  (g_ui->ui_ta_ip_w)
#define UI_TA_SHORT_W               (g_ui->ui_ta_short_w)
#define UI_BTN_CONN_W               (g_ui->ui_btn_conn_w)
#define UI_BAR_TITLE_W              (g_ui->ui_bar_title_w)
#define UI_BAR_VALUE_W              (g_ui->ui_bar_value_w)
#define UI_BAR_H                    (g_ui->ui_bar_h)
#define UI_INDICATOR_SZ             (g_ui->ui_indicator_sz)
#define UI_SWITCH_LABEL_W           (g_ui->ui_switch_label_w)
#define UI_SWITCH_W                 (g_ui->ui_switch_w)
#define UI_SWITCH_H                 (g_ui->ui_switch_h)
#define UI_SETTINGS_ROW_H           (g_ui->ui_settings_row_h)
#define UI_SETTINGS_LABEL_W         (g_ui->ui_settings_label_w)
#define UI_SETTINGS_TA_W            (g_ui->ui_settings_ta_w)
#define UI_LIST_ITEM_H              (g_ui->ui_list_item_h)
#define UI_BTN_OPEN_W               (g_ui->ui_btn_open_w)
#define UI_BTN_ACTION_W             (g_ui->ui_btn_action_w)
#define UI_BTN_JOB_W                (g_ui->ui_btn_job_w)
#define UI_CONFIRM_BOX_W            (g_ui->ui_confirm_box_w)
#define UI_CONFIRM_TEXT_W           (g_ui->ui_confirm_text_w)
#define UI_CONFIRM_BTN_W            (g_ui->ui_confirm_btn_w)
#define UI_CONFIRM_BTN_H            (g_ui->ui_confirm_btn_h)
#define UI_PAGE_ANIM_MS             (g_ui->ui_page_anim_ms)
#define UI_PAGE_ANIM_SLIDE          (g_ui->ui_page_anim_slide)
#define UI_BTN_PAD_HOR              (g_ui->ui_btn_pad_hor)
#define UI_BTN_PAD_VER              (g_ui->ui_btn_pad_ver)
#define UI_NAV_ACTIVE_BORDER        (g_ui->ui_nav_active_border)

#define USER_SIDEBAR_W              (g_ui->user_sidebar_w)
#define USER_NAV_ITEM_H             (g_ui->user_nav_item_h)
#define USER_NAV_ICON_SZ            (g_ui->user_nav_icon_sz)
#define USER_NAV_ICON_PAD           (g_ui->user_nav_icon_pad)
#define USER_TOPBAR_H               (g_ui->user_topbar_h)
#define USER_TOPBAR_PAD_HOR         (g_ui->user_topbar_pad_hor)
#define USER_TOPBAR_PAD_VER         (g_ui->user_topbar_pad_ver)
#define USER_PAD                    (g_ui->user_pad)
#define USER_PAD_SM                 (g_ui->user_pad_sm)
#define USER_PAD_LG                 (g_ui->user_pad_lg)
#define USER_RADIUS                 (g_ui->user_radius)
#define USER_BTN_PRIMARY_W          (g_ui->user_btn_primary_w)
#define USER_BTN_PRIMARY_H          (g_ui->user_btn_primary_h)
#define USER_BTN_DOOR_SZ            (g_ui->user_btn_door_sz)
#define USER_BTN_DOOR_ICON_SZ       (g_ui->user_btn_door_icon_sz)
#define USER_VALUE_LABEL_W          (g_ui->user_value_label_w)
#define USER_STATUS_DOT_SZ          (g_ui->user_status_dot_sz)
#define USER_HOME_DEVICE_W          (g_ui->user_home_device_w)
#define USER_HOME_DEVICE_H          (g_ui->user_home_device_h)
#define USER_BACK_BTN_W             (g_ui->user_back_btn_w)
#define USER_BACK_BTN_H             (g_ui->user_back_btn_h)

#define USER_DLG_W                  (g_ui->user_dlg_w)
#define USER_DLG_PAD                (g_ui->user_dlg_pad)
#define USER_DLG_PAD_LG             (g_ui->user_dlg_pad_lg)
#define USER_DLG_TITLE_PAD_BOTTOM   (g_ui->user_dlg_title_pad_bottom)
#define USER_DLG_RADIUS             (g_ui->user_dlg_radius)
#define USER_DLG_BACKDROP_OPA       LV_OPA_70   /* 跟分辨率无关，固定 */
#define USER_DLG_BODY_MIN_H         (g_ui->user_dlg_body_min_h)
#define USER_DLG_BTN_GHOST_H        (g_ui->user_dlg_btn_ghost_h)
#define USER_DLG_FOOTER_BTN_W       (g_ui->user_dlg_footer_btn_w)
#define USER_DLG_FOOTER_BTN_H       (g_ui->user_dlg_footer_btn_h)
#define USER_FILE_DLG_W             (g_ui->user_file_dlg_w)
#define USER_FILE_DLG_PREVIEW_W     (g_ui->user_file_dlg_preview_w)
#define USER_FILE_DLG_PREVIEW_H     (g_ui->user_file_dlg_preview_h)
#define USER_FILE_SEL_W             (g_ui->user_file_sel_w)
#define USER_FILE_SEL_H             (g_ui->user_file_sel_h)
#define USER_FILE_CARD_W            (g_ui->user_file_card_w)
#define USER_FILE_CARD_H            (g_ui->user_file_card_h)
#define USER_FILE_THUMB_H           (g_ui->user_file_thumb_h)
#define USER_SLIDE_W                (g_ui->user_slide_w)
#define USER_SLIDE_H                (g_ui->user_slide_h)
#define USER_SLIDE_KNOB_SZ          (g_ui->user_slide_knob_sz)
#define USER_SPINNER_SZ             (g_ui->user_spinner_sz)
#define USER_SPINNER_STROKE         (g_ui->user_spinner_stroke)
#define USER_PROGRESS_W             (g_ui->user_progress_w)
#define USER_PROGRESS_H             (g_ui->user_progress_h)
#define USER_PRINT_BAR_H            (g_ui->user_print_bar_h)
#define USER_PRINT_BAR_PAD_HOR      (g_ui->user_print_bar_pad_hor)
#define USER_PRINT_BAR_PAD_VER      (g_ui->user_print_bar_pad_ver)
#define USER_PRINT_BAR_GAP          (g_ui->user_print_bar_gap)
#define USER_PRINT_TERMINATE_W      (g_ui->user_print_terminate_w)
#define USER_PRINT_TERMINATE_H      (g_ui->user_print_terminate_h)

/**
 * 初始化主题：根据当前 lv_display 实际分辨率挑选 profile，再初始化所有 lv_style。
 * 必须在 platform_init() 创建 lv_display 之后调用。
 */
void ui_theme_init(void);

/**
 * 强制使用某套 profile（必须在 ui_theme_init() 之前调用）。
 * profile_id 取 UI_RES_1920 / UI_RES_1280。
 * 传 0 表示取消强制，按自动逻辑选择。
 */
void ui_theme_force_profile(int8_t profile_id);

/** 当前生效的 profile id（调试/日志用） */
int8_t ui_theme_get_profile(void);

/* ---- 字体接口（运行时返回当前 profile 的字体） ---- */
const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_title(void);
const lv_font_t *user_font_value(void);
const lv_font_t *user_font_text(void);
const lv_font_t *user_font_caption(void);
const lv_font_t *user_font_icon(void);

/* ---- 预置样式 ---- */
lv_style_t *user_style_btn_primary(void);
lv_style_t *user_style_btn_ghost(void);

#ifdef __cplusplus
}
#endif

#endif
