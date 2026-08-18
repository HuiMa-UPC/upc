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

/* ---- 用户 UI（Snowpod 风格，深黑底 + 主色高光；user_color_accent 现为蓝 #4258ff） ---- */
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
    /** 稿里那套弹窗/详情面板按钮组件的圆角（8@1280，不是全圆胶囊）：
     *  确认弹窗的取消/危险钮、选文件页详情面板的 Confirm Print / Delete File。 */
    int32_t user_btn_radius;
    int32_t user_btn_primary_w;
    int32_t user_btn_primary_h;
    int32_t user_btn_door_sz;
    int32_t user_btn_door_icon_sz;
    int32_t user_value_label_w;
    int32_t user_status_dot_sz;
    int32_t user_home_device_w;
    int32_t user_home_device_h;
    /** 首页左下入口胶囊那一行的定宽（稿 5339:27275 是 386@1280）。行内灰玻璃钮按内容
     *  撑宽、蓝钮 flex_grow 吃掉剩余，所以只需定这一个总宽。
     *  实取比稿宽 10%（424@1280）：稿排 Poppins、工程用 Montserrat，同字号更宽——按稿
     *  值折到 1024 档只有 309，此时蓝钮内可用宽 140，装不下图标(25)+"Start Print"(约 85)
     *  还留出间距。放到 424 后 1024 档是 339、蓝钮内余量约 30px。同一套「按稿宽补 10%」
     *  的处理见 user_print_card_w。改两枚按钮文案前先量一遍：label 是 LONG_WRAP，
     *  超了会折成两行把胶囊撑变形，而不是截断。 */
    int32_t user_home_action_row_w;
    int32_t user_back_btn_w;
    int32_t user_back_btn_h;
    int32_t user_sensor_strip_h;   /**< 调试页传感器条高（原先借用 user_topbar_h） */

    /* ---- 打印流程：弹窗 / 滑动 / 旋转环 / 进度 / 打印中底条 ---- */
    int32_t user_dlg_w;
    int32_t user_dlg_pad;
    int32_t user_dlg_pad_lg;
    int32_t user_dlg_title_pad_bottom;
    int32_t user_dlg_radius;
    int32_t user_dlg_body_min_h;
    int32_t user_dlg_footer_btn_w;
    /** 弹窗页脚按钮 / 满宽主按钮高：按稿 64@1280 等比折算（1024=51、1920=96）。
     *  共用方：确认弹窗页脚、门弹窗与放版弹窗的满宽钮、选文件页详情两枚按钮与厚度分段。 */
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
    /** 圆环加载圈直径与线宽。全部四个 ui_arc_spinner（门弹窗、准备弹窗、转运盒两步）
     *  都只从这两个值取尺寸，没有别处写死，改这里即全局生效。 */
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

    /* ---- 打印态浮层（稿 3588:8735 / 3589:9992 一套六态，按 1280 稿折算三档）----
     * 六个态（打印中/暂停/中断/完成/吹粉中/吹粉完成）共用一张 stats-card、一条
     * 提示条、一组底部复合控件，尺寸全部走这几个常量，不写死坐标。 */
    /** stats-card 宽。稿上是 416@1280，实机比稿宽 10%：Montserrat 比稿里的 Futura
     *  更占地方，按稿宽会把 CONSUMABLES 那一列的 caption 截掉。 */
    int32_t user_print_card_w;
    int32_t user_print_thumb_sz;  /**< stats-card 左侧缩略图边长（稿 100） */
    /* 下面两个尺寸与预生成的玻璃底衬图强耦合：底衬按 tools/gen_ctl_backdrop.py 的
     * PROFILES 三档出图、运行时不缩放，改了必须同步改脚本并重跑，否则会和按钮错位。
     * 底衬上那颗圆的直径 = ctl_btn×5/4、两块之间的缝 = ctl_btn/8（均取自稿）。 */
    int32_t user_print_ctl_btn;   /**< 底部复合控件里的圆钮直径（稿 64；右侧中部的拍照钮同尺寸） */
    int32_t user_print_pill_w;    /**< 底部复合控件的胶囊宽（稿 400，即滑动取件轨道宽） */
    int32_t user_print_badge_sz;  /**< 徽标行状态图标边长（稿 16） */
    int32_t user_print_prog_h;    /**< 卡片内进度条高（稿 8） */

    /* ---- 打印历史页（稿 4366:5400 一套，按 1280 稿等比折算三档）---- */
    int32_t user_hist_tab_h;      /**< Tab 行高（同行右侧 Manage/多选按钮也用此高） */
    int32_t user_hist_card_h;     /**< 历史卡片高；左侧缩略图与卡等高、宽为其 8/5 */
    int32_t user_hist_banner_h;   /**< 存储告警横幅高 */
    int32_t user_hist_bar_w;      /**< 顶部存储容量进度条轨道宽 */
    int32_t user_hist_bar_h;      /**< 顶部存储容量进度条轨道高 */
    int32_t user_hist_check_sz;   /**< 管理模式复选框边长 */
    int32_t user_hist_btn_w;      /**< Tab 行右侧按钮宽（Manage/Delete/Select All/Cancel） */

    /* ---- 字体（开发者 Shell） ---- */
    const lv_font_t *font_body;
    const lv_font_t *font_form;
    const lv_font_t *font_title;
    const lv_font_t *font_icon;

    /* ---- 字体（用户 Shell） ---- */
    const lv_font_t *font_user_value;
    const lv_font_t *font_user_text;
    const lv_font_t *font_user_caption;
    /** 比 caption 再小一档，给窄容器里的说明字用（打印统计卡的 TIME/CONSUMABLES 两列）。
     *  1024/1280 两档落到 12 号——这个字号是为它才在 rongsu_ui.defaults 里打开的。 */
    const lv_font_t *font_user_caption_sm;
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
#define USER_BTN_RADIUS             (g_ui->user_btn_radius)
#define USER_BTN_PRIMARY_W          (g_ui->user_btn_primary_w)
#define USER_BTN_PRIMARY_H          (g_ui->user_btn_primary_h)
#define USER_BTN_DOOR_SZ            (g_ui->user_btn_door_sz)
#define USER_BTN_DOOR_ICON_SZ       (g_ui->user_btn_door_icon_sz)
#define USER_VALUE_LABEL_W          (g_ui->user_value_label_w)
#define USER_STATUS_DOT_SZ          (g_ui->user_status_dot_sz)
#define USER_HOME_DEVICE_W          (g_ui->user_home_device_w)
#define USER_HOME_DEVICE_H          (g_ui->user_home_device_h)
#define USER_HOME_ACTION_ROW_W      (g_ui->user_home_action_row_w)
#define USER_BACK_BTN_W             (g_ui->user_back_btn_w)
#define USER_BACK_BTN_H             (g_ui->user_back_btn_h)
#define USER_SENSOR_STRIP_H         (g_ui->user_sensor_strip_h)

#define USER_DLG_W                  (g_ui->user_dlg_w)
#define USER_DLG_PAD                (g_ui->user_dlg_pad)
#define USER_DLG_PAD_LG             (g_ui->user_dlg_pad_lg)
#define USER_DLG_TITLE_PAD_BOTTOM   (g_ui->user_dlg_title_pad_bottom)
#define USER_DLG_RADIUS             (g_ui->user_dlg_radius)
#define USER_DLG_BACKDROP_OPA       LV_OPA_70   /* 跟分辨率无关，固定 */
#define USER_DLG_BODY_MIN_H         (g_ui->user_dlg_body_min_h)
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

#define USER_PRINT_CARD_W           (g_ui->user_print_card_w)
#define USER_PRINT_THUMB_SZ         (g_ui->user_print_thumb_sz)
#define USER_PRINT_CTL_BTN          (g_ui->user_print_ctl_btn)
#define USER_PRINT_PILL_W           (g_ui->user_print_pill_w)
#define USER_PRINT_BADGE_SZ         (g_ui->user_print_badge_sz)
#define USER_PRINT_PROG_H           (g_ui->user_print_prog_h)

#define USER_HIST_TAB_H             (g_ui->user_hist_tab_h)
#define USER_HIST_CARD_H            (g_ui->user_hist_card_h)
#define USER_HIST_BANNER_H          (g_ui->user_hist_banner_h)
#define USER_HIST_BAR_W             (g_ui->user_hist_bar_w)
#define USER_HIST_BAR_H             (g_ui->user_hist_bar_h)
#define USER_HIST_CHECK_SZ          (g_ui->user_hist_check_sz)
#define USER_HIST_BTN_W             (g_ui->user_hist_btn_w)

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
/** 比 caption 小一档，给窄容器里塞不下的说明字用。 */
const lv_font_t *user_font_caption_sm(void);
const lv_font_t *user_font_icon(void);
/** 中文字体（子集，编译进固件）。用于需要中文标签的页面，如调试测试页。 */
const lv_font_t *user_font_cjk(void);

/* ---- 预置样式 ---- */
lv_style_t *user_style_btn_primary(void);
lv_style_t *user_style_btn_ghost(void);

#ifdef __cplusplus
}
#endif

#endif
