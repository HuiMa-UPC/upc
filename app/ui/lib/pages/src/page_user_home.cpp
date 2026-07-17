#include "page_user_home.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "ui_msg_proxy.hpp"
#include "camera_preview.hpp"
#include "control.hpp"
#include "home_assets.h"
#include "home_btn_icons.h"
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

void set_pause_ui(bool paused);   /* 定义见下方（打印中底条按钮区） */

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

    /* 暂停/恢复回执：以后端状态为准同步按钮图标 */
    ui::UiMsgProxy::instance().register_print_pause_done(
        [](bool paused) { set_pause_ui(paused); });
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
lv_obj_t *g_topbar       = nullptr;   /**< 顶部状态条根（打印中浮于摄像头之上） */
lv_obj_t *g_lbl_wifi     = nullptr;   /**< 顶栏 WiFi 图标（按连接状态变色） */
lv_obj_t *g_lbl_dev_name = nullptr;
lv_obj_t *g_dot_status   = nullptr;
lv_obj_t *g_lbl_status   = nullptr;
lv_obj_t *g_btn_files    = nullptr;   /**< 右上角 "My Files" 磨砂按钮 */

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
lv_obj_t *g_camera_hint     = nullptr; /**< 占位提示 */
lv_obj_t *g_camera_image    = nullptr; /**< 真实视频帧图像槽（先隐藏） */
lv_obj_t *g_env_lbl_pm_indoor = nullptr;   /**< EXTERIOR PM2.5 数值 */
lv_obj_t *g_env_lbl_pm_cabin  = nullptr;   /**< INTERIOR PM2.5 数值 */
lv_obj_t *g_env_lbl_temp      = nullptr;
lv_obj_t *g_env_lbl_oxygen    = nullptr;
lv_obj_t *g_env_bar_indoor    = nullptr;   /**< EXTERIOR PM2.5 mini 进度条（打印面板） */
lv_obj_t *g_env_bar_cabin     = nullptr;   /**< INTERIOR PM2.5 mini 进度条（打印面板） */
lv_obj_t *g_env_status_indoor = nullptr;   /**< EXTERIOR 状态："● Good"/"● Too poor…" */
lv_obj_t *g_env_status_cabin  = nullptr;   /**< INTERIOR 状态 */
lv_obj_t *g_home_bar_indoor   = nullptr;   /**< 首页 EXTERIOR PM2.5 mini 进度条 */
lv_obj_t *g_home_bar_cabin    = nullptr;   /**< 首页 INTERIOR PM2.5 mini 进度条 */
lv_obj_t *g_prog_bar          = nullptr;
lv_obj_t *g_prog_value        = nullptr; /**< "37%" */
lv_obj_t *g_prog_remaining    = nullptr; /**< "剩余 45 min" */
lv_obj_t *g_prog_file_name    = nullptr;
lv_obj_t *g_btn_pause         = nullptr; /**< 暂停/恢复按钮（终止按钮左侧） */
lv_obj_t *g_pause_icon        = nullptr; /**< 暂停按钮图标（PAUSE ↔ PLAY） */
bool      g_print_paused      = false;   /**< 当前是否暂停（乐观切换，后端回执校正） */
bool      g_printing_active   = false;

/* ---------- 顶栏"查看监控"入口 + 待机监控（摄像头全屏）视图 ---------- */
lv_obj_t *g_btn_camera             = nullptr; /**< 顶栏圆形"查看监控"按钮 */
lv_obj_t *g_monitor_area           = nullptr; /**< 监控浮层（默认隐藏，与 normal/print 互斥） */
lv_obj_t *g_monitor_camera_viewport= nullptr;
lv_obj_t *g_monitor_camera_hint    = nullptr;
lv_obj_t *g_monitor_camera_image   = nullptr; /**< 监控视频帧槽（复用同一 CameraPreviewPlayer） */
lv_obj_t *g_mon_env_pm_ext         = nullptr; /**< 右上环境面板：EXTERIOR PM2.5 数值 */
lv_obj_t *g_mon_env_pm_int         = nullptr; /**< INTERIOR PM2.5 数值 */
lv_obj_t *g_mon_env_o2             = nullptr; /**< O2 CONC. 数值 */
lv_obj_t *g_mon_env_temp           = nullptr; /**< DEVICE TEMP 数值 */
ui_slide_unlock_t *g_monitor_door  = nullptr; /**< 监控界面的滑动开门 */
bool      g_monitor_active         = false;

void apply_camera_preview_layout(uint32_t src_w, uint32_t src_h = 0);
void on_camera_viewport_size_changed(lv_event_t *e);
void enter_monitor_mode(void);
void exit_monitor_mode(void);

/** 预览布局（通用）：宽贴满 viewport，高等比缩放后居中裁剪（解码层仍输出全帧）。
 *  打印区与监控区共用同一算法，只是作用于不同的 viewport/image。 */
static void cam_apply_layout(lv_obj_t *vp, lv_obj_t *img, uint32_t src_w, uint32_t src_h)
{
    if(!img || !vp || src_w == 0) {
        return;
    }

    lv_obj_update_layout(vp);
    const int32_t vp_w = lv_obj_get_width(vp);
    const int32_t vp_h = lv_obj_get_height(vp);
    if(vp_w <= 0 || vp_h <= 0) {
        return;
    }

    lv_obj_set_size(img, vp_w, vp_h);
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    /* cover 填充：取宽/高两方向缩放比的较大者，保证画面铺满 viewport（多出侧居中裁切）。 */
    if(src_h == 0 && ui::CameraPreviewPlayer::instance().active()) {
        src_h = ui::CameraPreviewPlayer::instance().stream_height();
    }
    const int64_t zoom_w = static_cast<int64_t>(vp_w) * 256 / static_cast<int32_t>(src_w);
    const int64_t zoom_h = src_h > 0
                           ? static_cast<int64_t>(vp_h) * 256 / static_cast<int32_t>(src_h)
                           : zoom_w;
    const uint32_t zoom = static_cast<uint32_t>(zoom_w > zoom_h ? zoom_w : zoom_h);
    lv_image_set_scale(img, zoom > 0 ? zoom : LV_SCALE_NONE);
    lv_obj_invalidate(img);
}

void apply_camera_preview_layout(uint32_t src_w, uint32_t src_h)
{
    cam_apply_layout(g_camera_viewport, g_camera_image, src_w, src_h);
}

void on_camera_viewport_size_changed(lv_event_t * /*e*/)
{
    uint32_t src_w = 1024u;
    if(ui::CameraPreviewPlayer::instance().active()) {
        src_w = ui::CameraPreviewPlayer::instance().stream_width();
        if(src_w == 0) {
            src_w = 1024u;
        }
    }
    apply_camera_preview_layout(src_w);
}

void on_monitor_viewport_size_changed(lv_event_t * /*e*/)
{
    uint32_t src_w = 1024u;
    if(ui::CameraPreviewPlayer::instance().active()) {
        src_w = ui::CameraPreviewPlayer::instance().stream_width();
        if(src_w == 0) {
            src_w = 1024u;
        }
    }
    cam_apply_layout(g_monitor_camera_viewport, g_monitor_camera_image, src_w, 0);
}

void on_files_click(lv_event_t *)
{
    /* 设计稿右上角为 "我的文件" 入口。后端文件管理流程后续重建，
     * 暂复用现有打印编排作为占位，保证流程可达。 */
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

/** 同步暂停按钮图标：暂停态显示"播放"图标（表示可恢复），运行态显示"暂停"图标。 */
void set_pause_ui(bool paused)
{
    g_print_paused = paused;
    if(g_pause_icon) {
        lv_label_set_text(g_pause_icon, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

/** 暂停/恢复：乐观切换图标并发请求；后端回执经 register_print_pause_done 校正。 */
void on_pause_click(lv_event_t *)
{
    const bool want_pause = !g_print_paused;
    set_pause_ui(want_pause);
    if(want_pause) {
        ui::UiMsgProxy::instance().send_print_pause();
        ui_toast_show("Print paused", user_color_accent, 1500);
    } else {
        ui::UiMsgProxy::instance().send_print_resume();
        ui_toast_show("Print resumed", user_color_accent, 1500);
    }
}

/** 按钮内图标：A8 掩膜 recolor 成白色，等比缩放到 target；外套固定尺寸盒，
 *  保证在 flex 布局里只占 target×target（避免 96px 源图撑开按钮）。 */
lv_obj_t *make_btn_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t target)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, target, target);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_pivot(img, 48, 48);                 /* 96×96 源图中心 */
    lv_image_set_scale(img, (uint32_t)(256 * target / 96));
    lv_obj_center(img);
    return box;
}

/** 右上角 "Start Print" 主按钮：蓝色对角渐变玻璃胶囊 + 白色亮边 + 图标 + 文字
 *  （对齐 Figma node 2513:19772；无中文字库，文字用英文 "Start Print"）。 */
lv_obj_t *build_files_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    /* 比通用主按钮再高一点（×5/4，贴近 Figma 64px 胶囊），避免过扁。 */
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H * 5 / 4);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    /* 蓝色对角(45°)渐变（对齐 Figma #4054FD → #4D80EE），带少量半透明玻璃感，
     * 与首页滑动解锁材质统一。bg_grad 只存指针，一次性按钮用函数内 static 描述符。 */
    static lv_grad_dsc_t s_btn_grad;
    static bool s_btn_grad_ready = false;
    if(!s_btn_grad_ready) {
        const lv_color_t cols[2] = { lv_color_hex(0x4054FD), lv_color_hex(0x4D80EE) };
        const lv_opa_t   opa[2]  = { LV_OPA_COVER, LV_OPA_COVER };
        const uint8_t    frac[2] = { 0, 255 };
        lv_grad_init_stops(&s_btn_grad, cols, opa, frac, 2);
        lv_grad_linear_init(&s_btn_grad, LV_GRAD_LEFT, LV_GRAD_TOP,
                            LV_GRAD_RIGHT, LV_GRAD_BOTTOM, LV_GRAD_EXTEND_PAD);
        s_btn_grad_ready = true;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4054FD), 0);
    lv_obj_set_style_bg_grad(btn, &s_btn_grad, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);              /* 保留一点通透 */
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);       /* 胶囊 */
    /* 玻璃亮边（近似 Figma 白色渐变描边）+ 柔和蓝色投影 */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(btn, 16, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 4, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x4054FD), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_ver(btn, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, on_files_click, LV_EVENT_CLICKED, nullptr);

    make_btn_icon(btn, &ic_folder, USER_BTN_PRIMARY_H * 5 / 8);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, "Start Print");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);
    lv_obj_set_style_text_font(txt, user_font_text(), 0);

    return btn;
}

void on_camera_btn_click(lv_event_t * /*e*/)
{
    enter_monitor_mode();
}

/** 顶栏"查看监控"圆钮：灰色玻璃 + 摄像头图标（次级样式，位于"Start Print"左侧）。 */
lv_obj_t *build_camera_button(lv_obj_t *parent)
{
    const int32_t sz = USER_BTN_PRIMARY_H * 5 / 4;   /* 与 Start Print 同高，圆形 */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x757575), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, on_camera_btn_click, LV_EVENT_CLICKED, nullptr);

    make_btn_icon(btn, &ic_camera, USER_BTN_PRIMARY_H * 5 / 8);

    return btn;
}

/** 顶部状态条：左侧 设备名 + 状态点 + 状态文字；右侧 "查看监控"圆钮 + "Start Print" 按钮 */
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

    lv_obj_t *lbl_wifi = lv_label_create(bar);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi, user_color_text, 0);
    lv_obj_set_style_text_font(lbl_wifi, user_font_text(), 0);
    g_lbl_wifi = lbl_wifi;

    g_lbl_dev_name = lv_label_create(bar);
    lv_label_set_text(g_lbl_dev_name, "SNOWPOD A1");
    lv_obj_set_style_text_color(g_lbl_dev_name, user_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_dev_name, user_font_text(), 0);

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

    /* 用一个 flex_grow 撑开把按钮顶到右边 */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    g_btn_camera = build_camera_button(bar);
    g_btn_files = build_files_button(bar);

    return bar;
}

/* 首页环境数值块（对齐 Figma 1.1.1）：顶部细分隔线 + 图标+标题 + 大数值+小单位
 * +（PM 才有）mini 进度条。right=true 时整体右对齐（右列 温度/氧气）。 */
lv_obj_t *home_make_metric(lv_obj_t *parent, const lv_image_dsc_t *icon,
                           const char *caption, const char *unit,
                           bool right, bool with_bar,
                           lv_obj_t **out_val, lv_obj_t **out_bar)
{
    /* 外层竖列：顶部细线 +（图标+文本列）行 +（PM 才有）mini 进度条。
     * right=true 时把"图标+文本"整组在块内靠右对齐（图标仍在左，仅整组贴右）。 */
    const lv_flex_align_t group_align = right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START;

    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, USER_VALUE_LABEL_W + USER_PAD_LG);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, group_align, group_align);
    lv_obj_set_style_pad_row(col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部细分隔线 */
    lv_obj_t *div = lv_obj_create(col);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, user_color_text, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_30, 0);
    lv_obj_set_style_margin_bottom(div, USER_PAD_SM, 0);

    /* 行：[图标] [文本列]，图标相对 标题+数值 垂直居中（对齐 Figma 2824:3882）。 */
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if(icon) {
        lv_obj_t *ic = lv_image_create(row);
        lv_image_set_src(ic, icon);
        lv_obj_set_style_image_recolor(ic, user_color_text_dim, 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
    }

    /* 文本列：标题（上，字距近似 0.1em）+ 数值行（下） */
    lv_obj_t *tcol = lv_obj_create(row);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_size(tcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tcol, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(tcol, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(tcol, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(tcol);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);
    lv_obj_set_style_text_letter_space(cap, 2, 0);

    /* 大数值 + 小单位（底对齐） */
    lv_obj_t *vrow = lv_obj_create(tcol);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vrow, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(vrow);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_value(), 0);
    if(out_val) *out_val = val;

    lv_obj_t *u = lv_label_create(vrow);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, user_color_text_dim, 0);
    lv_obj_set_style_text_font(u, user_font_caption(), 0);
    lv_obj_set_style_margin_bottom(u, USER_PAD_SM, 0);

    if(with_bar) {
        lv_obj_t *bar = lv_bar_create(col);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), USER_PROGRESS_H / 2 > 0 ? USER_PROGRESS_H / 2 : 6);
        lv_obj_set_style_bg_color(bar, user_color_panel_alt, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, user_color_accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        if(out_bar) *out_bar = bar;
    }
    return col;
}

/** 中部主区：全屏整机背景（RGB565）+ 左右环境数值浮层。
 *  area 为覆盖整个内容区的浮层（含状态栏区域），整机背景一直顶到最上；
 *  状态栏在其后创建 → 浮于背景之上，顶部不再留黑边。 */
lv_obj_t *build_main_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(area, 0, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 整机背景大图：铺满 area、等比 cover、居中（源 1120x700） */
    lv_obj_t *bg = lv_image_create(area);
    lv_image_set_src(bg, &home_bg);
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(bg, LV_IMAGE_ALIGN_COVER);
    lv_obj_center(bg);

    /* 左列环境浮层：EXTERIOR / INTERIOR PM2.5（图标 + 数值 + mini 进度条） */
    lv_obj_t *col_left = lv_obj_create(area);
    lv_obj_remove_style_all(col_left);
    lv_obj_add_flag(col_left, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(col_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_left, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(col_left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(col_left, LV_ALIGN_LEFT_MID, USER_PAD_LG, 0);
    home_make_metric(col_left, &ic_pm, "EXTERIOR PM2.5", "ug/m3",
                     false, true, &g_lbl_pm_indoor, &g_home_bar_indoor);
    home_make_metric(col_left, &ic_pm, "INTERIOR PM2.5", "ug/m3",
                     false, true, &g_lbl_pm_cabin, &g_home_bar_cabin);

    /* 右列环境浮层：DEVICE TEMP / O2 CONC.（图标 + 数值，无进度条；
     * 两块统一"图标在左、左对齐"，使温度与氧气浓度上下对齐） */
    lv_obj_t *col_right = lv_obj_create(area);
    lv_obj_remove_style_all(col_right);
    lv_obj_add_flag(col_right, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(col_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(col_right, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(col_right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(col_right, LV_ALIGN_RIGHT_MID, -USER_PAD_LG, 0);
    home_make_metric(col_right, &ic_temp, "DEVICE TEMP", "\xC2\xB0""C",
                     false, false, &g_lbl_temp, nullptr);
    home_make_metric(col_right, &ic_o2, "O2 CONC.", "%",
                     false, false, &g_lbl_oxygen, nullptr);

    return area;
}

/** 右下角浮动 "Slide to Open" 滑动解锁条（真正可拖动；滑到底触发开门） */
lv_obj_t *build_door_floating(lv_obj_t *parent)
{
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text  = "SWIPE TO OPEN";   /* 箭头由控件绘制 3 个 chevron */
    cfg.on_unlocked = on_door_slide_unlocked;
    cfg.user_data  = nullptr;
    /* 首页开门滑块放大 ~1.33×（更贴设计稿 432×96 的大胶囊，且更好按）。
     * 仅放大首页这处；USER_SLIDE_W 仍被打印前"未放入"弹窗复用，保持原尺寸。 */
    cfg.track_w    = USER_SLIDE_W * 4 / 3;
    cfg.track_h    = USER_BTN_DOOR_ICON_SZ * 4 / 3;
    cfg.custom_accent = true;                /* 设计稿为红色渐变滑块 */
    cfg.accent_color  = user_color_status_err;
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
/* 1px 竖分隔线 */
lv_obj_t *make_v_divider(lv_obj_t *parent)
{
    lv_obj_t *ln = lv_obj_create(parent);
    lv_obj_remove_style_all(ln);
    lv_obj_set_size(ln, 1, LV_PCT(72));
    lv_obj_set_style_bg_color(ln, user_color_divider, 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
    return ln;
}

/* PM2.5 块：标题 + 大数值+单位 + mini 进度条 + 状态文案（Good / Too poor…） */
lv_obj_t *make_pm_block(lv_obj_t *parent, const char *caption,
                        lv_obj_t **out_val, lv_obj_t **out_bar, lv_obj_t **out_status)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    /* 数值 + 单位 */
    lv_obj_t *vrow = lv_obj_create(col);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vrow, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(vrow);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_value(), 0);
    if(out_val) *out_val = val;

    lv_obj_t *unit = lv_label_create(vrow);
    lv_label_set_text(unit, "ug/m3");
    lv_obj_set_style_text_color(unit, user_color_text_dim, 0);
    lv_obj_set_style_text_font(unit, user_font_caption(), 0);

    /* mini 进度条 */
    lv_obj_t *bar = lv_bar_create(col);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(90), USER_PROGRESS_H / 2 > 0 ? USER_PROGRESS_H / 2 : 6);
    lv_obj_set_style_bg_color(bar, user_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, user_color_status_ok, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    if(out_bar) *out_bar = bar;

    lv_obj_t *st = lv_label_create(col);
    lv_label_set_text(st, LV_SYMBOL_OK "  Good");
    lv_obj_set_style_text_color(st, user_color_status_ok, 0);
    lv_obj_set_style_text_font(st, user_font_caption(), 0);
    if(out_status) *out_status = st;

    return col;
}

/* 小 kv 卡：标题 + 数值（设备温度 / 氧气浓度） */
lv_obj_t *make_env_kv(lv_obj_t *parent, const char *caption, lv_obj_t **out_val)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 2, 0);
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
    if(out_val) *out_val = val;
    return col;
}

lv_obj_t *build_print_env_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(panel, 3);   /* 左:右 ≈ 3:2（对齐 Figma 656:420） */
    lv_obj_set_style_bg_color(panel, user_color_panel, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, user_color_divider, 0);
    lv_obj_set_style_pad_hor(panel, USER_PRINT_BAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(panel, USER_PRINT_BAR_PAD_VER, 0);
    lv_obj_set_style_pad_column(panel, USER_PRINT_BAR_GAP, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    make_pm_block(panel, "EXTERIOR PM2.5", &g_env_lbl_pm_indoor,
                  &g_env_bar_indoor, &g_env_status_indoor);
    make_v_divider(panel);
    make_pm_block(panel, "INTERIOR PM2.5", &g_env_lbl_pm_cabin,
                  &g_env_bar_cabin, &g_env_status_cabin);

    make_v_divider(panel);

    /* 温度 / 氧气 两个小 kv 竖排 */
    lv_obj_t *kv = lv_obj_create(panel);
    lv_obj_remove_style_all(kv);
    lv_obj_set_size(kv, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(kv, USER_PAD, 0);
    lv_obj_set_style_bg_opa(kv, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(kv, LV_OBJ_FLAG_SCROLLABLE);
    make_env_kv(kv, "DEVICE TEMP", &g_env_lbl_temp);
    make_env_kv(kv, "O2 CONCENTRATION", &g_env_lbl_oxygen);

    return panel;
}

/* 进度面板内的一个 kv（TIME LEFT / PRINT PROGRESS）：小标题 + 数值 */
lv_obj_t *make_prog_kv(lv_obj_t *parent, const char *caption,
                       lv_color_t val_color, lv_obj_t **out_val)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *val = lv_label_create(col);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, val_color, 0);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    if(out_val) *out_val = val;
    return col;
}

lv_obj_t *build_print_progress_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(panel, 2);   /* 左:右 ≈ 3:2 */
    lv_obj_set_style_bg_color(panel, user_color_panel, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, user_color_divider, 0);
    lv_obj_set_style_pad_hor(panel, USER_PRINT_BAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(panel, USER_PRINT_BAR_PAD_VER, 0);
    lv_obj_set_style_pad_column(panel, USER_PAD, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 缩略图（打印件预览占位：中性灰圆角块 + 图片图标，方形） */
    const int32_t thumb_sz = USER_PRINT_TERMINATE_H + USER_PRINT_BAR_PAD_VER * 2;
    lv_obj_t *thumb = lv_obj_create(panel);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, thumb_sz, thumb_sz);
    lv_obj_set_style_bg_color(thumb, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, USER_RADIUS, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *thumb_icon = lv_label_create(thumb);
    lv_label_set_text(thumb_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(thumb_icon, user_color_text_dim, 0);
    lv_obj_set_style_text_font(thumb_icon, user_font_value(), 0);
    lv_obj_center(thumb_icon);

    /* 中列：进度条 + (TIME LEFT / PRINT PROGRESS) */
    lv_obj_t *mid = lv_obj_create(panel);
    lv_obj_remove_style_all(mid);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    g_prog_bar = lv_bar_create(mid);
    lv_obj_remove_style_all(g_prog_bar);
    lv_obj_set_size(g_prog_bar, LV_PCT(100), USER_PROGRESS_H / 2 > 0 ? USER_PROGRESS_H / 2 : 8);
    lv_obj_set_style_bg_color(g_prog_bar, user_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_prog_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_prog_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_prog_bar, user_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_prog_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_prog_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(g_prog_bar, 0, 100);
    lv_bar_set_value(g_prog_bar, 0, LV_ANIM_OFF);

    lv_obj_t *kv_row = lv_obj_create(mid);
    lv_obj_remove_style_all(kv_row);
    lv_obj_set_width(kv_row, LV_PCT(100));
    lv_obj_set_height(kv_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kv_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kv_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(kv_row, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(kv_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(kv_row, LV_OBJ_FLAG_SCROLLABLE);
    make_prog_kv(kv_row, "TIME LEFT", user_color_text, &g_prog_remaining);
    make_prog_kv(kv_row, "PROGRESS", user_color_accent, &g_prog_value);
    /* 文件名不在打印底条显示（贴 Figma）；保留句柄避免刷新逻辑空指针 */
    g_prog_file_name = nullptr;

    /* 右：暂停 + 终止（竖排两颗，节省横向空间；暂停在上/左，终止其后） */
    g_btn_pause = lv_button_create(panel);
    lv_obj_remove_style_all(g_btn_pause);
    lv_obj_set_size(g_btn_pause, USER_PRINT_TERMINATE_H, USER_PRINT_TERMINATE_H);
    lv_obj_set_style_bg_color(g_btn_pause, user_color_accent, 0);
    lv_obj_set_style_bg_opa(g_btn_pause, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_pause, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(g_btn_pause, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_btn_pause, 0, 0);
    lv_obj_add_event_cb(g_btn_pause, on_pause_click, LV_EVENT_CLICKED, nullptr);
    g_pause_icon = lv_label_create(g_btn_pause);
    lv_label_set_text(g_pause_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(g_pause_icon, user_color_text, 0);
    lv_obj_set_style_text_font(g_pause_icon, user_font_text(), 0);
    lv_obj_center(g_pause_icon);

    /* 右：终止任务（圆形，仅图标，避免文字过宽被裁） */
    lv_obj_t *btn = lv_button_create(panel);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_PRINT_TERMINATE_H, USER_PRINT_TERMINATE_H);
    lv_obj_set_style_bg_color(btn, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_terminate_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(btn_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(btn_lbl, user_font_text(), 0);
    lv_obj_center(btn_lbl);

    return panel;
}

lv_obj_t *build_printing_area(lv_obj_t *parent)
{
    /* 打印中：整块内容区全屏铺满。area 为浮层（脱离首页纵向 flex），
     * 覆盖包括状态栏在内的整个内容区；摄像头做背景，状态栏/底部面板浮于其上。 */
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 摄像头预览容器：铺满 area（全屏背景，无圆角/边框） */
    g_camera_viewport = lv_obj_create(area);
    lv_obj_remove_style_all(g_camera_viewport);
    lv_obj_set_size(g_camera_viewport, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_camera_viewport, 0, 0);
    lv_obj_set_style_bg_color(g_camera_viewport, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_camera_viewport, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_camera_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_camera_viewport, on_camera_viewport_size_changed,
                        LV_EVENT_SIZE_CHANGED, nullptr);

    /* 占位提示文字 */
    g_camera_hint = lv_label_create(g_camera_viewport);
    lv_label_set_text(g_camera_hint, LV_SYMBOL_VIDEO "  Camera not connected\n(live cabin feed will appear while printing)");
    lv_obj_set_style_text_color(g_camera_hint, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_camera_hint, user_font_text(), 0);
    lv_obj_set_style_text_align(g_camera_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(g_camera_hint);

    /* 真实视频帧 lv_image 占位（默认隐藏） */
    g_camera_image = lv_image_create(g_camera_viewport);
    lv_obj_add_flag(g_camera_image, LV_OBJ_FLAG_HIDDEN);
    apply_camera_preview_layout(1024u);

    /* 底部双面板：浮于摄像头之上，贴底居中，两侧留内边距。
     * 高度自适应内容（cross STRETCH 使两块等高），避免固定高度裁切文字。 */
    lv_obj_t *bottom = lv_obj_create(area);
    lv_obj_remove_style_all(bottom);
    lv_obj_add_flag(bottom, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_height(bottom, LV_SIZE_CONTENT);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -USER_PAD);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bottom, USER_PAD, 0);
    lv_obj_set_style_pad_column(bottom, USER_PAD, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    build_print_env_panel(bottom);
    build_print_progress_panel(bottom);

    return area;
}

/* ============================================================================
 *  待机"查看监控"：全屏摄像头 + 右上环境面板 + 左下 EXIT + 右下滑动开门
 * ============================================================================ */

/* 环境面板单列：标题(上) + 数值+单位(下)；value 输出到 *out_val。 */
lv_obj_t *monitor_env_col(lv_obj_t *parent, const char *caption,
                          const char *unit, lv_obj_t **out_val)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);
    lv_obj_set_style_text_letter_space(cap, 1, 0);

    lv_obj_t *vrow = lv_obj_create(col);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vrow, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(vrow);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    if(out_val) *out_val = val;

    if(unit) {
        lv_obj_t *u = lv_label_create(vrow);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_color(u, user_color_text_dim, 0);
        lv_obj_set_style_text_font(u, user_font_caption(), 0);
    }
    return col;
}

/* 右上环境玻璃面板（4 列）。LVGL 无背景模糊 → 半透明深底 + 细白描边近似玻璃。 */
lv_obj_t *build_env_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, USER_PAD, 0);
    lv_obj_set_style_pad_column(panel, USER_PAD_LG, 0);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    monitor_env_col(panel, "EXTERIOR PM2.5", "ug/m3", &g_mon_env_pm_ext);
    monitor_env_col(panel, "INTERIOR PM2.5", "ug/m3", &g_mon_env_pm_int);
    monitor_env_col(panel, "O2 CONC.", "%", &g_mon_env_o2);
    monitor_env_col(panel, "DEVICE TEMP", "\xC2\xB0""C", &g_mon_env_temp);
    return panel;
}

void on_monitor_exit_click(lv_event_t * /*e*/)
{
    exit_monitor_mode();
}

/* EXIT 灰玻璃按钮（图标 + 文字），左下角 */
lv_obj_t *build_monitor_exit_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H * 5 / 4);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x757575), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(btn, on_monitor_exit_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, "EXIT");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);
    lv_obj_set_style_text_font(txt, user_font_text(), 0);
    return btn;
}

/* 监控界面滑动开门：解锁触发开门，短延时弹回复位（与首页一致） */
void monitor_door_reset_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if(g_monitor_door) {
        ui_slide_unlock_reset(g_monitor_door);
    }
}

void on_monitor_door_unlocked(void * /*u*/)
{
    home_trigger_door_open();
    ui_toast_show("Open Door", user_color_accent, 1500);
    lv_timer_t *t = lv_timer_create(monitor_door_reset_cb, 700, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

lv_obj_t *build_monitor_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 摄像头全屏背景 */
    g_monitor_camera_viewport = lv_obj_create(area);
    lv_obj_remove_style_all(g_monitor_camera_viewport);
    lv_obj_set_size(g_monitor_camera_viewport, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_monitor_camera_viewport, 0, 0);
    lv_obj_set_style_bg_color(g_monitor_camera_viewport, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_monitor_camera_viewport, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_monitor_camera_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_monitor_camera_viewport, on_monitor_viewport_size_changed,
                        LV_EVENT_SIZE_CHANGED, nullptr);

    g_monitor_camera_hint = lv_label_create(g_monitor_camera_viewport);
    lv_label_set_text(g_monitor_camera_hint, LV_SYMBOL_VIDEO "  Camera not connected\n(live cabin feed will appear here)");
    lv_obj_set_style_text_color(g_monitor_camera_hint, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_monitor_camera_hint, user_font_text(), 0);
    lv_obj_set_style_text_align(g_monitor_camera_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(g_monitor_camera_hint);

    g_monitor_camera_image = lv_image_create(g_monitor_camera_viewport);
    lv_obj_add_flag(g_monitor_camera_image, LV_OBJ_FLAG_HIDDEN);

    /* 右上：环境小面板 */
    lv_obj_t *env = build_env_panel(area);
    lv_obj_add_flag(env, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(env, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);

    /* 左下：EXIT */
    lv_obj_t *exit_btn = build_monitor_exit_button(area);
    lv_obj_add_flag(exit_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(exit_btn, LV_ALIGN_BOTTOM_LEFT, USER_PAD, -USER_PAD);

    /* 右下：滑动开门（与首页同尺寸/配色） */
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text     = "SWIPE TO OPEN";
    cfg.on_unlocked   = on_monitor_door_unlocked;
    cfg.user_data     = nullptr;
    cfg.track_w       = USER_SLIDE_W * 4 / 3;
    cfg.track_h       = USER_BTN_DOOR_ICON_SZ * 4 / 3;
    cfg.custom_accent = true;
    cfg.accent_color  = user_color_status_err;
    g_monitor_door = ui_slide_unlock_create(area, &cfg);
    lv_obj_t *door_root = ui_slide_unlock_get_root(g_monitor_door);
    if(door_root) {
        lv_obj_add_flag(door_root, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(door_root, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD, -USER_PAD);
    }

    return area;
}

static void deferred_monitor_camera_start_cb(void * /*user*/)
{
    ui::CameraPreviewPlayer::instance().start(
        g_monitor_camera_image, g_monitor_camera_hint, "monitor");
}

void enter_monitor_mode(void)
{
    if(g_monitor_active || g_printing_active) {
        return;
    }
    g_monitor_active = true;

    if(g_normal_area)  lv_obj_add_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)   lv_obj_add_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_files)    lv_obj_add_flag(g_btn_files,   LV_OBJ_FLAG_HIDDEN);
    if(g_btn_camera)   lv_obj_add_flag(g_btn_camera,  LV_OBJ_FLAG_HIDDEN);
    if(g_monitor_area) lv_obj_clear_flag(g_monitor_area, LV_OBJ_FLAG_HIDDEN);
    /* 监控层移到背景：顶部状态栏（保留设备状态）与左侧导航栏自然浮于其上。 */
    if(g_monitor_area) lv_obj_move_background(g_monitor_area);
    if(g_monitor_door) ui_slide_unlock_reset(g_monitor_door);

    page_user_home_refresh(true);
    cam_apply_layout(g_monitor_camera_viewport, g_monitor_camera_image, 1024u, 0u);
    lv_async_call(deferred_monitor_camera_start_cb, nullptr);
}

void exit_monitor_mode(void)
{
    ui::CameraPreviewPlayer::instance().stop();
    if(!g_monitor_active) {
        return;
    }
    g_monitor_active = false;

    if(g_monitor_area) lv_obj_add_flag(g_monitor_area, LV_OBJ_FLAG_HIDDEN);
    if(g_normal_area)  lv_obj_clear_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)   lv_obj_clear_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_files)    lv_obj_clear_flag(g_btn_files,   LV_OBJ_FLAG_HIDDEN);
    if(g_btn_camera)   lv_obj_clear_flag(g_btn_camera,  LV_OBJ_FLAG_HIDDEN);

    page_user_home_refresh(true);
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

/* PM2.5 开门阈值（与 Glavo handle_pm25_request 的 pm25_cabin>35 一致） */
constexpr float kPmGateThreshold = 35.0f;
/* mini 进度条满量程（μg/m³）：用于把浓度映射为条形填充百分比 */
constexpr float kPmBarFullScale  = 100.0f;

/** 刷新单个 PM2.5 块：数值、进度条填充 + 颜色、状态文案（Good / Too poor…）。 */
void update_pm_block(lv_obj_t *val_lbl, lv_obj_t *bar, lv_obj_t *status_lbl, float pm)
{
    if(val_lbl) {
        lv_label_set_text_fmt(val_lbl, "%.1f", pm);
    }
    const bool ok = (pm <= kPmGateThreshold);
    const lv_color_t c = ok ? user_color_status_ok : user_color_status_err;
    if(bar) {
        int fill = static_cast<int>(pm * 100.0f / kPmBarFullScale + 0.5f);
        if(fill < 0)   fill = 0;
        if(fill > 100) fill = 100;
        lv_bar_set_value(bar, fill, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
    }
    if(status_lbl) {
        lv_label_set_text(status_lbl,
                          ok ? LV_SYMBOL_OK "  Good"
                             : LV_SYMBOL_WARNING "  Too poor to open the gate");
        lv_obj_set_style_text_color(status_lbl, c, 0);
    }
}

} /* namespace */

void page_user_home_update_camera_preview_layout(uint32_t src_w, uint32_t src_h)
{
    apply_camera_preview_layout(src_w, src_h);
}

void page_user_home_refresh(bool /*force*/)
{
    const user_home_snapshot_t &snap = g_snap;

    if(g_lbl_wifi) {
        /* 连上=正常白，断开=灰暗，作为顶栏 WiFi 连接状态指示 */
        lv_obj_set_style_text_color(g_lbl_wifi,
            snap.wifi_connected ? user_color_text : user_color_text_dim, 0);
    }

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

    /* 首页环境（单位为独立静态标签，数值只填数字）；PM 块带 mini 进度条 + 阈值配色 */
    update_pm_block(g_lbl_pm_indoor, g_home_bar_indoor, nullptr, snap.pm25_indoor);
    update_pm_block(g_lbl_pm_cabin,  g_home_bar_cabin,  nullptr, snap.pm25_cabin);
    if(g_lbl_temp) {
        lv_label_set_text_fmt(g_lbl_temp, "%.1f", snap.temperature_c);
    }
    if(g_lbl_oxygen) {
        lv_label_set_text_fmt(g_lbl_oxygen, "%.1f", snap.oxygen_percent);
    }

    /* 打印模式下的环境标签也由同一快照驱动（串口 0x9B → APP_Update_Ntf） */
    update_pm_block(g_env_lbl_pm_indoor, g_env_bar_indoor, g_env_status_indoor,
                    snap.pm25_indoor);
    update_pm_block(g_env_lbl_pm_cabin, g_env_bar_cabin, g_env_status_cabin,
                    snap.pm25_cabin);
    if(g_env_lbl_temp) {
        lv_label_set_text_fmt(g_env_lbl_temp, "%.1f °C", snap.temperature_c);
    }
    if(g_env_lbl_oxygen) {
        lv_label_set_text_fmt(g_env_lbl_oxygen, "%.1f %%", snap.oxygen_percent);
    }

    /* 待机查看监控：右上环境小面板（单位为独立标签，数值只填数字） */
    if(g_mon_env_pm_ext) lv_label_set_text_fmt(g_mon_env_pm_ext, "%.1f", snap.pm25_indoor);
    if(g_mon_env_pm_int) lv_label_set_text_fmt(g_mon_env_pm_int, "%.1f", snap.pm25_cabin);
    if(g_mon_env_o2)     lv_label_set_text_fmt(g_mon_env_o2,     "%.1f", snap.oxygen_percent);
    if(g_mon_env_temp)   lv_label_set_text_fmt(g_mon_env_temp,   "%.1f", snap.temperature_c);
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

    /* 先建首页主区（全屏整机背景浮层，置于最底），再建状态栏——状态栏遂浮于背景之上。 */
    g_normal_area = build_main_area(page);
    g_topbar = build_topbar(page);

    /* 打印中模式：摄像头 + 双底条；与 g_normal_area 互斥显示。 */
    g_print_area = build_printing_area(page);
    lv_obj_add_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);

    /* 待机查看监控层：摄像头全屏 + 右上环境面板 + 左下 EXIT + 右下开门；默认隐藏。 */
    g_monitor_area = build_monitor_area(page);
    lv_obj_add_flag(g_monitor_area, LV_OBJ_FLAG_HIDDEN);

    /* 底部胶囊浮动按钮：相对于 page 绝对定位 */
    g_door_float = build_door_floating(page);

    page_user_home_refresh(true);
    return page;
}

static void deferred_camera_start_cb(void* /*user*/)
{
    ui::CameraPreviewPlayer::instance().start(
        g_camera_image, g_camera_hint, "print");
}

void page_user_home_request_camera_preview(void)
{
    lv_async_call(deferred_camera_start_cb, nullptr);
}

void page_user_home_enter_printing_mode(const control_file_t *file)
{
    if(g_printing_active) {
        return;
    }
    /* 若正在查看监控，先退出（CameraPreviewPlayer 单例，二者不能同时用） */
    if(g_monitor_active) {
        exit_monitor_mode();
    }
    g_printing_active = true;

    if(g_normal_area) lv_obj_add_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)  lv_obj_add_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_files)   lv_obj_add_flag(g_btn_files,   LV_OBJ_FLAG_HIDDEN);
    if(g_btn_camera)  lv_obj_add_flag(g_btn_camera,  LV_OBJ_FLAG_HIDDEN);
    if(g_print_area)  lv_obj_clear_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);
    /* 摄像头浮层置于最底层：状态栏/其它 sibling 自然浮于其上。
     * 注意：不能用 move_foreground(topbar)——那会改变 topbar 在纵向 flex 中的
     * 子节点顺序，导致退出打印后状态栏跑到页面底部。print_area 是 FLOATING，
     * 移到背景只改 z 序、不影响布局。 */
    if(g_print_area)  lv_obj_move_background(g_print_area);

    /* 复位暂停按钮为"运行"态 */
    set_pause_ui(false);

    /* 状态条切到 "正在打印" */
    home_set_status(USER_HOME_STATUS_PRINTING);
    page_user_home_refresh(true);
    page_user_home_update_camera_preview_layout(1024u, 0u);

    if(file && g_prog_file_name) {
        lv_label_set_text(g_prog_file_name, file->name);
    }
    if(g_prog_bar)       lv_bar_set_value(g_prog_bar, 0, LV_ANIM_OFF);
    if(g_prog_value)     lv_label_set_text(g_prog_value, "0%");
    if(g_prog_remaining && file) {
        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
        lv_label_set_text(g_prog_remaining, dur);
    }

    page_user_home_request_camera_preview();
}

void page_user_home_exit_printing_mode(void)
{
    ui::CameraPreviewPlayer::instance().stop();
    g_printing_active = false;

    if(g_print_area)  lv_obj_add_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);
    if(g_normal_area) lv_obj_clear_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)  lv_obj_clear_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_files)   lv_obj_clear_flag(g_btn_files,   LV_OBJ_FLAG_HIDDEN);
    if(g_btn_camera)  lv_obj_clear_flag(g_btn_camera,  LV_OBJ_FLAG_HIDDEN);
    set_pause_ui(false);

    home_set_status(USER_HOME_STATUS_READY);
    page_user_home_refresh(true);
}

void page_user_home_printing_update(const control_progress_t *p)
{
    if(!p || !g_printing_active) {
        return;
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
        lv_label_set_text(g_prog_remaining, dur);
    }
}
