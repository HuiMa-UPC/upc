#include "page_user_home.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "ui_msg_proxy.hpp"
#include "camera_msg_proxy.hpp"
#include "camera_preview.hpp"
#include "control.hpp"
#include "home_assets.h"
#include "home_btn_icons.h"
#include "home_metric_icons.h"
#include "ui_slide_unlock.hpp"
#include "config/cal_config.hpp"
#include "config/camera_resolution_profile.hpp"

#include <cmath>
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

/* 前四项与后端 EnvSnapshotPayload.status 数值对应（见 apply_env 的强转），
 * 后两项仅由前端在打印结果页本地置位，后端不会推送。 */
typedef enum {
    USER_HOME_STATUS_IDLE = 0,
    USER_HOME_STATUS_READY,
    USER_HOME_STATUS_PRINTING,
    USER_HOME_STATUS_ERROR,
    USER_HOME_STATUS_PRINT_DONE,       /**< 打印完成（结果页顶栏绿点） */
    USER_HOME_STATUS_PRINT_TERMINATED, /**< 打印终止（结果页顶栏红点） */
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
    float              pm10_indoor;      /**< 室内 PM10（<0/NaN=无数据） */
    float              humidity_percent; /**< 舱内湿度（<0/NaN=无数据） */
    float              pressure_kpa;     /**< 舱内气压（<0/NaN=无数据） */
} user_home_snapshot_t;

/** 数值无效（后端未采集）：负值或 NaN → UI 显示 "--"。 */
static inline bool metric_invalid(float v) { return std::isnan(v) || v < 0.0f; }

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
    g_snap.pm10_indoor    = p.pm10_indoor;
    g_snap.humidity_percent = p.humidity_percent;
    g_snap.pressure_kpa   = p.pressure_kpa;
    page_user_home_refresh(false);
}

void set_pause_ui(bool paused);   /* 定义见下方（打印中底条按钮区） */
extern lv_obj_t *g_monitor_photo_btn;
extern lv_obj_t *g_result_photo_btn;
extern lv_obj_t *g_result_purify_btn;
extern bool      g_result_cleaning;
extern bool      g_result_purify_done;
extern bool      g_monitor_photo_pending;
extern char      g_result_job_name[96];

struct CleanDoneToastCtx {
    bool success;
};

void on_clean_start_reject_async(void * /*user*/)
{
    g_result_cleaning = false;
    if(g_result_purify_btn && !g_result_purify_done) {
        lv_obj_clear_state(g_result_purify_btn, LV_STATE_DISABLED);
    }
    ui_toast_show("Powder cleaning is busy", ui_color_danger, 1800);
}

void on_clean_done_async(void *user)
{
    CleanDoneToastCtx *ctx = static_cast<CleanDoneToastCtx*>(user);
    const bool success = ctx ? ctx->success : false;
    delete ctx;

    g_result_cleaning = false;
    if(!control_is_printing()) {
        return;
    }
    if(success) {
        g_result_purify_done = true;
        if(g_result_purify_btn) {
            lv_obj_add_state(g_result_purify_btn, LV_STATE_DISABLED);
        }
        ui_toast_show("Powder cleaning finished", user_color_status_ok, 2200);
    } else {
        if(g_result_purify_btn && !g_result_purify_done) {
            lv_obj_clear_state(g_result_purify_btn, LV_STATE_DISABLED);
        }
        ui_toast_show("Powder cleaning failed, video retained", ui_color_danger, 2600);
    }
}

void on_clean_close_rsp_async(void * /*user*/)
{
    g_result_cleaning = false;
    if(g_result_purify_btn && !g_result_purify_done) {
        lv_obj_clear_state(g_result_purify_btn, LV_STATE_DISABLED);
    }
}

void home_env_init(void)
{
    if(!g_env_defaults_loaded) {
        std::memset(&g_snap, 0, sizeof(g_snap));
        std::strcpy(g_snap.device_name, upc::config::kDeviceDisplayName);
        g_snap.status         = USER_HOME_STATUS_READY;
        g_snap.wifi_connected = false;
        g_snap.signal_bars    = 0;
        /* 环境指标默认无数据（<0）→ 首帧环境到达前统一显示 "--" */
        g_snap.pm25_indoor    = -1.0f;
        g_snap.pm25_cabin     = -1.0f;
        g_snap.temperature_c  = -1.0f;
        g_snap.oxygen_percent = -1.0f;
        g_snap.pm10_indoor    = -1.0f;
        g_snap.humidity_percent = -1.0f;
        g_snap.pressure_kpa   = -1.0f;
        g_env_defaults_loaded = true;
    }
    ui::UiMsgProxy::instance().register_env_update(
        [](const middleware::EnvSnapshotPayload &p) { apply_env(p); });

    /* 暂停/恢复回执：以后端状态为准同步按钮图标 */
    ui::UiMsgProxy::instance().register_print_pause_done(
        [](bool paused) { set_pause_ui(paused); });

    ui::CameraMsgProxy::instance().register_snapshot_rsp(
        [](const middleware::CameraSnapshotRspPayload &rsp) {
            g_monitor_photo_pending = false;
            if(g_monitor_photo_btn) {
                lv_obj_clear_state(g_monitor_photo_btn, LV_STATE_DISABLED);
            }
            if(g_result_photo_btn) {
                lv_obj_clear_state(g_result_photo_btn, LV_STATE_DISABLED);
            }
            if(rsp.ok) {
                ui_toast_show("Photo saved to /home/photo", user_color_accent, 1500);
            } else {
                ui_toast_show("Photo save failed", ui_color_danger, 1500);
            }
        });

    ui::UiMsgProxy::instance().register_clean_test_start_rsp(
        [](bool accepted) {
            if(accepted) {
                return;
            }
            lv_async_call(on_clean_start_reject_async, nullptr);
        });

    ui::UiMsgProxy::instance().register_clean_test_done(
        [](const middleware::CleanTestDonePayload &rsp) {
            CleanDoneToastCtx *ctx = new CleanDoneToastCtx{};
            ctx->success = (rsp.result_code == 0);
            lv_async_call(on_clean_done_async, ctx);
        });

    ui::UiMsgProxy::instance().register_clean_test_close_rsp([]() {
        lv_async_call(on_clean_close_rsp_async, nullptr);
    });
}

void home_set_status(user_home_status_t s)
{
    g_snap.status = s;
    /* 打印中/打印结果页的状态由前端说了算，锁住不让环境推送覆盖 */
    g_status_locked = (s == USER_HOME_STATUS_PRINTING ||
                       s == USER_HOME_STATUS_PRINT_DONE ||
                       s == USER_HOME_STATUS_PRINT_TERMINATED);
}

void exit_monitor_mode(void);

void home_trigger_door_open(void)
{
     /* 从监控页滑动开门时先退出监控态，避免回首页后二次进入预览被早退拦截。 */
     exit_monitor_mode();
     /* 走 control 的门弹窗流程（Opening→DoorOpened→DoorClosed，含 PM2.5 净化检查/
     * 最短停留/关门轮询），确认关门后回首页。不再直接 send_open_door()。 */
     control_open_door_request();
}

/* ---------- 顶部状态条 ---------- */
lv_obj_t *g_topbar       = nullptr;   /**< 顶部状态条根（打印中浮于摄像头之上） */
lv_obj_t *g_lbl_wifi     = nullptr;   /**< 顶栏 WiFi 图标（按连接状态变色） */
lv_obj_t *g_lbl_dev_name = nullptr;
lv_obj_t *g_dot_status   = nullptr;
lv_obj_t *g_lbl_status   = nullptr;
lv_obj_t *g_btn_files    = nullptr;   /**< 右上角 "My Files" 磨砂按钮 */

/* ---------- 首页左卡（HOME 空气质量·可切换 PM2.5/PM10） ---------- */
lv_obj_t *g_home_card_title = nullptr;  /**< "HOME PM2.5" / "HOME PM10" */
lv_obj_t *g_home_card_level = nullptr;  /**< 分级大字 Good/Fair/Poor/Very Poor */
lv_obj_t *g_home_card_value = nullptr;  /**< 灰色小字 "(36.5 ug/m3)" */
lv_obj_t *g_home_seg_track  = nullptr;  /**< 4 段色条容器（用于计算游标 x） */
lv_obj_t *g_home_seg_dot    = nullptr;  /**< 分级游标圆点 */
int       g_home_metric_idx = 0;        /**< 0=PM2.5 1=PM10（切换按钮，重启复位） */

/* ---------- 首页右侧 CABIN CONDITION（5 行数值） ---------- */
lv_obj_t *g_cab_aqi_val   = nullptr;    /**< AQI（占位 --） */
lv_obj_t *g_cab_temp_val  = nullptr;
lv_obj_t *g_cab_temp_unit = nullptr;
lv_obj_t *g_cab_o2_val    = nullptr;
lv_obj_t *g_cab_o2_unit   = nullptr;
lv_obj_t *g_cab_hum_val   = nullptr;
lv_obj_t *g_cab_hum_unit  = nullptr;
lv_obj_t *g_cab_prs_val   = nullptr;
lv_obj_t *g_cab_prs_unit  = nullptr;

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
lv_obj_t *g_prog_bar          = nullptr;
lv_obj_t *g_prog_value        = nullptr; /**< "37%" */
lv_obj_t *g_prog_remaining    = nullptr; /**< "剩余 45 min" */
lv_obj_t *g_prog_file_name    = nullptr;
lv_obj_t *g_print_bottom      = nullptr; /**< 打印中底部双条容器（结果页要整块隐藏） */
lv_obj_t *g_btn_pause         = nullptr; /**< 暂停/恢复按钮（终止按钮左侧） */
lv_obj_t *g_pause_icon        = nullptr; /**< 暂停按钮图标（PAUSE ↔ PLAY） */
bool      g_print_paused      = false;   /**< 当前是否暂停（乐观切换，后端回执校正） */
bool      g_printing_active   = false;

/* ---------- 打印结果浮层：与打印中底条互斥，挂在 g_print_area 内 ----------
 * 摄像头背景与 CameraPreviewPlayer 沿用打印中那一套，避免停流重开导致黑屏闪烁。 */
lv_obj_t *g_result_layer       = nullptr; /**< 结果页所有元素的根（默认隐藏） */
lv_obj_t *g_result_badge_icon  = nullptr; /**< stats card 头部图标（✓ / ！） */
lv_obj_t *g_result_badge_text  = nullptr; /**< "PRINT COMPLETE" / "PRINT TERMINATED" */
lv_obj_t *g_result_time_val    = nullptr; /**< TIME COST 数值 "2:23:23" */
lv_obj_t *g_result_material_val= nullptr; /**< CONSUMABLES 数值 "592.32" */
lv_obj_t *g_result_material_unit=nullptr; /**< CONSUMABLES 单位 "G"（无数据时隐藏） */
lv_obj_t *g_result_photo_btn   = nullptr; /**< 结果页拍照钮（与监控页共用在途标志） */
lv_obj_t *g_result_purify_btn  = nullptr; /**< 结果页 Purify 按钮 */
ui_slide_unlock_t *g_result_slide = nullptr; /**< "SWIPE TO COLLECT" */
char      g_result_job_name[96] = {0}; /**< 清粉录像目录键（优先用 manifest.frames_dir） */
bool      g_result_cleaning = false;
bool      g_result_purify_done = false; /**< 结果页已成功清粉，禁止再次 Purify */
/* 顶部左侧 HOME PM2.5 卡（稿 4045:18205）：点卡片本身切换 PM2.5/PM10，
 * 与首页左卡共用 g_home_metric_idx，两处显示始终同步。 */
lv_obj_t *g_res_pm_title = nullptr;       /**< "HOME PM2.5" / "HOME PM10" */
lv_obj_t *g_res_pm_level = nullptr;       /**< 分级词 Good/Fair/Poor/Very Poor */
lv_obj_t *g_res_pm_value = nullptr;       /**< 灰色小字 "(25.0 ug/m3)" */
/* 顶部环境条 5 列（稿 4045:18156）：舱内 PM2.5 分级 + TEMP/O2/HUM/PRESSURE。
 * 稿中首列是文字 caption（无图标），后四列是 32×32 图标 + 数值。 */
lv_obj_t *g_res_env_cabin_val = nullptr;  /**< 舱内 PM2.5 分级词 */
lv_obj_t *g_res_env_temp_val = nullptr;
lv_obj_t *g_res_env_o2_val   = nullptr;
lv_obj_t *g_res_env_hum_val  = nullptr;
lv_obj_t *g_res_env_prs_val  = nullptr;

/* ---------- 顶栏"查看监控"入口 + 待机监控（摄像头全屏）视图 ---------- */
lv_obj_t *g_btn_camera             = nullptr; /**< 顶栏圆形"查看监控"按钮 */
lv_obj_t *g_monitor_area           = nullptr; /**< 监控浮层（默认隐藏，与 normal/print 互斥） */
lv_obj_t *g_monitor_camera_viewport= nullptr;
lv_obj_t *g_monitor_camera_hint    = nullptr;
lv_obj_t *g_monitor_camera_image   = nullptr; /**< 监控视频帧槽（复用同一 CameraPreviewPlayer） */
lv_obj_t *g_monitor_photo_btn      = nullptr; /**< 监控区拍照按钮 */
lv_obj_t *g_mon_env_pm_ext         = nullptr; /**< 右上环境面板：EXTERIOR PM2.5 数值 */
lv_obj_t *g_mon_env_pm_int         = nullptr; /**< INTERIOR PM2.5 数值 */
lv_obj_t *g_mon_env_o2             = nullptr; /**< O2 CONC. 数值 */
lv_obj_t *g_mon_env_temp           = nullptr; /**< DEVICE TEMP 数值 */
ui_slide_unlock_t *g_monitor_door  = nullptr; /**< 监控界面的滑动开门 */
bool      g_monitor_active         = false;
bool      g_monitor_photo_pending  = false;   /**< 拍照请求在途，防连按 */

void apply_camera_preview_layout(uint32_t src_w, uint32_t src_h = 0);
void on_camera_viewport_size_changed(lv_event_t *e);
void enter_monitor_mode(void);
void exit_monitor_mode(void);

/** 预览布局（通用）：宽贴满 viewport；高度按等比缩放后做上下居中裁剪（解码层仍输出全帧）。
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
    /* 固定按宽度适配：宽度刚好贴满 viewport，高度超出部分做上下居中裁剪。 */
    if(src_h == 0 && ui::CameraPreviewPlayer::instance().active()) {
        src_h = ui::CameraPreviewPlayer::instance().stream_height();
    }
    const int64_t zoom_w = static_cast<int64_t>(vp_w) * 256 / static_cast<int32_t>(src_w);
    (void)src_h;
    const uint32_t zoom = static_cast<uint32_t>(zoom_w);
    lv_image_set_scale(img, zoom > 0 ? zoom : LV_SCALE_NONE);
    lv_obj_invalidate(img);
}

void apply_camera_preview_layout(uint32_t src_w, uint32_t src_h)
{
    cam_apply_layout(g_camera_viewport, g_camera_image, src_w, src_h);
}

void on_camera_viewport_size_changed(lv_event_t * /*e*/)
{
    uint32_t src_w = UPC_CAMERA_STREAM_WIDTH;
    if(ui::CameraPreviewPlayer::instance().active()) {
        src_w = ui::CameraPreviewPlayer::instance().stream_width();
        if(src_w == 0) {
            src_w = UPC_CAMERA_STREAM_WIDTH;
        }
    }
    apply_camera_preview_layout(src_w);
}

void on_monitor_viewport_size_changed(lv_event_t * /*e*/)
{
    uint32_t src_w = UPC_CAMERA_STREAM_WIDTH;
    if(ui::CameraPreviewPlayer::instance().active()) {
        src_w = ui::CameraPreviewPlayer::instance().stream_width();
        if(src_w == 0) {
            src_w = UPC_CAMERA_STREAM_WIDTH;
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
    /* 开门流程改由门弹窗承载，不再弹「Open Door」吐司（避免与弹窗重复） */
    home_trigger_door_open();
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
 *  保证在 flex 布局里只占 target×target（避免大尺寸源图撑开按钮）。
 *  源图边长从图头取，96×96（home_btn_icons）与 40×40（home_metric_icons）通吃。 */
lv_obj_t *make_btn_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t target)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, target, target);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t src_sz = (int32_t)src->header.w;
    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_pivot(img, src_sz / 2, src_sz / 2);
    lv_image_set_scale(img, (uint32_t)(256 * target / src_sz));
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
    /* 纯布局容器，去掉 lv_obj_create 默认带的 CLICKABLE：打印态下 print_area 被
     * move_background() 压到状态栏之下（见 page_user_home_enter_printing_mode），
     * 这条 100%×TOPBAR_H 的带子若可点，会把结果页贴顶那颗圆钮和 HOME PM2.5 卡的
     * 触摸全部吃掉。栏内两个真按钮是 lv_button_create，各有独立命中区。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl_wifi = lv_label_create(bar);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi, user_color_text, 0);
    lv_obj_set_style_text_font(lbl_wifi, user_font_text(), 0);
    g_lbl_wifi = lbl_wifi;

    g_lbl_dev_name = lv_label_create(bar);
    lv_label_set_text(g_lbl_dev_name, upc::config::kDeviceDisplayName);
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

    /* 顺序对齐设计稿：开始打印在左（976），摄像头圆钮在最右（1184）。 */
    g_btn_files = build_files_button(bar);
    g_btn_camera = build_camera_button(bar);

    return bar;
}

/* ============================================================================
 *  首页 normal 视图（对齐 Figma 1.1.1 待机中）
 *    左：HOME 空气质量磨砂卡（PM2.5/PM10 可切换 + 四档色条 + 游标）
 *    右：CABIN CONDITION 悬浮 5 行（AQI/TEMP/O2/HUMIDITY/PRESSURE）
 * ========================================================================== */

/** 空气质量指标描述：标题 + 三档上界（Good/Fair/Poor 上限）+ Very Poor 插值伪上限。 */
struct AirMetric {
    const char *title;
    float       b_good;     /**< ≤ b_good → Good */
    float       b_fair;     /**< ≤ b_fair → Fair */
    float       b_poor;     /**< ≤ b_poor → Poor；否则 Very Poor */
    float       pseudo_max; /**< Very Poor 段游标插值用的伪上限 */
};

/* 阈值取自设计稿分级表：PM2.5 35/75/150，PM10 50/150/350（μg/m³）。 */
const AirMetric kAirMetrics[2] = {
    { "HOME PM2.5", 35.0f,  75.0f, 150.0f, 300.0f },
    { "HOME PM10",  50.0f, 150.0f, 350.0f, 700.0f },
};

const char *const kLevelName[4] = { "Good", "Fair", "Poor", "Very Poor" };

/** 四档色段颜色（绿/黄/橙/红，对齐 Figma AQI-优/良/轻度/中度）。 */
lv_color_t air_level_color(int level)
{
    switch(level) {
    case 0:  return lv_color_hex(0x4cd276);
    case 1:  return lv_color_hex(0xf2d33a);
    case 2:  return lv_color_hex(0xf99635);
    default: return lv_color_hex(0xec4848);
    }
}

/** 由数值定档并给出色条全局比例（0..1）：档内按数值在区间内线性插值。 */
int air_classify(const AirMetric &m, float v, float *frac_global)
{
    int   level;
    float lo, hi;
    if(v <= m.b_good)      { level = 0; lo = 0;        hi = m.b_good; }
    else if(v <= m.b_fair) { level = 1; lo = m.b_good; hi = m.b_fair; }
    else if(v <= m.b_poor) { level = 2; lo = m.b_fair; hi = m.b_poor; }
    else                   { level = 3; lo = m.b_poor; hi = m.pseudo_max; }
    float fin = (hi > lo) ? (v - lo) / (hi - lo) : 0.5f;
    if(fin < 0.0f) fin = 0.0f;
    if(fin > 1.0f) fin = 1.0f;
    if(frac_global) *frac_global = (static_cast<float>(level) + fin) / 4.0f;
    return level;
}

/** dim 着色的指标图标（源 40×40 A8，等比缩放到 sz 方盒、居中）。 */
lv_obj_t *make_dim_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t sz)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, sz, sz);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, user_color_text_dim, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_image_set_pivot(img, 20, 20);                 /* 40×40 源图中心 */
    lv_image_set_scale(img, (uint32_t)(256 * sz / 40));
    lv_obj_center(img);
    return box;
}

void home_card_update(void);   /* 定义见下方数据刷新区 */

/** 左卡右上角切换按钮：PM2.5 ⇄ PM10 循环（重启复位到 PM2.5）。 */
void on_home_metric_toggle(lv_event_t * /*e*/)
{
    g_home_metric_idx = (g_home_metric_idx + 1) % 2;
    home_card_update();
}

/* 左卡：HOME 空气质量磨砂卡（对齐 Figma 3540:14581 / 3593:11006）。 */
lv_obj_t *build_home_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(card, LV_PCT(30));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_set_style_pad_row(card, USER_PAD_LG, 0);
    /* LVGL 无背景模糊：半透明深底 + 细白描边近似磨砂玻璃。 */
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_radius(card, USER_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_LEFT_MID, USER_PAD_LG, 0);

    /* 头部：[home 图标] 标题 ...(撑开)... [切换按钮] */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    make_dim_icon(head, &ic_home, 32);

    g_home_card_title = lv_label_create(head);
    lv_label_set_text(g_home_card_title, kAirMetrics[0].title);
    lv_obj_set_style_text_color(g_home_card_title, user_color_text, 0);
    lv_obj_set_style_text_opa(g_home_card_title, LV_OPA_50, 0);
    lv_obj_set_style_text_font(g_home_card_title, user_font_text(), 0);

    lv_obj_t *hspacer = lv_obj_create(head);
    lv_obj_remove_style_all(hspacer);
    lv_obj_set_height(hspacer, 1);
    lv_obj_set_flex_grow(hspacer, 1);
    lv_obj_set_style_bg_opa(hspacer, LV_OPA_TRANSP, 0);

    lv_obj_t *toggle = lv_button_create(head);
    lv_obj_remove_style_all(toggle);
    lv_obj_set_size(toggle, 40, 40);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(toggle, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(toggle, on_home_metric_toggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *tg_icon = make_dim_icon(toggle, &ic_switch, 32);
    lv_obj_center(tg_icon);

    /* 分隔线 */
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, user_color_text, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_20, 0);

    /* 分级大字 + 灰色小字（底对齐） */
    lv_obj_t *lrow = lv_obj_create(card);
    lv_obj_remove_style_all(lrow);
    lv_obj_set_size(lrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(lrow, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(lrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lrow, LV_OBJ_FLAG_SCROLLABLE);

    g_home_card_level = lv_label_create(lrow);
    lv_label_set_text(g_home_card_level, "--");
    lv_obj_set_style_text_color(g_home_card_level, user_color_text, 0);
    lv_obj_set_style_text_font(g_home_card_level, user_font_value(), 0);

    g_home_card_value = lv_label_create(lrow);
    lv_label_set_text(g_home_card_value, "");
    lv_obj_set_style_text_color(g_home_card_value, user_color_text, 0);
    lv_obj_set_style_text_opa(g_home_card_value, LV_OPA_50, 0);
    lv_obj_set_style_text_font(g_home_card_value, user_font_caption(), 0);
    lv_obj_set_style_margin_bottom(g_home_card_value, USER_PAD_SM, 0);

    /* 四段色条 + 游标圆点（游标在 wrap 内按比例定位；track 宽度 100%） */
    lv_obj_t *seg_wrap = lv_obj_create(card);
    lv_obj_remove_style_all(seg_wrap);
    lv_obj_set_width(seg_wrap, LV_PCT(100));
    lv_obj_set_height(seg_wrap, 16);
    lv_obj_set_style_bg_opa(seg_wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(seg_wrap, LV_OBJ_FLAG_SCROLLABLE);

    g_home_seg_track = lv_obj_create(seg_wrap);
    lv_obj_remove_style_all(g_home_seg_track);
    lv_obj_set_width(g_home_seg_track, LV_PCT(100));
    lv_obj_set_height(g_home_seg_track, 8);
    lv_obj_align(g_home_seg_track, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_flex_flow(g_home_seg_track, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_home_seg_track, 4, 0);
    lv_obj_set_style_bg_opa(g_home_seg_track, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_home_seg_track, LV_OBJ_FLAG_SCROLLABLE);
    for(int i = 0; i < 4; ++i) {
        lv_obj_t *seg = lv_obj_create(g_home_seg_track);
        lv_obj_remove_style_all(seg);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_height(seg, 8);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_set_style_bg_color(seg, air_level_color(i), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    }

    g_home_seg_dot = lv_obj_create(seg_wrap);
    lv_obj_remove_style_all(g_home_seg_dot);
    lv_obj_set_size(g_home_seg_dot, 14, 14);
    lv_obj_set_style_radius(g_home_seg_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_home_seg_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_home_seg_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_home_seg_dot, 2, 0);
    lv_obj_set_style_border_color(g_home_seg_dot, lv_color_black(), 0);
    lv_obj_set_style_border_opa(g_home_seg_dot, LV_OPA_30, 0);
    lv_obj_align(g_home_seg_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(g_home_seg_dot, LV_OBJ_FLAG_SCROLLABLE);

    return card;
}

/* CABIN 单行：[图标] 标签 ...(撑开)... 数值 + 单位。unit_text 为空 → 无单位（AQI）。 */
lv_obj_t *make_cabin_row(lv_obj_t *parent, const lv_image_dsc_t *icon,
                         const char *label, const char *unit_text,
                         lv_obj_t **out_val, lv_obj_t **out_unit)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    make_dim_icon(row, icon, 32);

    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_color(cap, user_color_text, 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_50, 0);
    lv_obj_set_style_text_font(cap, user_font_text(), 0);

    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    /* 数值 + 单位（底对齐） */
    lv_obj_t *vrow = lv_obj_create(row);
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

    if(unit_text) {
        lv_obj_t *u = lv_label_create(vrow);
        lv_label_set_text(u, unit_text);
        lv_obj_set_style_text_color(u, user_color_text, 0);
        lv_obj_set_style_text_opa(u, LV_OPA_50, 0);
        lv_obj_set_style_text_font(u, user_font_caption(), 0);
        lv_obj_set_style_margin_bottom(u, USER_PAD_SM, 0);
        if(out_unit) *out_unit = u;
    } else if(out_unit) {
        *out_unit = nullptr;
    }
    return row;
}

/* 右侧 CABIN CONDITION：头部 + 分隔线 + 5 行（悬浮文本，无卡片底）。 */
lv_obj_t *build_cabin_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    /* 面板宽 28%（左缘 ~70%）；配合 gen_home_bg.py 把设备左移，使设备右缘退到
     * ~63% 给面板腾出右侧暗区，两者不重叠。 */
    lv_obj_set_width(panel, LV_PCT(28));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, USER_PAD, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(panel, LV_ALIGN_RIGHT_MID, -USER_PAD_LG, 0);

    /* 头部：[cabin 图标] CABIN CONDITION */
    lv_obj_t *head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    make_dim_icon(head, &ic_cabin, 32);
    lv_obj_t *htitle = lv_label_create(head);
    lv_label_set_text(htitle, "CABIN CONDITION");
    lv_obj_set_style_text_color(htitle, user_color_text, 0);
    lv_obj_set_style_text_opa(htitle, LV_OPA_50, 0);
    lv_obj_set_style_text_font(htitle, user_font_text(), 0);
    lv_obj_set_style_text_letter_space(htitle, 1, 0);

    /* 分隔线 */
    lv_obj_t *div = lv_obj_create(panel);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, user_color_text, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_20, 0);

    make_cabin_row(panel, &ic_aqi,      "AQI",      nullptr,       &g_cab_aqi_val,  nullptr);
    make_cabin_row(panel, &ic_temp,     "TEMP",     "\xC2\xB0""C", &g_cab_temp_val, &g_cab_temp_unit);
    make_cabin_row(panel, &ic_o2,       "O2 CONC.", "%",           &g_cab_o2_val,   &g_cab_o2_unit);
    make_cabin_row(panel, &ic_humidity, "HUMIDITY", "%",           &g_cab_hum_val,  &g_cab_hum_unit);
    make_cabin_row(panel, &ic_pressure, "PRESSURE", "kPa",         &g_cab_prs_val,  &g_cab_prs_unit);
    return panel;
}

/** 中部主区：全屏整机背景（RGB565）+ 左卡（空气质量）+ 右侧 CABIN 面板。
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

    build_home_card(area);
    build_cabin_panel(area);

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

/* ============================================================================
 *  打印结果浮层（PRINT COMPLETE / PRINT TERMINATED）
 *  ----------------------------------------------------------------------------
 *  对齐 Figma「打印完成 / 打印终止」，两稿只差状态色与头部文案，故实现为
 *  一套控件 + 一个 terminated 变量。挂在 g_print_area 内，与打印中底条
 *  （g_print_bottom）互斥显示，摄像头背景与预览播放器完全沿用打印中那套。
 *
 *  布局（内容区，避开左侧导航栏与顶部状态栏）：
 *    右上   环境条 6 列（同首页六项）      右侧   三颗圆钮（回首页/Purify/拍照）
 *    左下   结果 stats card                右下   SWIPE TO COLLECT
 *
 *  设计稿按 1280×800 出，这里不写死坐标，全部走主题宏 + 边缘对齐，
 *  1024×600 上按同一比例收缩。
 * ========================================================================== */

/* 结果页圆钮直径：稿上 64（1280×800），×8/7 折算成 1024 档的 50、1280 档的 59，
 * 仍在 40px 触摸下限之上。图标取直径一半，与稿上 64:32 的比例一致。 */
constexpr int32_t kResultBtnRatioNum = 8;
constexpr int32_t kResultBtnRatioDen = 7;

/** 圆形玻璃钮 + 下方标签（稿 3896:14303/14310）。label 为空则只出钮（右上角那颗）。
 *  返回「钮 + 标签」的整组，供调用方整体对齐；out_btn 回传钮本体用于置灰。 */
lv_obj_t *make_result_round_btn(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                const char *label, lv_event_cb_t cb,
                                lv_obj_t **out_btn)
{
    const int32_t sz = USER_BTN_PRIMARY_H * kResultBtnRatioNum / kResultBtnRatioDen;

    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(group, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_button_create(group);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x757575), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = make_btn_icon(btn, icon, sz / 2);
    lv_obj_center(ic);
    if(out_btn) *out_btn = btn;

    if(label) {
        lv_obj_t *cap = lv_label_create(group);
        lv_label_set_text(cap, label);
        lv_obj_set_style_text_color(cap, lv_color_white(), 0);
        lv_obj_set_style_text_opa(cap, LV_OPA_50, 0);
        lv_obj_set_style_text_font(cap, user_font_text(), 0);
    }
    return group;
}

/* 右上角按钮：直接退出打印状态回待机首页（不开门，不保留返回结果页的入口）。 */
void on_result_home_click(lv_event_t * /*e*/)
{
    control_print_result_dismiss();
}

/* Purify：触发打印结果页清粉测试（后端走 clean_test_start/close + TLV 0x22）。
 * 一次打印结果仅允许成功清粉一次。 */
void on_result_purify_click(lv_event_t * /*e*/)
{
    if(g_result_cleaning || g_result_purify_done) {
        return;
    }
    g_result_cleaning = true;
    if(g_result_purify_btn) {
        lv_obj_add_state(g_result_purify_btn, LV_STATE_DISABLED);
    }
    ui::UiMsgProxy::instance().send_clean_test_start(g_result_job_name, 0, 0, 0);
    ui_toast_show("Powder cleaning started", user_color_accent, 1500);
}

void on_result_photo_click(lv_event_t * /*e*/)
{
    if(g_monitor_photo_pending) {
        return;
    }
    g_monitor_photo_pending = true;
    if(g_result_photo_btn) {
        lv_obj_add_state(g_result_photo_btn, LV_STATE_DISABLED);
    }
    ui::CameraMsgProxy::instance().send_snapshot_req();
    ui_toast_show("Capturing photo...", user_color_accent, 1200);
}

/* 滑动取件：退出打印状态并走开门流程（与首页「SWIPE TO OPEN」同一套弹窗）。 */
void on_result_collect_unlocked(void * /*u*/)
{
    control_print_result_collect();
}

/* 环境条图标边长：稿上 32（1280×800）→ 1024 档 25、1280 档 29。
 * 主题宏展开成 g_ui->xxx，运行期才可用，所以只能是内联函数不能是 constexpr。 */
inline int32_t result_env_icon_sz(void) { return USER_BTN_PRIMARY_H * 4 / 7; }

/** 环境条数值行：数值 + 可选单位（底对齐，稿 EL-23615d6d）。 */
lv_obj_t *result_env_value_row(lv_obj_t *col, const char *unit, lv_obj_t **out_val)
{
    lv_obj_t *vrow = lv_obj_create(col);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vrow, USER_PAD_SM / 4, 0);
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
        lv_obj_set_style_text_color(u, user_color_text, 0);
        lv_obj_set_style_text_opa(u, LV_OPA_50, 0);
        lv_obj_set_style_text_font(u, user_font_caption(), 0);
    }
    return vrow;
}

/** 环境条列骨架：上下两行居中（上为图标或文字 caption，下为数值行）。 */
lv_obj_t *result_env_col_base(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 4, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

/** 图标列（稿中第 2~5 列）：一行图标 + 一行数值。 */
lv_obj_t *result_env_icon_col(lv_obj_t *parent, const lv_image_dsc_t *icon,
                              const char *unit, lv_obj_t **out_val)
{
    lv_obj_t *col = result_env_col_base(parent);
    make_dim_icon(col, icon, result_env_icon_sz());
    result_env_value_row(col, unit, out_val);
    return col;
}

/** 文字列（稿中第 1 列，舱内 PM2.5）：稿上这列用的是文字 caption，不是图标。 */
lv_obj_t *result_env_text_col(lv_obj_t *parent, const char *caption,
                              lv_obj_t **out_val)
{
    lv_obj_t *col = result_env_col_base(parent);
    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text, 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_50, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);
    result_env_value_row(col, nullptr, out_val);
    return col;
}

void result_pm_card_update(void);   /* 定义见下方数据刷新区 */

/* 结果页 HOME PM2.5 卡：点卡片本身切换 PM2.5/PM10（稿上这张卡没有切换圆钮）。
 * 与首页左卡共用 g_home_metric_idx，两处一起刷新以免回到首页时不一致。 */
void on_result_pm_card_click(lv_event_t * /*e*/)
{
    g_home_metric_idx = (g_home_metric_idx + 1) % 2;
    home_card_update();
    result_pm_card_update();
}

/** 顶部左侧 HOME PM2.5 卡（稿 4045:18205）：标题 + 分级词 + 括号数值。
 *  稿上宽 157，但最坏文案 "Very Poor (150.0 ug/m3)" 放不下，故用内容自适应宽。 */
lv_obj_t *build_result_pm_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(card, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 4, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_result_pm_card_click, LV_EVENT_CLICKED, nullptr);

    g_res_pm_title = lv_label_create(card);
    lv_label_set_text(g_res_pm_title, kAirMetrics[0].title);
    lv_obj_set_style_text_color(g_res_pm_title, user_color_text, 0);
    lv_obj_set_style_text_opa(g_res_pm_title, LV_OPA_50, 0);
    lv_obj_set_style_text_font(g_res_pm_title, user_font_caption(), 0);

    lv_obj_t *lrow = lv_obj_create(card);
    lv_obj_remove_style_all(lrow);
    lv_obj_set_size(lrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(lrow, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(lrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lrow, LV_OBJ_FLAG_CLICKABLE);

    g_res_pm_level = lv_label_create(lrow);
    lv_label_set_text(g_res_pm_level, "--");
    lv_obj_set_style_text_color(g_res_pm_level, user_color_text, 0);
    lv_obj_set_style_text_font(g_res_pm_level, user_font_text(), 0);

    g_res_pm_value = lv_label_create(lrow);
    lv_label_set_text(g_res_pm_value, "");
    lv_obj_set_style_text_color(g_res_pm_value, user_color_text, 0);
    lv_obj_set_style_text_opa(g_res_pm_value, LV_OPA_50, 0);
    lv_obj_set_style_text_font(g_res_pm_value, user_font_caption(), 0);
    return card;
}

/** 顶部环境条（稿 4045:18156）：舱内 PM2.5 + TEMP/O2/HUMIDITY/PRESSURE 五列。
 *  稿上没有 AQI，第一列是舱内 PM2.5 的分级词。 */
lv_obj_t *build_result_env_bar(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(panel, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_30, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    result_env_text_col(panel, "PM2.5", &g_res_env_cabin_val);
    result_env_icon_col(panel, &ic_temp,     "\xC2\xB0""C", &g_res_env_temp_val);
    result_env_icon_col(panel, &ic_o2,       "%",           &g_res_env_o2_val);
    result_env_icon_col(panel, &ic_humidity, "%",           &g_res_env_hum_val);
    result_env_icon_col(panel, &ic_pressure, "kpa",         &g_res_env_prs_val);
    return panel;
}

/** stats card 内一列（稿 3589:10035）：caption 在上、数值在下，两列并排各占一半。
 *  数值用 user_font_value()——稿上 32px 正是 1280 档的 value 字号，1024 档 24 同样
 *  等于稿值按 600/800 折算的结果，两档都不用另外补偿。 */
lv_obj_t *result_stat_col(lv_obj_t *parent, const char *caption,
                          const char *unit, lv_obj_t **out_val,
                          lv_obj_t **out_unit)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    /* 两列等分剩余宽度：用 grow 而不是 PCT(50)，否则 50%+50%+列间距会溢出 */
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 2, 0);
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
    lv_obj_set_style_pad_column(vrow, USER_PAD_SM / 4, 0);
    lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(vrow);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_value(), 0);
    if(out_val) *out_val = val;

    if(unit) {
        lv_obj_t *u = lv_label_create(vrow);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_color(u, user_color_text, 0);
        lv_obj_set_style_text_opa(u, LV_OPA_50, 0);
        lv_obj_set_style_text_font(u, user_font_caption(), 0);
        lv_obj_set_style_text_letter_space(u, 1, 0);
        /* 单位与数值基线对齐：value 字号比 caption 大，靠下边距把它抬到基线上 */
        lv_obj_set_style_margin_bottom(u, USER_PAD_SM / 4, 0);
        if(out_unit) *out_unit = u;
    }
    return col;
}

/** 左下结果卡（稿 3588:8999 / 3589:10028）：头部状态徽标 + 分隔线 +
 *  TIME COST / CONSUMABLES 两列并排。稿中无作业名字段，caption 行也没有图标。 */
lv_obj_t *build_result_stats_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    /* 稿宽 416（1280×800）；×8/7 折成 1024 档 329、1280 档 411，与右下滑动条成一对 */
    lv_obj_set_width(card, USER_SLIDE_W * 8 / 7);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* 稿上 padding/gap 都是 16 → PAD_SM（14/11），不是 USER_PAD（28/22） */
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(card, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, USER_RADIUS / 2, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 头部：图标 + 状态词（颜色/文案在 page_user_home_enter_print_result 里定） */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    g_result_badge_icon = lv_label_create(head);
    lv_label_set_text(g_result_badge_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(g_result_badge_icon, user_font_caption(), 0);

    g_result_badge_text = lv_label_create(head);
    lv_label_set_text(g_result_badge_text, "PRINT COMPLETE");
    lv_obj_set_style_text_font(g_result_badge_text, user_font_caption(), 0);
    lv_obj_set_style_text_letter_space(g_result_badge_text, 1, 0);

    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_10, 0);

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    result_stat_col(body, "TIME COST",   nullptr, &g_result_time_val,     nullptr);
    result_stat_col(body, "CONSUMABLES", "G",     &g_result_material_val,
                    &g_result_material_unit);
    return card;
}

/** 摄像头画面左右两侧的黑色渐变遮罩（稿 3588:8910/8911，各 400×800）。
 *  纯背景装饰，不吃触摸；调用点要放在摄像头 viewport 之后、各浮层之前。
 *  稿上导出的角度两侧一致（都是右深左浅），左侧那条按可读性改成外深内浅。 */
void build_camera_gradient_masks(lv_obj_t *parent)
{
    /* 保留左右渐变遮罩，但避免“硬黑条”观感：
     * - 收窄遮罩宽度
     * - 降低边缘不透明度，做柔和压暗 */
    static lv_grad_dsc_t s_mask_l, s_mask_r;
    static bool s_mask_ready = false;
    if(!s_mask_ready) {
        const lv_color_t cols[2] = { lv_color_black(), lv_color_black() };
        const uint8_t    frac[2] = { 0, 255 };
        const lv_opa_t   opa_l[2] = { LV_OPA_30, LV_OPA_TRANSP };
        const lv_opa_t   opa_r[2] = { LV_OPA_TRANSP, LV_OPA_30 };
        lv_grad_init_stops(&s_mask_l, cols, opa_l, frac, 2);
        lv_grad_init_stops(&s_mask_r, cols, opa_r, frac, 2);
        lv_grad_linear_init(&s_mask_l, LV_GRAD_LEFT, LV_GRAD_TOP,
                            LV_GRAD_RIGHT, LV_GRAD_TOP, LV_GRAD_EXTEND_PAD);
        lv_grad_linear_init(&s_mask_r, LV_GRAD_LEFT, LV_GRAD_TOP,
                            LV_GRAD_RIGHT, LV_GRAD_TOP, LV_GRAD_EXTEND_PAD);
        s_mask_ready = true;
    }

    struct {
        const lv_grad_dsc_t *grad;
        lv_align_t align;
        int32_t width_pct;
    } kMasks[2] = {
        { &s_mask_l, LV_ALIGN_LEFT_MID, 8 },
        { &s_mask_r, LV_ALIGN_RIGHT_MID, 10 },
    };
    for(const auto &m : kMasks) {
        lv_obj_t *mask = lv_obj_create(parent);
        lv_obj_remove_style_all(mask);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(mask, LV_PCT(m.width_pct), LV_PCT(100));
        lv_obj_align(mask, m.align, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
        lv_obj_set_style_bg_grad(mask, m.grad, 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    }
}

lv_obj_t *build_print_result_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t btn_sz = USER_BTN_PRIMARY_H * kResultBtnRatioNum / kResultBtnRatioDen;

    /* 右侧三钮不是一列等距：稿上第一颗贴顶（y=32），Purify 组紧随其后（y=120，
     * 与第一颗间距 24），Photo 组则是「钮+标签」整组垂直居中（y=354，组高 92，
     * 中心正好落在 800/2）。所以三者各自独立对齐，不放同一个 flex 列。 */
    lv_obj_t *home_grp = make_result_round_btn(layer, &ic_home, nullptr,
                                               on_result_home_click, nullptr);
    lv_obj_add_flag(home_grp, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(home_grp, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);

    lv_obj_t *purify_grp = make_result_round_btn(layer, &ic_purify, "Purify",
                                                 on_result_purify_click,
                                                 &g_result_purify_btn);
    lv_obj_add_flag(purify_grp, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(purify_grp, LV_ALIGN_TOP_RIGHT, -USER_PAD,
                 USER_PAD * 2 + btn_sz);

    lv_obj_t *photo_grp = make_result_round_btn(layer, &ic_camera, "Photo",
                                                on_result_photo_click,
                                                &g_result_photo_btn);
    lv_obj_add_flag(photo_grp, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(photo_grp, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);

    /* 右上环境区：HOME PM2.5 卡 + 5 列环境条两个独立容器，整体右对齐并让开圆钮列。 */
    lv_obj_t *env_wrap = lv_obj_create(layer);
    lv_obj_remove_style_all(env_wrap);
    lv_obj_add_flag(env_wrap, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(env_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(env_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(env_wrap, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(env_wrap, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(env_wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(env_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(env_wrap, LV_ALIGN_TOP_RIGHT,
                 -(USER_PAD + btn_sz + USER_PAD_SM), USER_PAD);

    build_result_pm_card(env_wrap);
    build_result_env_bar(env_wrap);

    /* 左下结果卡 */
    lv_obj_t *card = build_result_stats_card(layer);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(card, LV_ALIGN_BOTTOM_LEFT, USER_PAD, -USER_PAD);

    /* 右下滑动取件：红色 knob，同监控页尺寸 */
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text     = "SWIPE TO COLLECT";
    cfg.on_unlocked   = on_result_collect_unlocked;
    cfg.user_data     = nullptr;
    cfg.track_w       = USER_SLIDE_W * 4 / 3;
    cfg.track_h       = USER_BTN_DOOR_ICON_SZ * 4 / 3;
    cfg.custom_accent = true;
    cfg.accent_color  = user_color_status_err;
    g_result_slide = ui_slide_unlock_create(layer, &cfg);
    lv_obj_t *slide_root = ui_slide_unlock_get_root(g_result_slide);
    if(slide_root) {
        lv_obj_add_flag(slide_root, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(slide_root, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD, -USER_PAD);
    }

    return layer;
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
    apply_camera_preview_layout(UPC_CAMERA_STREAM_WIDTH);

    /* 左右渐变遮罩：压在画面之上、底条与结果页浮层之下，保证压字处可读 */
    build_camera_gradient_masks(area);

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
    g_print_bottom = bottom;

    /* 打印结果浮层：同一 area 内与 bottom 互斥显示（摄像头背景共用，预览不断流） */
    g_result_layer = build_print_result_layer(area);
    lv_obj_add_flag(g_result_layer, LV_OBJ_FLAG_HIDDEN);

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
    /* 开门流程改由门弹窗承载，不再弹「Open Door」吐司（避免与弹窗重复） */
    home_trigger_door_open();
    lv_timer_t *t = lv_timer_create(monitor_door_reset_cb, 700, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void on_monitor_photo_click(lv_event_t * /*e*/)
{
    if(g_monitor_photo_pending) {
        return;
    }
    g_monitor_photo_pending = true;
    if(g_monitor_photo_btn) {
        lv_obj_add_state(g_monitor_photo_btn, LV_STATE_DISABLED);
    }
    ui::CameraMsgProxy::instance().send_snapshot_req();
    ui_toast_show("Capturing photo...", user_color_accent, 1200);
}

lv_obj_t *build_monitor_photo_button(lv_obj_t *parent)
{
    const int32_t sz = USER_BTN_PRIMARY_H * 5 / 4;
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
    lv_obj_add_event_cb(btn, on_monitor_photo_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);
    lv_obj_center(icon);
    return btn;
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

    /* 左右渐变遮罩：压在画面之上、下方各控件之下 */
    build_camera_gradient_masks(area);

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

    g_monitor_photo_btn = build_monitor_photo_button(area);
    lv_obj_add_flag(g_monitor_photo_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_monitor_photo_btn, LV_ALIGN_BOTTOM_RIGHT,
                 -(USER_PAD * 2 + cfg.track_w), -USER_PAD);

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
    g_monitor_photo_pending = false;
    if(g_monitor_photo_btn) {
        lv_obj_clear_state(g_monitor_photo_btn, LV_STATE_DISABLED);
    }

    if(g_normal_area)  lv_obj_add_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)   lv_obj_add_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_files)    lv_obj_add_flag(g_btn_files,   LV_OBJ_FLAG_HIDDEN);
    if(g_btn_camera)   lv_obj_add_flag(g_btn_camera,  LV_OBJ_FLAG_HIDDEN);
    if(g_monitor_area) lv_obj_clear_flag(g_monitor_area, LV_OBJ_FLAG_HIDDEN);
    /* 监控层移到背景：顶部状态栏（保留设备状态）与左侧导航栏自然浮于其上。 */
    if(g_monitor_area) lv_obj_move_background(g_monitor_area);
    if(g_monitor_door) ui_slide_unlock_reset(g_monitor_door);

    page_user_home_refresh(true);
    cam_apply_layout(g_monitor_camera_viewport, g_monitor_camera_image,
                     UPC_CAMERA_STREAM_WIDTH, 0u);
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
    case USER_HOME_STATUS_PRINT_DONE:       return "Print Complete";
    case USER_HOME_STATUS_PRINT_TERMINATED: return "Print Terminated";
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
    case USER_HOME_STATUS_PRINT_DONE:       return user_color_status_ok;
    case USER_HOME_STATUS_PRINT_TERMINATED: return user_color_status_err;
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
    if(metric_invalid(pm)) {
        if(val_lbl) {
            lv_label_set_text(val_lbl, "--");
        }
        if(bar) {
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, user_color_divider, LV_PART_INDICATOR);
        }
        if(status_lbl) {
            lv_label_set_text(status_lbl, "--");
            lv_obj_set_style_text_color(status_lbl, user_color_text_dim, 0);
        }
        return;
    }

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

/** 刷新左卡（HOME 空气质量）：按当前切换指标填 标题/分级/数值/游标位置。 */
void home_card_update(void)
{
    if(!g_home_card_title) {
        return;
    }
    const AirMetric &m = kAirMetrics[g_home_metric_idx];
    lv_label_set_text(g_home_card_title, m.title);

    const float v = (g_home_metric_idx == 0) ? g_snap.pm25_indoor
                                             : g_snap.pm10_indoor;
    if(metric_invalid(v)) {
        /* 无数据：分级显示 "--"，清空数值、隐藏游标 */
        lv_label_set_text(g_home_card_level, "--");
        lv_obj_set_style_text_color(g_home_card_level, user_color_text_dim, 0);
        lv_label_set_text(g_home_card_value, "");
        if(g_home_seg_dot) {
            lv_obj_add_flag(g_home_seg_dot, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    float frac = 0.0f;
    const int level = air_classify(m, v, &frac);
    lv_label_set_text(g_home_card_level, kLevelName[level]);
    lv_obj_set_style_text_color(g_home_card_level, user_color_text, 0);
    lv_label_set_text_fmt(g_home_card_value, "(%.1f ug/m3)", v);

    /* 游标：档内插值得到全局比例 frac，落在色条对应位置。 */
    if(g_home_seg_dot && g_home_seg_track) {
        lv_obj_remove_flag(g_home_seg_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(g_home_seg_track);
        const int32_t tw = lv_obj_get_width(g_home_seg_track);
        const int32_t dot_sz = lv_obj_get_width(g_home_seg_dot);
        int32_t dx = static_cast<int32_t>(frac * static_cast<float>(tw)) - dot_sz / 2;
        if(dx < 0) dx = 0;
        if(dx > tw - dot_sz) dx = tw - dot_sz;
        lv_obj_align(g_home_seg_dot, LV_ALIGN_LEFT_MID, dx, 0);
    }
}

/** 刷新结果页 HOME PM2.5 卡：与首页左卡同源同档位，只是不画色条与游标。 */
void result_pm_card_update(void)
{
    if(!g_res_pm_title) {
        return;
    }
    const AirMetric &m = kAirMetrics[g_home_metric_idx];
    lv_label_set_text(g_res_pm_title, m.title);

    const float v = (g_home_metric_idx == 0) ? g_snap.pm25_indoor
                                             : g_snap.pm10_indoor;
    if(metric_invalid(v)) {
        lv_label_set_text(g_res_pm_level, "--");
        lv_obj_set_style_text_color(g_res_pm_level, user_color_text_dim, 0);
        lv_label_set_text(g_res_pm_value, "");
        return;
    }
    const int level = air_classify(m, v, nullptr);
    lv_label_set_text(g_res_pm_level, kLevelName[level]);
    lv_obj_set_style_text_color(g_res_pm_level, user_color_text, 0);
    lv_label_set_text_fmt(g_res_pm_value, "(%.1f ug/m3)", v);
}

/** 刷新 CABIN 单行数值：无效（<0/NaN）显示 "--" 并隐藏单位，否则 "%.1f" + 单位。 */
void update_cabin_metric(lv_obj_t *val, lv_obj_t *unit, float v)
{
    if(!val) {
        return;
    }
    if(metric_invalid(v)) {
        lv_label_set_text(val, "--");
        if(unit) lv_obj_add_flag(unit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(val, "%.1f", v);
        if(unit) lv_obj_remove_flag(unit, LV_OBJ_FLAG_HIDDEN);
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

    /* 首页左卡（HOME 空气质量·可切换 PM2.5/PM10：分级 + 四档色条 + 游标） */
    home_card_update();

    /* 首页右侧 CABIN CONDITION：AQI 占位 "--"（待接后端）；其余按哨兵判空 */
    if(g_cab_aqi_val) {
        lv_label_set_text(g_cab_aqi_val, "--");
    }
    update_cabin_metric(g_cab_temp_val, g_cab_temp_unit, snap.temperature_c);
    update_cabin_metric(g_cab_o2_val,   g_cab_o2_unit,   snap.oxygen_percent);
    update_cabin_metric(g_cab_hum_val,  g_cab_hum_unit,  snap.humidity_percent);
    update_cabin_metric(g_cab_prs_val,  g_cab_prs_unit,  snap.pressure_kpa);

    /* 打印模式下的环境标签也由同一快照驱动（串口 0x9B → APP_Update_Ntf） */
    update_pm_block(g_env_lbl_pm_indoor, g_env_bar_indoor, g_env_status_indoor,
                    snap.pm25_indoor);
    update_pm_block(g_env_lbl_pm_cabin, g_env_bar_cabin, g_env_status_cabin,
                    snap.pm25_cabin);
    if(g_env_lbl_temp) {
        if(metric_invalid(snap.temperature_c)) {
            lv_label_set_text(g_env_lbl_temp, "--");
        } else {
            lv_label_set_text_fmt(g_env_lbl_temp, "%.1f °C", snap.temperature_c);
        }
    }
    if(g_env_lbl_oxygen) {
        if(metric_invalid(snap.oxygen_percent)) {
            lv_label_set_text(g_env_lbl_oxygen, "--");
        } else {
            lv_label_set_text_fmt(g_env_lbl_oxygen, "%.1f %%", snap.oxygen_percent);
        }
    }

    /* 待机查看监控：右上环境小面板（单位为独立标签，数值只填数字） */
    if(g_mon_env_pm_ext) {
        if(metric_invalid(snap.pm25_indoor)) lv_label_set_text(g_mon_env_pm_ext, "--");
        else lv_label_set_text_fmt(g_mon_env_pm_ext, "%.1f", snap.pm25_indoor);
    }
    if(g_mon_env_pm_int) {
        if(metric_invalid(snap.pm25_cabin)) lv_label_set_text(g_mon_env_pm_int, "--");
        else lv_label_set_text_fmt(g_mon_env_pm_int, "%.1f", snap.pm25_cabin);
    }
    if(g_mon_env_o2) {
        if(metric_invalid(snap.oxygen_percent)) lv_label_set_text(g_mon_env_o2, "--");
        else lv_label_set_text_fmt(g_mon_env_o2, "%.1f", snap.oxygen_percent);
    }
    if(g_mon_env_temp) {
        if(metric_invalid(snap.temperature_c)) lv_label_set_text(g_mon_env_temp, "--");
        else lv_label_set_text_fmt(g_mon_env_temp, "%.1f", snap.temperature_c);
    }

    /* 打印结果页顶部：左侧 HOME PM2.5 卡 + 右侧 5 列环境条 */
    result_pm_card_update();
    /* 环境条首列是舱内 PM2.5，按档位取分级词并着色（稿上这列显示 "Good"） */
    if(g_res_env_cabin_val) {
        if(metric_invalid(snap.pm25_cabin)) {
            lv_label_set_text(g_res_env_cabin_val, "--");
            lv_obj_set_style_text_color(g_res_env_cabin_val, user_color_text_dim, 0);
        } else {
            const int level = air_classify(kAirMetrics[0], snap.pm25_cabin, nullptr);
            lv_label_set_text(g_res_env_cabin_val, kLevelName[level]);
            lv_obj_set_style_text_color(g_res_env_cabin_val, air_level_color(level), 0);
        }
    }
    update_cabin_metric(g_res_env_temp_val, nullptr, snap.temperature_c);
    update_cabin_metric(g_res_env_o2_val,   nullptr, snap.oxygen_percent);
    update_cabin_metric(g_res_env_hum_val,  nullptr, snap.humidity_percent);
    update_cabin_metric(g_res_env_prs_val,  nullptr, snap.pressure_kpa);
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
    page_user_home_update_camera_preview_layout(UPC_CAMERA_STREAM_WIDTH, 0u);

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

void page_user_home_enter_print_result(const control_print_result_t *result)
{
    if(!result || !g_printing_active || !g_result_layer) {
        return;
    }

    /* 摄像头与 g_print_area 保持不动，只把打印中底条换成结果页浮层 */
    if(g_print_bottom) lv_obj_add_flag(g_print_bottom, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_result_layer, LV_OBJ_FLAG_HIDDEN);

    g_monitor_photo_pending = false;
    g_result_cleaning = false;
    g_result_purify_done = false;
    if(result->video_dir[0]) {
        std::snprintf(g_result_job_name, sizeof(g_result_job_name), "%s",
                      result->video_dir);
    } else {
        std::snprintf(g_result_job_name, sizeof(g_result_job_name), "%s",
                      result->job_name);
    }
    if(g_result_photo_btn) {
        lv_obj_clear_state(g_result_photo_btn, LV_STATE_DISABLED);
    }
    if(g_result_purify_btn) {
        lv_obj_clear_state(g_result_purify_btn, LV_STATE_DISABLED);
    }
    if(g_result_slide) {
        ui_slide_unlock_reset(g_result_slide);
    }

    /* 完成/终止两稿的差异只有这里：状态色 + 头部图标与文案 */
    const lv_color_t accent = result->terminated ? user_color_status_err
                                                 : user_color_status_ok;
    if(g_result_badge_icon) {
        lv_label_set_text(g_result_badge_icon,
                          result->terminated ? LV_SYMBOL_WARNING : LV_SYMBOL_OK);
        lv_obj_set_style_text_color(g_result_badge_icon, accent, 0);
    }
    if(g_result_badge_text) {
        lv_label_set_text(g_result_badge_text,
                          result->terminated ? "PRINT TERMINATED"
                                             : "PRINT COMPLETE");
        lv_obj_set_style_text_color(g_result_badge_text, accent, 0);
    }

    /* manifest 无条目（0 帧）时耗时/耗材无数据，统一显示 "--" */
    if(g_result_time_val) {
        if(!result->has_stats) {
            lv_label_set_text(g_result_time_val, "--");
        } else {
            /* 稿上是 H:MM:SS（"2:23:23"），小时位不补零 */
            const uint32_t s = result->duration_sec;
            lv_label_set_text_fmt(g_result_time_val, "%u:%02u:%02u",
                                  s / 3600u, (s / 60u) % 60u, s % 60u);
        }
    }
    if(g_result_material_val) {
        /* material_g 由 config.json 反查，查不到时后端写 0 → 同样按无数据处理。
         * 无数据时连单位一起隐藏，避免出现 "-- G" 这种半截读法。 */
        const bool has_material = result->has_stats && result->material_g > 0.0f;
        if(has_material) {
            lv_label_set_text_fmt(g_result_material_val, "%.2f",
                                  static_cast<double>(result->material_g));
        } else {
            lv_label_set_text(g_result_material_val, "--");
        }
        if(g_result_material_unit) {
            if(has_material) lv_obj_remove_flag(g_result_material_unit, LV_OBJ_FLAG_HIDDEN);
            else             lv_obj_add_flag(g_result_material_unit, LV_OBJ_FLAG_HIDDEN);
        }
    }

    home_set_status(result->terminated ? USER_HOME_STATUS_PRINT_TERMINATED
                                       : USER_HOME_STATUS_PRINT_DONE);
    page_user_home_refresh(true);
}

void page_user_home_reset_result_slide(void)
{
    if(g_result_slide) {
        ui_slide_unlock_reset(g_result_slide);
    }
}

void page_user_home_exit_printing_mode(void)
{
    ui::CameraPreviewPlayer::instance().stop();
    g_printing_active = false;
    g_result_cleaning = false;
    g_result_purify_done = false;

    /* 结果页浮层与打印中底条一起复位，下次进打印中从底条起步 */
    if(g_result_layer)  lv_obj_add_flag(g_result_layer, LV_OBJ_FLAG_HIDDEN);
    if(g_print_bottom)  lv_obj_remove_flag(g_print_bottom, LV_OBJ_FLAG_HIDDEN);

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
