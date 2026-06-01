#include "page_user_home.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "ui_msg_proxy.hpp"
#include "control.hpp"
#include "snowpod_device_img.h"
#include "ui_slide_unlock.hpp"

#include <cstdio>
#include <cstring>

/**
 * @file page_user_home.cpp
 * @brief Snowpod 风格用户首页
 *
 * 视觉层次（与设计稿对齐）：
 *   ┌────────────────────────────────────────────────┐
 *   │ [WiFi] [Sig] SNOWPOD A1 ● 准备打印     [开始打印]│ ← topbar
 *   ├────────────┬────────────────┬────────────────┤
 *   │ 室内 PM2.5 │                │   设备温度     │
 *   │   25.3     │   [Device]     │    23.6°C     │
 *   │ 舱内 PM2.5 │                │   氧气浓度     │
 *   │   25.3     │                │    84.9%      │
 *   └────────────┴────────────────┴────────────────┘
 *                                  [滑动开门 >>>]   ← 浮动
 *
 * 环境快照通过 UiMsgProxy::register_env_update 订阅，数据保存在本文件内。
 */

namespace {

typedef enum {
    USER_HOME_STATUS_IDLE = 0,
    USER_HOME_STATUS_READY,
    USER_HOME_STATUS_PRINTING,
    USER_HOME_STATUS_ERROR,
} user_home_status_t;

typedef struct {
    char               device_name[32];
    user_home_status_t status;
    bool               wifi_connected;
    int                signal_bars;
    float              pm25_indoor;
    float              pm25_cabin;
    float              temperature_c;
    float              oxygen_percent;
} user_home_snapshot_t;

user_home_snapshot_t g_snap;
bool g_env_defaults_loaded = false;
bool g_status_locked = false;

void apply_env(const middleware::EnvSnapshotPayload &p)
{
    std::strncpy(g_snap.device_name, p.device_name, sizeof(g_snap.device_name) - 1);
    g_snap.device_name[sizeof(g_snap.device_name) - 1] = '\0';
    if(!g_status_locked) {
        g_snap.status = static_cast<user_home_status_t>(p.status);
    }
    g_snap.wifi_connected = p.wifi_connected != 0;
    g_snap.signal_bars    = p.signal_bars;
    g_snap.pm25_indoor    = p.pm25_indoor;
    g_snap.pm25_cabin     = p.pm25_cabin;
    g_snap.temperature_c  = p.temperature_c;
    g_snap.oxygen_percent = p.oxygen_percent;
    page_user_home_refresh(false);
}

void home_env_init(void)
{
    if(!g_env_defaults_loaded) {
        std::memset(&g_snap, 0, sizeof(g_snap));
        std::strcpy(g_snap.device_name, "SNOWPOD A1");
        g_snap.status         = USER_HOME_STATUS_READY;
        g_snap.wifi_connected = true;
        g_snap.signal_bars    = 3;
        g_snap.pm25_indoor    = 25.0f;
        g_snap.pm25_cabin     = 25.0f;
        g_snap.temperature_c  = 23.6f;
        g_snap.oxygen_percent = 84.9f;
        g_env_defaults_loaded = true;
    }
    ui::UiMsgProxy::instance().register_env_update(
        [](const middleware::EnvSnapshotPayload &p) { apply_env(p); });
}

void home_set_status(user_home_status_t s)
{
    g_snap.status = s;
    g_status_locked = (s == USER_HOME_STATUS_PRINTING);
}

void home_trigger_door_open(void)
{
    ui::UiMsgProxy::instance().send_open_door();
}

/* ---------- 顶部状态条 ---------- */
lv_obj_t *g_lbl_dev_name = nullptr;
lv_obj_t *g_dot_status   = nullptr;
lv_obj_t *g_lbl_status   = nullptr;
lv_obj_t *g_lbl_wifi     = nullptr;
lv_obj_t *g_lbl_signal   = nullptr;
lv_obj_t *g_btn_start    = nullptr;

/* ---------- 数值（左/右 各 2 块） ---------- */
lv_obj_t *g_lbl_pm_indoor = nullptr;
lv_obj_t *g_lbl_pm_cabin  = nullptr;
lv_obj_t *g_lbl_temp      = nullptr;
lv_obj_t *g_lbl_oxygen    = nullptr;

/* ---------- 页面容器：用于打印中模式切换 ---------- */
lv_obj_t *g_page_root    = nullptr;
lv_obj_t *g_normal_area  = nullptr;   /**< 中央设备 + 左右数值（打印中隐藏） */
lv_obj_t *g_door_float   = nullptr;   /**< 浮动开门滑块（打印中隐藏） */
ui_slide_unlock_t *g_door_slide = nullptr; /**< 首页"滑动开门"控件 */

/* ---------- 打印中模式：摄像头 + 双底条 ---------- */
lv_obj_t *g_print_area      = nullptr; /**< 摄像头 + 双底条容器（默认隐藏） */
lv_obj_t *g_camera_viewport = nullptr; /**< 摄像头画面容器 */
lv_obj_t *g_camera_image    = nullptr; /**< 真实视频帧图像槽（先隐藏） */
lv_obj_t *g_env_lbl_pm_indoor = nullptr;
lv_obj_t *g_env_lbl_pm_cabin  = nullptr;
lv_obj_t *g_env_lbl_temp      = nullptr;
lv_obj_t *g_env_lbl_oxygen    = nullptr;
lv_obj_t *g_prog_bar          = nullptr;
lv_obj_t *g_prog_value        = nullptr; /**< "37%" */
lv_obj_t *g_prog_remaining    = nullptr; /**< "剩余 45 min" */
lv_obj_t *g_prog_file_name    = nullptr;
bool      g_printing_active   = false;

void on_start_print_click(lv_event_t *)
{
    /* 不再使用 mock toast，转为真正的打印流程入口。 */
    control_start();
}

/* 滑动开门解锁后：触发开门 + 提示，并在短暂延时后把滑块弹回以便再次使用 */
void door_slide_reset_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if(g_door_slide) {
        ui_slide_unlock_reset(g_door_slide);
    }
}

void on_door_slide_unlocked(void * /*user_data*/)
{
    home_trigger_door_open();
    ui_toast_show("Open Door", user_color_accent, 1500);
    lv_timer_t *t = lv_timer_create(door_slide_reset_cb, 700, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void on_terminate_click(lv_event_t *)
{
    control_terminate_request();
}

/** 顶部状态条：左侧设备名/状态 + 右侧 "开始打印" 橙色按钮 */
lv_obj_t *build_topbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_TOPBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(bar, USER_TOPBAR_PAD_VER, 0);
    lv_obj_set_style_pad_column(bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_wifi = lv_label_create(bar);
    lv_label_set_text(g_lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(g_lbl_wifi, user_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_wifi, user_font_text(), 0);

    g_lbl_signal = lv_label_create(bar);
    /* LVGL 没有专门的信号格图标，用 BLUETOOTH 占位（视觉相近）。
     * 后续接入 Material 图标字体时替换为真实 signal 图标。 */
    lv_label_set_text(g_lbl_signal, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(g_lbl_signal, user_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_signal, user_font_text(), 0);

    g_lbl_dev_name = lv_label_create(bar);
    lv_label_set_text(g_lbl_dev_name, "SNOWPOD A1");
    lv_obj_set_style_text_color(g_lbl_dev_name, user_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_dev_name, user_font_text(), 0);
    lv_obj_set_style_pad_left(g_lbl_dev_name, USER_PAD_SM, 0);

    g_dot_status = lv_obj_create(bar);
    lv_obj_remove_style_all(g_dot_status);
    lv_obj_set_size(g_dot_status, USER_STATUS_DOT_SZ, USER_STATUS_DOT_SZ);
    lv_obj_set_style_radius(g_dot_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dot_status, user_color_status_ok, 0);
    lv_obj_set_style_bg_opa(g_dot_status, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(g_dot_status, USER_PAD_SM, 0);

    g_lbl_status = lv_label_create(bar);
    lv_label_set_text(g_lbl_status, "Ready");
    lv_obj_set_style_text_color(g_lbl_status, user_color_status_ok, 0);
    lv_obj_set_style_text_font(g_lbl_status, user_font_text(), 0);

    /* 用一个 flex_grow 撑开把后面按钮顶到右边 */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    g_btn_start = lv_button_create(bar);
    lv_obj_add_style(g_btn_start, user_style_btn_primary(), 0);
    lv_obj_set_size(g_btn_start, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
    lv_obj_add_event_cb(g_btn_start, on_start_print_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_lbl = lv_label_create(g_btn_start);
    lv_label_set_text(btn_lbl, "Start Print");
    lv_obj_set_style_text_color(btn_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(btn_lbl, user_font_text(), 0);
    lv_obj_center(btn_lbl);

    return bar;
}

/* 设备/倒影资源的 master 像素尺寸（必须与 tools/gen_device_img.py 输出一致） */
#define SNOWPOD_DEV_MASTER_W   520
#define SNOWPOD_DEV_MASTER_H   723
#define SNOWPOD_REF_MASTER_W   520
#define SNOWPOD_REF_MASTER_H   304

/** 中央设备图 + 地面倒影。
 *  设备图按 master 渲染（520×723），这里用 lv_image_set_scale 缩放，使设备显示
 *  高度 ≈ USER_HOME_DEVICE_H（1920 档 660 / 1280 档 440），倒影按同比例贴在脚下。
 *  pivot 设 (0,0)，缩放围绕左上角，便于用绝对坐标精确摆放。 */
lv_obj_t *build_device_placeholder(lv_obj_t *parent)
{
    /* 256 = 100%。让设备显示高度对齐 USER_HOME_DEVICE_H。 */
    const int32_t scale = (256 * USER_HOME_DEVICE_H) / SNOWPOD_DEV_MASTER_H;
    const int32_t dev_w = SNOWPOD_DEV_MASTER_W * scale / 256;
    const int32_t dev_h = SNOWPOD_DEV_MASTER_H * scale / 256;
    const int32_t ref_w = SNOWPOD_REF_MASTER_W * scale / 256;
    const int32_t ref_h = SNOWPOD_REF_MASTER_H * scale / 256;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, dev_w, dev_h + ref_h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    /* 允许子图缩放后超出布局盒而不被裁剪 */
    lv_obj_set_style_clip_corner(box, false, 0);

    /* 设备主体 */
    lv_obj_t *dev = lv_image_create(box);
    lv_image_set_src(dev, &snowpod_device_img);
    lv_image_set_pivot(dev, 0, 0);
    lv_image_set_scale(dev, scale);
    lv_obj_set_pos(dev, 0, 0);

    /* 地面倒影（已预翻转 + 渐隐），贴在设备脚下、水平居中 */
    lv_obj_t *ref = lv_image_create(box);
    lv_image_set_src(ref, &snowpod_reflection_img);
    lv_image_set_pivot(ref, 0, 0);
    lv_image_set_scale(ref, scale);
    lv_obj_set_pos(ref, (dev_w - ref_w) / 2, dev_h);

    return box;
}

/** 中部主区：左 metrics | 中央设备 | 右 metrics */
lv_obj_t *build_main_area(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, USER_PAD_LG, 0);
    lv_obj_set_style_pad_column(row, USER_PAD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 左列：室内 PM2.5 / 舱内 PM2.5
     * 高度锁到中央设备一样高 + 行的 cross-axis 是 center，
     * 三块就会一起垂直居中，底部会留出空间给右下角的开门浮动按钮。 */
    lv_obj_t *col_left = lv_obj_create(row);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_width(col_left, USER_VALUE_LABEL_W);
    lv_obj_set_height(col_left, USER_HOME_DEVICE_H);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_left, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(col_left, LV_OPA_TRANSP, 0);
    ui_user_make_metric_block(col_left, "Indoor PM 2.5", LV_TEXT_ALIGN_LEFT, &g_lbl_pm_indoor);
    ui_user_make_metric_block(col_left, "Cabin PM 2.5", LV_TEXT_ALIGN_LEFT, &g_lbl_pm_cabin);

    /* 中央设备 */
    build_device_placeholder(row);

    /* 右列：设备温度 / 氧气浓度（高度同左列） */
    lv_obj_t *col_right = lv_obj_create(row);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_width(col_right, USER_VALUE_LABEL_W);
    lv_obj_set_height(col_right, USER_HOME_DEVICE_H);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_bg_opa(col_right, LV_OPA_TRANSP, 0);
    ui_user_make_metric_block(col_right, "Temperature", LV_TEXT_ALIGN_RIGHT, &g_lbl_temp);
    ui_user_make_metric_block(col_right, "Oxygen", LV_TEXT_ALIGN_RIGHT, &g_lbl_oxygen);

    return row;
}

/** 右下角浮动 "Slide to Open" 滑动解锁条（真正可拖动；滑到底触发开门） */
lv_obj_t *build_door_floating(lv_obj_t *parent)
{
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text  = "Slide to Open  " LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT;
    cfg.on_unlocked = on_door_slide_unlocked;
    cfg.user_data  = nullptr;
    cfg.track_w    = USER_BTN_DOOR_SZ;       /* 用门控件宽度，贴右下角 */
    cfg.track_h    = USER_BTN_DOOR_ICON_SZ;
    g_door_slide = ui_slide_unlock_create(parent, &cfg);

    lv_obj_t *root = ui_slide_unlock_get_root(g_door_slide);
    if(root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(root, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD, -USER_PAD);
    }
    return root;
}

/* ---------------------------------------------------------------------------
 *  打印中模式：摄像头 + 双底条
 *  内容区结构（位于 g_print_area，全部默认 hidden）：
 *    ┌─────────────────────────────────────────────────┐
 *    │  camera_viewport (flex_grow=1)                  │
 *    │   ├ 占位文字 "摄像头未连接"                       │
 *    │   └ g_camera_image (lv_image, 真实帧到位时显示)   │
 *    ├─────────────────────────────────────────────────┤
 *    │  bottom_row  (两条横排面板)                       │
 *    │   [环境条 A]    [进度条 B + 终止按钮]             │
 *    └─────────────────────────────────────────────────┘
 * ------------------------------------------------------------------------- */
lv_obj_t *make_print_chip(lv_obj_t *parent, const char *caption,
                          lv_obj_t **out_value)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *val = lv_label_create(col);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    if(out_value) {
        *out_value = val;
    }
    return col;
}

lv_obj_t *build_print_env_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_height(panel, USER_PRINT_BAR_H);
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_bg_color(panel, user_color_panel, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, user_color_divider, 0);
    lv_obj_set_style_pad_hor(panel, USER_PRINT_BAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(panel, USER_PRINT_BAR_PAD_VER, 0);
    lv_obj_set_style_pad_column(panel, USER_PRINT_BAR_GAP, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    make_print_chip(panel, "Indoor PM2.5", &g_env_lbl_pm_indoor);
    make_print_chip(panel, "Cabin PM2.5", &g_env_lbl_pm_cabin);
    make_print_chip(panel, "Temperature", &g_env_lbl_temp);
    make_print_chip(panel, "Oxygen",      &g_env_lbl_oxygen);
    return panel;
}

lv_obj_t *build_print_progress_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_height(panel, USER_PRINT_BAR_H);
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_bg_color(panel, user_color_panel, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, user_color_divider, 0);
    lv_obj_set_style_pad_hor(panel, USER_PRINT_BAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(panel, USER_PRINT_BAR_PAD_VER, 0);
    lv_obj_set_style_pad_column(panel, USER_PRINT_BAR_GAP, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：文件名 + 进度条 + 百分比 */
    lv_obj_t *left = lv_obj_create(panel);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    g_prog_file_name = lv_label_create(left);
    lv_label_set_text(g_prog_file_name, "—");
    lv_label_set_long_mode(g_prog_file_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_prog_file_name, LV_PCT(100));
    lv_obj_set_style_text_color(g_prog_file_name, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_prog_file_name, user_font_caption(), 0);

    lv_obj_t *bar_row = lv_obj_create(left);
    lv_obj_remove_style_all(bar_row);
    lv_obj_set_width(bar_row, LV_PCT(100));
    lv_obj_set_height(bar_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(bar_row, LV_OPA_TRANSP, 0);

    g_prog_bar = lv_bar_create(bar_row);
    lv_obj_remove_style_all(g_prog_bar);
    lv_obj_set_size(g_prog_bar, LV_PCT(70), USER_PROGRESS_H);
    lv_obj_set_style_bg_color(g_prog_bar, user_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_prog_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_prog_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_prog_bar, user_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_prog_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_prog_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(g_prog_bar, 0, 100);
    lv_bar_set_value(g_prog_bar, 0, LV_ANIM_OFF);

    g_prog_value = lv_label_create(bar_row);
    lv_label_set_text(g_prog_value, "0%");
    lv_obj_set_style_text_color(g_prog_value, user_color_accent, 0);
    lv_obj_set_style_text_font(g_prog_value, user_font_text(), 0);

    /* 中：剩余时间 */
    g_prog_remaining = lv_label_create(panel);
    lv_label_set_text(g_prog_remaining, "Remaining — min");
    lv_obj_set_style_text_color(g_prog_remaining, user_color_text, 0);
    lv_obj_set_style_text_font(g_prog_remaining, user_font_text(), 0);

    /* 右：终止任务 */
    lv_obj_t *btn = lv_button_create(panel);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_PRINT_TERMINATE_W, USER_PRINT_TERMINATE_H);
    lv_obj_set_style_bg_color(btn, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_terminate_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Terminate");
    lv_obj_set_style_text_color(btn_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(btn_lbl, user_font_text(), 0);
    lv_obj_center(btn_lbl);

    return panel;
}

lv_obj_t *build_printing_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_set_width(area, LV_PCT(100));
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(area, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(area, USER_PAD, 0);
    lv_obj_set_style_pad_row(area, USER_PAD, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 摄像头预览容器（占据剩余高度） */
    g_camera_viewport = lv_obj_create(area);
    lv_obj_remove_style_all(g_camera_viewport);
    lv_obj_set_width(g_camera_viewport, LV_PCT(100));
    lv_obj_set_flex_grow(g_camera_viewport, 1);
    lv_obj_set_style_bg_color(g_camera_viewport, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_camera_viewport, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_camera_viewport, USER_RADIUS, 0);
    lv_obj_set_style_border_width(g_camera_viewport, 1, 0);
    lv_obj_set_style_border_color(g_camera_viewport, user_color_divider, 0);
    lv_obj_clear_flag(g_camera_viewport, LV_OBJ_FLAG_SCROLLABLE);

    /* 占位提示文字 */
    lv_obj_t *hint = lv_label_create(g_camera_viewport);
    lv_label_set_text(hint, LV_SYMBOL_VIDEO "  Camera not connected\n(live cabin feed will appear while printing)");
    lv_obj_set_style_text_color(hint, user_color_text_dim, 0);
    lv_obj_set_style_text_font(hint, user_font_text(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hint);

    /* 真实视频帧 lv_image 占位（默认隐藏） */
    g_camera_image = lv_image_create(g_camera_viewport);
    lv_obj_set_size(g_camera_image, LV_PCT(100), LV_PCT(100));
    lv_obj_center(g_camera_image);
    lv_obj_add_flag(g_camera_image, LV_OBJ_FLAG_HIDDEN);

    /* 底部双面板 */
    lv_obj_t *bottom = lv_obj_create(area);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_height(bottom, USER_PRINT_BAR_H);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bottom, USER_PAD, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    build_print_env_panel(bottom);
    build_print_progress_panel(bottom);

    return area;
}

/* ---------- 数据刷新 ---------- */

const char *status_text(user_home_status_t s)
{
    switch(s) {
    case USER_HOME_STATUS_READY:    return "Ready";
    case USER_HOME_STATUS_PRINTING: return "Printing";
    case USER_HOME_STATUS_ERROR:    return "Error";
    case USER_HOME_STATUS_IDLE:
    default:                        return "Idle";
    }
}

lv_color_t status_color(user_home_status_t s)
{
    switch(s) {
    case USER_HOME_STATUS_PRINTING: return user_color_accent;
    case USER_HOME_STATUS_ERROR:    return ui_color_danger;
    case USER_HOME_STATUS_READY:    return user_color_status_ok;
    case USER_HOME_STATUS_IDLE:
    default:                        return user_color_text_dim;
    }
}

} /* namespace */

void page_user_home_refresh(bool /*force*/)
{
    const user_home_snapshot_t &snap = g_snap;

    if(g_lbl_dev_name) {
        lv_label_set_text(g_lbl_dev_name, snap.device_name);
    }
    if(g_lbl_status) {
        lv_label_set_text(g_lbl_status, status_text(snap.status));
        lv_obj_set_style_text_color(g_lbl_status, status_color(snap.status), 0);
    }
    if(g_dot_status) {
        lv_obj_set_style_bg_color(g_dot_status, status_color(snap.status), 0);
    }
    if(g_lbl_wifi) {
        lv_obj_set_style_text_color(g_lbl_wifi,
            snap.wifi_connected ? user_color_text : user_color_icon_off, 0);
    }
    if(g_lbl_signal) {
        lv_obj_set_style_text_color(g_lbl_signal,
            snap.signal_bars > 0 ? user_color_text : user_color_icon_off, 0);
    }

    if(g_lbl_pm_indoor) {
        lv_label_set_text_fmt(g_lbl_pm_indoor, "%.1f", snap.pm25_indoor);
    }
    if(g_lbl_pm_cabin) {
        lv_label_set_text_fmt(g_lbl_pm_cabin, "%.1f", snap.pm25_cabin);
    }
    if(g_lbl_temp) {
        lv_label_set_text_fmt(g_lbl_temp, "%.1f°C", snap.temperature_c);
    }
    if(g_lbl_oxygen) {
        lv_label_set_text_fmt(g_lbl_oxygen, "%.1f%%", snap.oxygen_percent);
    }
}

lv_obj_t *page_user_home_create(lv_obj_t *parent)
{
    home_env_init();

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page_root = page;

    build_topbar(page);
    g_normal_area = build_main_area(page);

    /* 打印中模式：摄像头 + 双底条；与 g_normal_area 互斥显示。 */
    g_print_area = build_printing_area(page);
    lv_obj_add_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);

    /* 底部胶囊浮动按钮：相对于 page 绝对定位 */
    g_door_float = build_door_floating(page);

    page_user_home_refresh(true);
    return page;
}

void page_user_home_enter_printing_mode(const control_file_t *file)
{
    if(g_printing_active) {
        return;
    }
    g_printing_active = true;

    if(g_normal_area) lv_obj_add_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)  lv_obj_add_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_start)   lv_obj_add_flag(g_btn_start,   LV_OBJ_FLAG_HIDDEN);
    if(g_print_area)  lv_obj_clear_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);

    /* 状态条切到 "正在打印" */
    home_set_status(USER_HOME_STATUS_PRINTING);
    page_user_home_refresh(true);

    if(file && g_prog_file_name) {
        lv_label_set_text(g_prog_file_name, file->name);
    }
    if(g_prog_bar)       lv_bar_set_value(g_prog_bar, 0, LV_ANIM_OFF);
    if(g_prog_value)     lv_label_set_text(g_prog_value, "0%");
    if(g_prog_remaining && file) {
        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
        lv_label_set_text_fmt(g_prog_remaining, "Remaining %s", dur);
    }
}

void page_user_home_exit_printing_mode(void)
{
    if(!g_printing_active) {
        return;
    }
    g_printing_active = false;

    if(g_print_area)  lv_obj_add_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);
    if(g_normal_area) lv_obj_clear_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)  lv_obj_clear_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_start)   lv_obj_clear_flag(g_btn_start,   LV_OBJ_FLAG_HIDDEN);

    home_set_status(USER_HOME_STATUS_READY);
    page_user_home_refresh(true);
}

void page_user_home_printing_update(const control_progress_t *p)
{
    if(!p || !g_printing_active) {
        return;
    }

    if(g_env_lbl_pm_indoor) {
        lv_label_set_text_fmt(g_env_lbl_pm_indoor, "%.1f", (double)p->pm25_indoor);
    }
    if(g_env_lbl_pm_cabin) {
        lv_label_set_text_fmt(g_env_lbl_pm_cabin, "%.1f", (double)p->pm25_cabin);
    }
    if(g_env_lbl_temp) {
        lv_label_set_text_fmt(g_env_lbl_temp, "%.1f°C", (double)p->temperature_c);
    }
    if(g_env_lbl_oxygen) {
        lv_label_set_text_fmt(g_env_lbl_oxygen, "%.1f%%", (double)p->oxygen_percent);
    }

    if(g_prog_bar) {
        lv_bar_set_value(g_prog_bar, p->progress_percent, LV_ANIM_ON);
    }
    if(g_prog_value) {
        lv_label_set_text_fmt(g_prog_value, "%d%%", p->progress_percent);
    }
    if(g_prog_remaining) {
        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), p->remaining_minutes);
        lv_label_set_text_fmt(g_prog_remaining, "Remaining %s", dur);
    }
}
