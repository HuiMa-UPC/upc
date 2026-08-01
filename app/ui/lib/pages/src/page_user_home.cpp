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
#include "ui_device_state.hpp"
#include "config/camera_resolution_profile.hpp"


#include <cmath>
#include <cstdio>

/**
 * @file page_user_home.cpp
 * @brief Snowpod 风格用户首页
 *
 * 视觉层次（与设计稿对齐）：
 *   ┌────────────────────────────────────────────────┐
 *   │ (设备信息由外壳的常驻条画)     [监控] [开始打印]│ ← topbar
 *   ├────────────┬────────────────┬────────────────┤
 *   │ 室内 PM2.5 │                │   设备温度     │
 *   │   25.3     │   [Device]     │    23.6°C     │
 *   │ 舱内 PM2.5 │                │   氧气浓度     │
 *   │   25.3     │                │    84.9%      │
 *   └────────────┴────────────────┴────────────────┘
 *                                  [滑动开门 >>>]   ← 浮动
 *
 * 环境快照与运行状态在 `core/ui_device_state`，本页作为观察者读取；顶栏左半边的
 * 设备名/状态由外壳的常驻条 `ui_device_bar` 画，本页那条只剩右侧两颗按钮。
 */

namespace {

/** 数值无效（后端未采集）：负值或 NaN → UI 显示 "--"。 */
static inline bool metric_invalid(float v) { return std::isnan(v) || v < 0.0f; }

/** 共享快照的观察者：`ui_device_state` 收到环境推送/状态置位后重绘本页。 */
void on_device_state(const ui_device_snapshot_t * /*snap*/)
{
    page_user_home_refresh(false);
}

/** 打印态徽标四档。吹粉中/吹粉完成不是独立徽标档——稿 4234:6385 里吹粉中的徽标
 *  仍是 PRINT TERMINATED，只有状态栏与胶囊文案变，故另立 purify_stage_t。 */
enum print_phase_t {
    PRINT_PHASE_PRINTING = 0,   /**< PRINTING...      蓝 */
    PRINT_PHASE_PAUSED,         /**< PAUSE            蓝 */
    PRINT_PHASE_TERMINATED,     /**< PRINT TERMINATED 橙 */
    PRINT_PHASE_DONE,           /**< PRINT COMPLETE   绿 */
};

/** 结束态下的吹粉子阶段，决定状态栏文字、提示条变体与胶囊文案。 */
enum purify_stage_t {
    PURIFY_STAGE_IDLE = 0,   /**< 还没吹粉：提示条催吹粉 */
    PURIFY_STAGE_RUNNING,    /**< 吹粉中：吹粉钮图标旋转、胶囊改文案 */
    PURIFY_STAGE_DONE,       /**< 吹粉完成：提示条改口罩提醒 */
};

void set_pause_ui(bool paused);            /* 定义见下方（打印态浮层的运行组） */
void purify_stage_set(purify_stage_t s);   /* 同上 */
void apply_print_phase(void);              /* 同上 */
bool phase_is_finished(void);              /* 同上 */
extern print_phase_t  g_print_phase;
extern purify_stage_t g_purify_stage;
extern int            g_print_percent;
extern lv_obj_t *g_monitor_photo_btn;
extern lv_obj_t *g_result_photo_btn;
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
    purify_stage_set(PURIFY_STAGE_IDLE);
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
        /* 吹粉成功后徽标不变（稿 4045:18253 仍是 TERMINATED/COMPLETE），
         * 只有状态栏、提示条和取件胶囊的文案跟着 stage 走。 */
        g_result_purify_done = true;
        purify_stage_set(PURIFY_STAGE_DONE);
    } else {
        purify_stage_set(PURIFY_STAGE_IDLE);
        ui_toast_show("Powder cleaning failed, video retained", ui_color_danger, 2600);
    }
}

void on_clean_close_rsp_async(void * /*user*/)
{
    g_result_cleaning = false;
    if(!g_result_purify_done) {
        purify_stage_set(PURIFY_STAGE_IDLE);
    }
}

void home_env_init(void)
{
    /* 环境订阅与默认值都在 ui_device_state（由 ui_app.cpp 初始化一次），本页只
     * 挂观察者——register_env_update 是单槽的，这里再订阅一次会把常驻状态栏的
     * 数据源顶掉。 */
    ui_device_state_add_observer(on_device_state);

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

// void home_set_status(user_home_status_t s)
// {
//     g_snap.status = s;
//     /* 打印中/打印结果页的状态由前端说了算，锁住不让环境推送覆盖 */
//     g_status_locked = (s == USER_HOME_STATUS_PRINTING ||
//                        s == USER_HOME_STATUS_PRINT_DONE ||
//                        s == USER_HOME_STATUS_PRINT_TERMINATED);
// }

void exit_monitor_mode(void);


void home_trigger_door_open(void)
{
     /* 从监控页滑动开门时先退出监控态，避免回首页后二次进入预览被早退拦截。 */
     exit_monitor_mode();
     /* 走 control 的门弹窗流程（Opening→DoorOpened→DoorClosed，含 PM2.5 净化检查/
     * 最短停留/关门轮询），确认关门后回首页。不再直接 send_open_door()。 */
     control_open_door_request();
}

/* ---------- 顶部按钮行（与外壳常驻设备信息条同高、同一条带子） ---------- */
lv_obj_t *g_topbar       = nullptr;   /**< 按钮行根（打印中浮于摄像头之上） */
lv_obj_t *g_btn_files    = nullptr;   /**< 右上角 "Start Print" 渐变胶囊 */

/* ---------- 首页左卡（HOME 空气质量·可切换 PM2.5/PM10） ---------- */
lv_obj_t *g_home_card_title = nullptr;  /**< "HOME PM2.5" / "HOME PM10" */
lv_obj_t *g_home_card_level = nullptr;  /**< 分级大字 Good/Fair/Poor/Very Poor */
lv_obj_t *g_home_card_value = nullptr;  /**< 灰色小字 "(36.5 ug/m3)" */
lv_obj_t *g_home_seg_track  = nullptr;  /**< 4 段色条容器（用于计算游标 x） */
lv_obj_t *g_home_seg_dot    = nullptr;  /**< 分级游标圆点 */
int       g_home_metric_idx = 0;        /**< 0=PM2.5 1=PM10（切换按钮，重启复位） */

/* ---------- 首页右侧 CABIN CONDITION（5 行数值） ---------- */
lv_obj_t *g_cab_pm25_val  = nullptr;    /**< 舱内 PM2.5 分级词 Good/Fair/Poor/Very Poor */
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

/* ---------- 打印态：摄像头整屏 + 六态浮层 ---------- */
lv_obj_t *g_print_area      = nullptr; /**< 摄像头 + 打印态浮层容器（默认隐藏） */
lv_obj_t *g_camera_viewport = nullptr; /**< 摄像头画面容器 */
lv_obj_t *g_camera_hint     = nullptr; /**< 占位提示 */
lv_obj_t *g_camera_image    = nullptr; /**< 真实视频帧图像槽（先隐藏） */
bool      g_print_paused      = false;   /**< 当前是否暂停（乐观切换，后端回执校正） */
bool      g_printing_active   = false;

/* ---------- 打印态浮层：六态共用一批控件，挂在 g_print_area 内 ----------
 * 摄像头背景与 CameraPreviewPlayer 全程沿用同一套，避免停流重开导致黑屏闪烁。
 * 浮层在退出打印态时只隐藏不销毁：结果态下从侧栏切走再切回首页要能原样恢复。 */
lv_obj_t *g_print_overlay      = nullptr; /**< 浮层根（默认隐藏） */
/* stats-card（前缀 pc_ = print card） */
lv_obj_t *g_pc_badge_icon      = nullptr; /**< 徽标图标盒（转圈/三角/圆圈感叹/勾） */
lv_obj_t *g_pc_badge_text      = nullptr; /**< "PRINTING..." / "PRINT COMPLETE" … */
lv_obj_t *g_pc_percent         = nullptr; /**< 徽标行右端百分比 */
lv_obj_t *g_pc_bar             = nullptr; /**< 进度条 */
lv_obj_t *g_pc_time_cap        = nullptr; /**< "TIME LEFT" ↔ "TIME COST" */
lv_obj_t *g_pc_time_val        = nullptr;
lv_obj_t *g_pc_material_val    = nullptr;
lv_obj_t *g_pc_material_unit   = nullptr; /**< CONSUMABLES 单位 "G"（无数据时隐藏） */
/* 提示条（前缀 pn_ = print notice） */
lv_obj_t *g_pn_root            = nullptr;
lv_obj_t *g_pn_icon_box        = nullptr; /**< 图标底色块（信息蓝 / 告警红） */
lv_obj_t *g_pn_icon            = nullptr;
lv_obj_t *g_pn_title           = nullptr;
lv_obj_t *g_pn_sub             = nullptr;
/* 底部两组控件（互斥显示） */
lv_obj_t *g_ctl_run            = nullptr; /**< 运行组：暂停/恢复 + 终止 */
lv_obj_t *g_ctl_collect        = nullptr; /**< 取件组：吹粉 + 滑动取件 */
lv_obj_t *g_btn_pause          = nullptr;
lv_obj_t *g_pause_bars         = nullptr; /**< 暂停钮的两根竖条（运行中显示） */
lv_obj_t *g_pause_tri          = nullptr; /**< 暂停钮的实心三角（暂停时显示） */
lv_obj_t *g_purify_btn         = nullptr;
lv_obj_t *g_purify_icon        = nullptr; /**< 吹粉图标盒（吹粉中整体旋转） */
lv_obj_t *g_result_photo_btn   = nullptr; /**< 拍照钮（与监控页共用在途标志） */
ui_slide_unlock_t *g_result_slide = nullptr; /**< "SWIPE TO COLLECT" */
char      g_result_job_name[96] = {0}; /**< 清粉录像目录键（优先用 manifest.frames_dir） */
bool      g_result_cleaning = false;
bool      g_result_purify_done = false; /**< 已成功清粉一次，禁止再次触发 */
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

/** 同步暂停/恢复钮：暂停态钮内显示实心三角（表示可恢复），运行态显示两根竖条。
 *  同时把徽标档在 PRINTING ↔ PAUSE 之间切换（结束态下收到回执则不动）。 */
void set_pause_ui(bool paused)
{
    g_print_paused = paused;
    if(g_pause_bars) {
        if(paused) lv_obj_add_flag(g_pause_bars, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_remove_flag(g_pause_bars, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_pause_tri) {
        if(paused) lv_obj_remove_flag(g_pause_tri, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(g_pause_tri, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_print_phase == PRINT_PHASE_PRINTING || g_print_phase == PRINT_PHASE_PAUSED) {
        g_print_phase = paused ? PRINT_PHASE_PAUSED : PRINT_PHASE_PRINTING;
        apply_print_phase();
    }
}

/** 暂停/恢复：乐观切换图标并发请求；后端回执经 register_print_pause_done 校正。
 *  不弹 toast——徽标行已经在 PRINTING... ↔ PAUSE 之间切，状态栏也同步，再飘一条
 *  提示只是噪音。 */
void on_pause_click(lv_event_t *)
{
    const bool want_pause = !g_print_paused;
    set_pause_ui(want_pause);
    if(want_pause) {
        ui::UiMsgProxy::instance().send_print_pause();
    } else {
        ui::UiMsgProxy::instance().send_print_resume();
    }
}

/** 按钮内图标：A8 掩膜 recolor 成白色，等比缩放到 target；外套固定尺寸盒，
 *  保证在 flex 布局里只占 target×target（避免大尺寸源图撑开按钮）。
 *  源图边长从图头取，96×96（home_btn_icons）与 40×40（home_metric_icons）通吃。 */
lv_obj_t *make_btn_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t target)
{
    return ui_user_make_icon_img(parent, src, target, lv_color_white());
}

/** 右上角 "Start Print" 主按钮：蓝色对角渐变玻璃胶囊 + 白色亮边 + 图标 + 文字
 *  （对齐 Figma node 2513:19772；无中文字库，文字用英文 "Start Print"）。
 *  胶囊样式已提到 ui_user_make_primary_pill_btn()，与打印历史空态的同名按钮同源。 */
lv_obj_t *build_files_button(lv_obj_t *parent)
{
    return ui_user_make_primary_pill_btn(parent, &ic_folder, "Start Print",
                                         on_files_click);
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

/** 顶部按钮行：右侧 "查看监控"圆钮 + "Start Print" 按钮。
 *  左半边（设备名 + 状态点 + 状态文字）已交给外壳的常驻条 ui_device_bar，本行只
 *  保留右侧两颗按钮；行高仍取 USER_TOPBAR_H，与常驻条同高、同起点，视觉上是稿
 *  3540:14473 里那一条带子。 */
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
     * move_background() 压到本行之下（见 page_user_home_enter_printing_mode），
     * 这条 100%×TOPBAR_H 的带子若可点，会把结果页贴顶那颗圆钮和 HOME PM2.5 卡的
     * 触摸全部吃掉。行内两个真按钮是 lv_button_create，各有独立命中区。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    /* 用一个 flex_grow 撑开把按钮顶到右边（左半边现在是空的，仍靠它定位） */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    /* 顺序对齐设计稿 3540:14473：摄像头圆钮在左，开始打印按钮贴最右边缘。
     * 右端位置由上面 spacer 的 flex_grow 推出，两钮间隙由 bar 的 pad_column 决定，
     * 都不要写死 x 坐标——否则换分辨率档或按钮文案变宽就要回来补数字。 */
    g_btn_camera = build_camera_button(bar);
    g_btn_files  = build_files_button(bar);

    return bar;
}

/* ============================================================================
 *  首页 normal 视图（对齐 Figma 1.1.1 待机中）
 *    左：HOME 空气质量磨砂卡（PM2.5/PM10 可切换 + 四档色条 + 游标）
 *    右：CABIN CONDITION 悬浮 5 行（PM2.5/TEMP/O2/HUMIDITY/PRESSURE）
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

/* CABIN 单行：[图标] 标签 ...(撑开)... 数值 + 单位。
 * unit_text 为空 → 无单位（PM2.5 行填的是分级词而非数值）。 */
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

    /* 首行按稿 3540:14473 从 AQI 换成舱内 PM2.5；ic_aqi 本就是稿 3540:14510 的
     * 空气/云字形，与新稿这行的图标一致，故图标资源不变。 */
    make_cabin_row(panel, &ic_aqi,      "PM2.5",    nullptr,       &g_cab_pm25_val, nullptr);
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

/* ============================================================================
 *  打印态浮层（六态一套：稿 3588:8735 工作中 / 4045:17582 暂停 /
 *  3589:9992 中断 / 3588:8907 完成 / 4234:6385 吹粉中 / 4045:18253 吹粉完成）
 *  ----------------------------------------------------------------------------
 *  六态共用同一批控件，只换徽标行（图标/文案/配色）、进度条色系、两列语义与底部
 *  控件组，挂在 g_print_area 内，摄像头背景与预览播放器全程不断流：
 *    ┌────────────────────────────────────────────────────────┐
 *    │ camera_viewport（整屏背景）                               │
 *    │                   右上  HOME PM2.5 卡 + 5 列环境条        │
 *    │                                        右侧中  拍照圆钮   │
 *    │  左下 提示条（仅结束态）                                  │
 *    │       stats-card（缩略图 + 徽标行 + 进度条 + 两列指标）     │
 *    │                        右下  控件组（运行组 / 取件组）     │
 *    └────────────────────────────────────────────────────────┘
 *  运行组 = 暂停/恢复钮 + 终止钮；取件组 = 吹粉钮 + 滑动取件胶囊。两组互斥。
 *  设计稿按 1280×800 出，这里不写死坐标，全部走主题常量 + 边缘对齐，
 *  1024×600 上按同一比例收缩。
 * ========================================================================== */

/* 六态主色（稿 4156:9744 / 4166:9015 / 4156:9794）：徽标文字、百分比、进度条填充
 * 共用一个主色，轨道是同色 10%（打印中那档稿上是白 4%）。稿里这三个值和
 * user_color_* 那套不是同一份色板（如完成绿稿是 #37e47b，主题里是 #52ff92），
 * 打印态严格按稿走，所以在本页单列常量而不是套用主题色。 */
constexpr uint32_t kPhaseBlue      = 0x6476ff;   /* 徽标/百分比 */
constexpr uint32_t kPhaseBlueFillA = 0x4258ff;   /* 进度条填充：左 */
constexpr uint32_t kPhaseBlueFillB = 0x586bff;   /* 进度条填充：右 */
constexpr uint32_t kPhaseOrange    = 0xff7843;
constexpr uint32_t kPhaseOrangeB   = 0xff8454;
constexpr uint32_t kPhaseGreen     = 0x37e47b;
constexpr uint32_t kPhaseGreenB    = 0x5ceb95;

/* 底部圆钮渐变（稿 4231:5721 / 4232:6251，139.4° 两停靠点）。LVGL 的简易渐变只有
 * 水平/垂直两向，斜向用水平近似——钮只有 64px，肉眼看不出角度差。 */
constexpr uint32_t kCtlBlueA = 0x4054fd;
constexpr uint32_t kCtlBlueB = 0x4d80ee;
constexpr uint32_t kCtlRedA  = 0xfd4242;
constexpr uint32_t kCtlRedB  = 0xfe6868;

print_phase_t  g_print_phase   = PRINT_PHASE_PRINTING;
purify_stage_t g_purify_stage  = PURIFY_STAGE_IDLE;
int            g_print_percent = 0;   /**< 最后一次进度百分比（结束态沿用它显示） */

/* 定义都在本块下半段（要先有控件句柄），这里前置声明给事件回调用。 */
void apply_print_phase(void);
void purify_stage_set(purify_stage_t stage);

/** 圆形玻璃钮（稿 3896:14303 那档灰玻璃）：右侧中部的拍照钮用它。
 *  out_btn 回传钮本体用于置灰。 */
lv_obj_t *make_glass_round_btn(lv_obj_t *parent, const lv_image_dsc_t *icon,
                               lv_event_cb_t cb, lv_obj_t **out_btn)
{
    const int32_t sz = USER_PRINT_CTL_BTN;

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
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = make_btn_icon(btn, icon, sz / 2);
    lv_obj_center(ic);
    if(out_btn) *out_btn = btn;
    return btn;
}

/** 底部控件组的彩色圆钮（稿 4231:5721 / 4232:6251）：对角渐变 + 1px 白 40% 亮边。
 *  icon 允许为空——暂停钮的图标是两根竖条，由调用方自己摆。 */
lv_obj_t *make_ctl_round_btn(lv_obj_t *parent, uint32_t grad_a, uint32_t grad_b,
                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_PRINT_CTL_BTN, USER_PRINT_CTL_BTN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(grad_a), 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(grad_b), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    return btn;
}

/** 控件组的玻璃底衬容器（稿里的 Union 布尔形状）。
 *
 *  稿上那条底衬是「圆 blob + 细腰 + 胶囊」并集后统一填黑 50%，而半透明色块直接
 *  相互重叠会在交叠处叠出更深的一块。LVGL 对 opa < COVER 的父节点会先把子树画进
 *  独立图层、再整体混合，所以这里让子块全用不透明黑、由容器统一压到 50%，拼出来
 *  才是一整片均匀的底衬。子块描边同理要按 20% 给（净化后≈稿上的白 10%）。 */
lv_obj_t *make_ctl_backdrop(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *bd = lv_obj_create(parent);
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, w, h);
    lv_obj_set_style_bg_opa(bd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(bd, LV_OPA_50, 0);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    return bd;
}

/** 底衬里的一块：waist=false 走全圆角（blob / 胶囊），true 走圆角细腰。
 *  细腰保留上下边并做小圆角，让两端过渡更像水滴，不再是生硬矩形。 */
void add_backdrop_block(lv_obj_t *bd, int32_t x, int32_t y,
                        int32_t w, int32_t h, bool waist)
{
    lv_obj_t *blk = lv_obj_create(bd);
    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, w, h);
    lv_obj_set_pos(blk, x, y);
    lv_obj_set_style_bg_color(blk, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(blk, waist ? (h / 2) : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(blk, 1, 0);
    lv_obj_set_style_border_color(blk, lv_color_white(), 0);
    lv_obj_set_style_border_opa(blk, LV_OPA_20, 0);
    if(waist) {
        lv_obj_set_style_border_side(
            blk, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    }
    lv_obj_remove_flag(blk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(blk, LV_OBJ_FLAG_SCROLLABLE);
}

/* Purify：触发打印结果页清粉测试（后端走 clean_test_start/close + TLV 0x22）。
 * 一次打印结果仅允许成功清粉一次。 */
void on_result_purify_click(lv_event_t * /*e*/)
{
    if(g_result_cleaning || g_result_purify_done) {
        return;
    }
    g_result_cleaning = true;
    ui::UiMsgProxy::instance().send_clean_test_start(g_result_job_name, 0, 0, 0);
    purify_stage_set(PURIFY_STAGE_RUNNING);
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
    if(g_result_cleaning || g_purify_stage == PURIFY_STAGE_RUNNING) {
        ui_toast_show("Air purifying, please wait", user_color_accent, 1200);
        return;
    }
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

/** 左下 stats-card（稿 4156:9739 一张卡走六态）：左缩略图 + 右列（徽标行 +
 *  进度条 + TIME/CONSUMABLES 两列）。徽标文案/配色、两列 caption 与进度条色系
 *  都由 apply_print_phase() 按当前态改写，这里只搭骨架。 */
lv_obj_t *build_print_stats_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, USER_PRINT_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 稿上 padding/gap 都是 16 → PAD_SM（14/11），不是 USER_PAD（28/22） */
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(card, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, USER_RADIUS / 2, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：打印件缩略图。后端还没有切片预览图，先留中性灰块 + 图片图标占位。 */
    lv_obj_t *thumb = lv_obj_create(card);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, USER_PRINT_THUMB_SZ, USER_PRINT_THUMB_SZ);
    lv_obj_set_style_bg_color(thumb, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *thumb_icon = lv_label_create(thumb);
    lv_label_set_text(thumb_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(thumb_icon, user_color_text_dim, 0);
    lv_obj_set_style_text_font(thumb_icon, user_font_value(), 0);
    lv_obj_center(thumb_icon);

    /* 右列：徽标行 + 进度条 + 两列指标，三段之间稿上都是 gap 16 */
    lv_obj_t *right = lv_obj_create(card);
    lv_obj_remove_style_all(right);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* 徽标行：图标 + 状态词（左）… 百分比（右），稿上两者同色 */
    lv_obj_t *head = lv_obj_create(right);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    g_pc_badge_icon = ui_user_make_icon_img(head, &ic_badge_spin,
                                           USER_PRINT_BADGE_SZ,
                                           lv_color_hex(kPhaseBlue));

    g_pc_badge_text = lv_label_create(head);
    lv_label_set_text(g_pc_badge_text, "PRINTING...");
    lv_obj_set_style_text_font(g_pc_badge_text, user_font_caption(), 0);
    lv_obj_set_style_text_letter_space(g_pc_badge_text, 1, 0);

    lv_obj_t *spacer = lv_obj_create(head);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    g_pc_percent = lv_label_create(head);
    lv_label_set_text(g_pc_percent, "0%");
    lv_obj_set_style_text_font(g_pc_percent, user_font_caption(), 0);
    lv_obj_set_style_text_letter_space(g_pc_percent, 1, 0);

    /* 进度条：稿上 8 高、radius 2（不是全圆胶囊），轨道与填充都随态换色 */
    g_pc_bar = lv_bar_create(right);
    lv_obj_remove_style_all(g_pc_bar);
    lv_obj_set_size(g_pc_bar, LV_PCT(100), USER_PRINT_PROG_H);
    lv_obj_set_style_bg_opa(g_pc_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_pc_bar, USER_PRINT_PROG_H / 4 + 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_pc_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(g_pc_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_pc_bar, USER_PRINT_PROG_H / 4 + 1, LV_PART_INDICATOR);
    lv_bar_set_range(g_pc_bar, 0, 100);
    lv_bar_set_value(g_pc_bar, 0, LV_ANIM_OFF);

    /* 两列指标：caption 随态在 TIME LEFT / TIME COST 之间切换 */
    lv_obj_t *body = lv_obj_create(right);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *col_time = result_stat_col(body, "TIME LEFT", nullptr,
                                        &g_pc_time_val, nullptr);
    g_pc_time_cap = lv_obj_get_child(col_time, 0);
    result_stat_col(body, "CONSUMABLES", "G", &g_pc_material_val,
                    &g_pc_material_unit);
    return card;
}

/** 卡片上方的常驻提示条（稿 4234:6375 / 4045:18227）：色块图标 + 标题 + 副标题。
 *  两个变体（信息蓝 / 告警红）只换图标底色与两行文案，由 apply_print_phase() 定。 */
lv_obj_t *build_print_notice(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, USER_PRINT_CARD_W);
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(bar, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
    lv_obj_set_style_radius(bar, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 图标盒：稿上是 32 见方的实色圆角块，里面一枚 18 的白图标 */
    const int32_t box_sz = USER_PRINT_BADGE_SZ * 2;
    g_pn_icon_box = lv_obj_create(bar);
    lv_obj_remove_style_all(g_pn_icon_box);
    lv_obj_set_size(g_pn_icon_box, box_sz, box_sz);
    lv_obj_set_style_bg_opa(g_pn_icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_pn_icon_box, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(g_pn_icon_box, LV_OBJ_FLAG_SCROLLABLE);

    g_pn_icon = ui_user_make_icon_img(g_pn_icon_box, &ic_toast_info,
                                      box_sz * 9 / 16, lv_color_white());
    lv_obj_center(g_pn_icon);

    lv_obj_t *texts = lv_obj_create(bar);
    lv_obj_remove_style_all(texts);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(texts, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(texts, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);

    g_pn_title = lv_label_create(texts);
    lv_label_set_text(g_pn_title, "");
    lv_obj_set_style_text_color(g_pn_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_pn_title, user_font_text(), 0);

    g_pn_sub = lv_label_create(texts);
    lv_label_set_text(g_pn_sub, "");
    lv_obj_set_style_text_color(g_pn_sub, lv_color_white(), 0);
    lv_obj_set_style_text_opa(g_pn_sub, LV_OPA_60, 0);
    lv_obj_set_style_text_font(g_pn_sub, user_font_caption(), 0);
    return bar;
}

/** 运行组（稿 4231:5716）：暂停/恢复钮 + 终止钮，各坐在一颗玻璃 blob 上，
 *  中间一段细腰把两颗连成 Union 那条形状。 */
lv_obj_t *build_print_controls_run(lv_obj_t *parent)
{
    const int32_t btn  = USER_PRINT_CTL_BTN;
    const int32_t blob = btn + USER_PAD_SM;          /* 稿 64 → 80 */
    const int32_t gap  = USER_PAD_SM / 2;            /* 稿 8 */
    const int32_t pad  = (blob - btn) / 2;
    const int32_t w    = blob * 2 + gap;

    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, w, blob);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bd = make_ctl_backdrop(grp, w, blob);
    add_backdrop_block(bd, 0, 0, blob, blob, false);
    add_backdrop_block(bd, blob + gap, 0, blob, blob, false);
    add_backdrop_block(bd, blob - 2, (blob - blob / 2) / 2,
                       gap + 4, blob / 2, true);

    /* 暂停钮：运行态是两根竖条（稿 4231:5723/5724 的 4×16，间距 3），
     * 暂停态换成实心三角。两套图形都建好，靠显隐切换。 */
    g_btn_pause = make_ctl_round_btn(grp, kCtlBlueA, kCtlBlueB, on_pause_click);
    lv_obj_set_pos(g_btn_pause, pad, pad);

    g_pause_bars = lv_obj_create(g_btn_pause);
    lv_obj_remove_style_all(g_pause_bars);
    lv_obj_set_size(g_pause_bars, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_pause_bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_pause_bars, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_pause_bars, btn * 3 / 64, 0);
    lv_obj_set_style_bg_opa(g_pause_bars, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(g_pause_bars, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_pause_bars, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(g_pause_bars);
    for(int i = 0; i < 2; ++i) {
        lv_obj_t *stick = lv_obj_create(g_pause_bars);
        lv_obj_remove_style_all(stick);
        lv_obj_set_size(stick, btn * 4 / 64, btn * 16 / 64);
        lv_obj_set_style_bg_color(stick, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(stick, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stick, 1, 0);
        lv_obj_remove_flag(stick, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(stick, LV_OBJ_FLAG_SCROLLABLE);
    }

    g_pause_tri = make_btn_icon(g_btn_pause, &ic_play, btn / 2);
    lv_obj_center(g_pause_tri);
    lv_obj_add_flag(g_pause_tri, LV_OBJ_FLAG_HIDDEN);

    /* 终止钮：稿 4232:6159 是一枚 15×16、radius 2 的白方块 */
    lv_obj_t *stop = make_ctl_round_btn(grp, kCtlRedA, kCtlRedB, on_terminate_click);
    lv_obj_set_pos(stop, blob + gap + pad, pad);
    lv_obj_t *square = lv_obj_create(stop);
    lv_obj_remove_style_all(square);
    lv_obj_set_size(square, btn * 15 / 64, btn * 16 / 64);
    lv_obj_set_style_bg_color(square, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(square, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(square, 2, 0);
    lv_obj_remove_flag(square, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(square, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(square);
    return grp;
}

/** 取件组（稿 4231:6034）：吹粉钮坐在左侧 blob 上，右边是滑动取件胶囊，
 *  底衬同样用「blob + 细腰 + 胶囊」三块拼。胶囊本体交给 ui_slide_unlock——
 *  它的红 knob + 提示文字 + 三箭头正是稿上那半条，只是轨道底改由底衬来画。 */
lv_obj_t *build_print_controls_collect(lv_obj_t *parent)
{
    const int32_t btn  = USER_PRINT_CTL_BTN;
    const int32_t blob = btn + USER_PAD_SM;
    const int32_t gap  = USER_PAD_SM / 2;
    const int32_t pad  = (blob - btn) / 2;
    const int32_t pill = USER_PRINT_PILL_W;
    const int32_t w    = blob + gap + pill;

    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, w, blob);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bd = make_ctl_backdrop(grp, w, blob);
    add_backdrop_block(bd, 0, 0, blob, blob, false);
    add_backdrop_block(bd, blob + gap, 0, pill, blob, false);
    add_backdrop_block(bd, blob - 2, (blob - blob / 2) / 2,
                       gap + 4, blob / 2, true);

    g_purify_btn = make_ctl_round_btn(grp, kCtlBlueA, kCtlBlueB,
                                     on_result_purify_click);
    lv_obj_set_pos(g_purify_btn, pad, pad);
    g_purify_icon = make_btn_icon(g_purify_btn, &ic_purify, btn / 2);
    lv_obj_center(g_purify_icon);

    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text        = "SWIPE TO COLLECT";
    cfg.on_unlocked      = on_result_collect_unlocked;
    cfg.track_w          = pill;
    cfg.track_h          = blob;
    cfg.custom_accent    = true;
    cfg.accent_color     = user_color_status_err;
    cfg.knob_icon        = &ic_door_solid;
    cfg.transparent_track = true;   /* 轨道底衬由 bd 统一画，避免半透明叠深 */
    g_result_slide = ui_slide_unlock_create(grp, &cfg);
    lv_obj_t *slide_root = ui_slide_unlock_get_root(g_result_slide);
    if(slide_root) {
        lv_obj_set_pos(slide_root, blob + gap, 0);
    }
    return grp;
}

/* 吹粉钮图标旋转：稿 4234:6385 的注释写「逆时针 180 度/秒」，即两秒一圈。
 * lv_image_set_rotation 的单位是 0.1°，逆时针就是让角度递减。 */
void purify_spin_exec(void *img, int32_t v)
{
    lv_image_set_rotation(static_cast<lv_obj_t *>(img), v);
}

void purify_spin_set(bool on)
{
    if(!g_purify_icon) {
        return;
    }
    lv_obj_t *img = lv_obj_get_child(g_purify_icon, 0);
    if(!img) {
        return;
    }
    lv_anim_delete(img, purify_spin_exec);
    if(!on) {
        lv_image_set_rotation(img, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_exec_cb(&a, purify_spin_exec);
    lv_anim_set_values(&a, 3600, 0);
    lv_anim_set_duration(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
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

    /* 侧栏底衬：画面整屏之后侧栏浮在视频上，未选中图标（user_color_icon_off
     * #555555）的底色由当前帧决定，亮帧下会被盖过去。这里补一条实心暗条，
     * 与 menu_panel_ 自身的 bg_opa 一起把图标底色固定下来。
     * 不透明度取 LV_OPA_70——与左渐变的起始值相同，两者在侧栏右边缘无缝衔接，
     * 合起来就是「恒定段 + 淡出段」，不会出现硬边。 */
    lv_obj_t *rail = lv_obj_create(parent);
    lv_obj_remove_style_all(rail);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(rail, USER_SIDEBAR_W, LV_PCT(100));
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(rail, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_70, 0);
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    struct {
        const lv_grad_dsc_t *grad;
        lv_align_t align;
        int32_t width_pct;
        int32_t x_ofs;
    } kMasks[2] = {
        /* 左遮罩让开侧栏起画：偏移量补上后它在画面里的位置与整屏改造前一致。 */
        { &s_mask_l, LV_ALIGN_LEFT_MID, 14, USER_SIDEBAR_W },
        { &s_mask_r, LV_ALIGN_RIGHT_MID, 20, 0 },
    };
    for(const auto &m : kMasks) {
        lv_obj_t *mask = lv_obj_create(parent);
        lv_obj_remove_style_all(mask);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(mask, LV_PCT(m.width_pct), LV_PCT(100));
        lv_obj_align(mask, m.align, m.x_ofs, 0);
        lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
        lv_obj_set_style_bg_grad(mask, m.grad, 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    }
}

lv_obj_t *build_print_overlay(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    /* 先认领句柄：结尾那次 apply_print_phase() 要靠它判断控件已就位 */
    g_print_overlay = layer;

    /* 右侧中部拍照钮（六态都有，稿上位置 RIGHT_MID） */
    make_glass_round_btn(layer, &ic_camera, on_result_photo_click,
                         &g_result_photo_btn);
    lv_obj_add_flag(g_result_photo_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_result_photo_btn, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);

    /* 右上环境区：HOME PM2.5 卡 + 5 列环境条两个独立容器，整体右对齐并让开拍照钮列。 */
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
                 -(USER_PAD + USER_PRINT_CTL_BTN + USER_PAD_SM), USER_PAD);

    build_result_pm_card(env_wrap);
    build_result_env_bar(env_wrap);

    /* 左下：stats-card 贴底，提示条压在它上方（稿上两者左边缘对齐、间距 8）。
     * layer 已随摄像头层铺到屏幕左边缘，左对齐的东西要补上侧栏宽度才不会钻到
     * 侧栏底下（侧栏是浮层且可点，压住的部分连触摸都收不到）。 */
    const int32_t left_ofs = USER_PAD + USER_SIDEBAR_W;
    lv_obj_t *card = build_print_stats_card(layer);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(card, LV_ALIGN_BOTTOM_LEFT, left_ofs, -USER_PAD);

    g_pn_root = build_print_notice(layer);
    lv_obj_add_flag(g_pn_root, LV_OBJ_FLAG_FLOATING);
    lv_obj_align_to(g_pn_root, card, LV_ALIGN_OUT_TOP_LEFT, 0, -USER_PAD_SM / 2);

    /* 右下：两组控件同一位置互斥显示 */
    g_ctl_run = build_print_controls_run(layer);
    lv_obj_add_flag(g_ctl_run, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_ctl_run, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD, -USER_PAD);

    g_ctl_collect = build_print_controls_collect(layer);
    lv_obj_add_flag(g_ctl_collect, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_ctl_collect, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD, -USER_PAD);

    apply_print_phase();
    return layer;
}

/* ---------------------------------------------------------------------------
 *  六态外观切换：一处集中改徽标/配色/两列语义/底部控件组/状态栏/提示条。
 *  数值本身（百分比、剩余时间、耗时耗材）由各自的 update 入口写，这里不碰。
 * ------------------------------------------------------------------------- */

/** 当前态的主色（徽标文字、百分比、进度条）。 */
lv_color_t phase_accent(void)
{
    switch(g_print_phase) {
    case PRINT_PHASE_TERMINATED: return lv_color_hex(kPhaseOrange);
    case PRINT_PHASE_DONE:       return lv_color_hex(kPhaseGreen);
    case PRINT_PHASE_PRINTING:
    case PRINT_PHASE_PAUSED:
    default:                     return lv_color_hex(kPhaseBlue);
    }
}

/** 结束态（中断/完成）：底部换取件组、卡片两列换成实际耗时/耗材语义。 */
bool phase_is_finished(void)
{
    return g_print_phase == PRINT_PHASE_TERMINATED ||
           g_print_phase == PRINT_PHASE_DONE;
}

void apply_print_phase(void)
{
    if(!g_print_overlay) {
        return;
    }
    const bool     finished = phase_is_finished();
    const lv_color_t accent = phase_accent();

    /* 徽标行：图标 + 文案 + 百分比同色 */
    struct BadgeSpec { const lv_image_dsc_t *icon; const char *text; };
    const BadgeSpec kBadges[4] = {
        { &ic_badge_spin,  "PRINTING..."       },
        { &ic_badge_play,  "PAUSE"             },
        { &ic_badge_alert, "PRINT TERMINATED"  },
        { &ic_badge_check, "PRINT COMPLETE"    },
    };
    const BadgeSpec &badge = kBadges[g_print_phase];
    if(g_pc_badge_icon) {
        lv_obj_t *img = lv_obj_get_child(g_pc_badge_icon, 0);
        if(img) {
            lv_image_set_src(img, badge.icon);
            lv_obj_set_style_image_recolor(img, accent, 0);
        }
    }
    if(g_pc_badge_text) {
        lv_label_set_text(g_pc_badge_text, badge.text);
        lv_obj_set_style_text_color(g_pc_badge_text, accent, 0);
    }
    if(g_pc_percent) {
        lv_obj_set_style_text_color(g_pc_percent, accent, 0);
    }

    /* 进度条：填充是主色到浅一档的水平渐变；轨道打印中那档稿上是白 4%，
     * 结束两档是主色 10%。 */
    if(g_pc_bar) {
        uint32_t fill_b = kPhaseBlueFillB;
        lv_color_t fill_a = lv_color_hex(kPhaseBlueFillA);
        if(g_print_phase == PRINT_PHASE_TERMINATED) {
            fill_a = lv_color_hex(kPhaseOrange);
            fill_b = kPhaseOrangeB;
        } else if(g_print_phase == PRINT_PHASE_DONE) {
            fill_a = lv_color_hex(kPhaseGreen);
            fill_b = kPhaseGreenB;
        }
        lv_obj_set_style_bg_color(g_pc_bar, fill_a, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(g_pc_bar, lv_color_hex(fill_b), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(g_pc_bar, finished ? accent : lv_color_white(),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_pc_bar, finished ? 26 : 10, LV_PART_MAIN);
    }

    /* 两列语义：打印中/暂停给剩余时间 + 耗材占位；结束态才有实际耗时与耗材。 */
    if(g_pc_time_cap) {
        lv_label_set_text(g_pc_time_cap, finished ? "TIME COST" : "TIME LEFT");
    }

    /* 底部控件组二选一 */
    if(g_ctl_run) {
        if(finished) lv_obj_add_flag(g_ctl_run, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_remove_flag(g_ctl_run, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_ctl_collect) {
        if(finished) lv_obj_remove_flag(g_ctl_collect, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(g_ctl_collect, LV_OBJ_FLAG_HIDDEN);
    }

    /* 提示条只在结束态出现（稿上打印中/暂停两张都没有这条）。
     * 文案先按稿占位：吹粉前催吹粉，吹粉完成后无论 PM 高低都提醒戴口罩。 */
    if(g_pn_root) {
        if(!finished) {
            lv_obj_add_flag(g_pn_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_pn_root, LV_OBJ_FLAG_HIDDEN);
            const bool masked = (g_purify_stage == PURIFY_STAGE_DONE);
            if(g_pn_icon_box) {
                lv_obj_set_style_bg_color(g_pn_icon_box,
                    masked ? lv_color_hex(kPhaseOrange) : user_color_accent, 0);
            }
            if(g_pn_icon) {
                lv_obj_t *img = lv_obj_get_child(g_pn_icon, 0);
                if(img) lv_image_set_src(img, masked ? &ic_alert : &ic_toast_info);
            }
            if(g_pn_title) {
                lv_label_set_text(g_pn_title,
                    masked ? "Wear a mask before opening"
                           : (g_print_phase == PRINT_PHASE_DONE ? "Print finished"
                                                                : "Print stopped"));
            }
            if(g_pn_sub) {
                lv_label_set_text(g_pn_sub,
                    masked ? "Cabin powder may become airborne"
                           : "Purify the cabin before opening the door");
            }
        }
    }

    /* 取件胶囊文案：吹粉中改成吹粉提示，其余时候是滑动取件 */
    if(g_result_slide) {
        ui_slide_unlock_set_hint(g_result_slide,
            g_purify_stage == PURIFY_STAGE_RUNNING ? "AIR PURIFYING..."
                                                   : "SWIPE TO COLLECT");
        ui_slide_unlock_set_enabled(g_result_slide,
            g_purify_stage != PURIFY_STAGE_RUNNING);
    }

    /* 常驻状态栏：暂停与吹粉两个阶段都要在状态栏里体现。
     * 建页时也会走一次本函数（为的是把六态控件摆到初始态），那会儿还没进打印态，
     * 不能把状态栏写成 Printing，故只在打印态生效。 */
    if(!g_printing_active) {
        return;
    }
    ui_device_status_t st;
    if(g_purify_stage == PURIFY_STAGE_RUNNING) {
        st = UI_DEVICE_STATUS_PURIFYING;
    } else if(g_purify_stage == PURIFY_STAGE_DONE) {
        st = UI_DEVICE_STATUS_PURIFY_DONE;
    } else {
        switch(g_print_phase) {
        case PRINT_PHASE_PAUSED:     st = UI_DEVICE_STATUS_PAUSED; break;
        case PRINT_PHASE_TERMINATED: st = UI_DEVICE_STATUS_PRINT_TERMINATED; break;
        case PRINT_PHASE_DONE:       st = UI_DEVICE_STATUS_PRINT_DONE; break;
        case PRINT_PHASE_PRINTING:
        default:                     st = UI_DEVICE_STATUS_PRINTING; break;
        }
    }
    ui_device_state_set_status(st);
}

void purify_stage_set(purify_stage_t stage)
{
    g_purify_stage = stage;
    purify_spin_set(stage == PURIFY_STAGE_RUNNING);
    apply_print_phase();
}

lv_obj_t *build_printing_area(lv_obj_t *parent)
{
    /* 打印中：整块内容区全屏铺满。area 为浮层（脱离首页纵向 flex），
     * 覆盖包括状态栏在内的整个内容区；摄像头做背景，状态栏/底部面板浮于其上。
     * 负偏移跨回首页根节点的 pad_left，画面左边缘因此落在屏幕左边缘（侧栏之下）。
     * 宽度取屏宽而非 LV_PCT(100)：后者算的是父内容区，不含那段 padding。 */
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_HOR_RES, LV_PCT(100));
    lv_obj_set_pos(area, -USER_SIDEBAR_W, 0);
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

    /* 打印态浮层：六态共用一套控件，默认隐藏（进打印态时才显示） */
    build_print_overlay(area);
    lv_obj_add_flag(g_print_overlay, LV_OBJ_FLAG_HIDDEN);

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
    /* 几何同 build_printing_area：负偏移跨回 pad_left，画面铺到屏幕左边缘。 */
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_HOR_RES, LV_PCT(100));
    lv_obj_set_pos(area, -USER_SIDEBAR_W, 0);
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

    /* 左下：EXIT（补上侧栏宽度，避免被浮层侧栏压住而点不到） */
    lv_obj_t *exit_btn = build_monitor_exit_button(area);
    lv_obj_add_flag(exit_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(exit_btn, LV_ALIGN_BOTTOM_LEFT, USER_PAD + USER_SIDEBAR_W, -USER_PAD);

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
    /* 监控层移到背景：本页顶部按钮行浮于其上（外壳的常驻设备信息条与左侧导航栏
     * 本就在整页之上，不受这里的 z 序影响，所以查看监控时设备状态照常可见）。 */
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

/* 状态字与状态点配色已挪到 core/ui_device_state（ui_device_status_text/color），
 * 由常驻设备信息条使用；本页不再画那一组。 */

/* 旧打印底条那两个 PM2.5 方块（数值+进度条+分级文案）已随六态重排删除；
 * 打印态现在只保留结果页那条 5 列环境小条，取值逻辑见下方 res_env 部分。 */

/** 刷新左卡（HOME 空气质量）：按当前切换指标填 标题/分级/数值/游标位置。 */
void home_card_update(void)
{
    if(!g_home_card_title) {
        return;
    }
    const AirMetric &m = kAirMetrics[g_home_metric_idx];
    lv_label_set_text(g_home_card_title, m.title);

    const ui_device_snapshot_t &snap = *ui_device_state_get();
    const float v = (g_home_metric_idx == 0) ? snap.pm25_indoor
                                             : snap.pm10_indoor;
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

    const ui_device_snapshot_t &snap = *ui_device_state_get();
    const float v = (g_home_metric_idx == 0) ? snap.pm25_indoor
                                             : snap.pm10_indoor;
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
    /* 设备名/WiFi/状态点那一组不在本页——外壳的常驻条自己观察同一份快照。 */
    const ui_device_snapshot_t &snap = *ui_device_state_get();

    /* 首页左卡（HOME 空气质量·可切换 PM2.5/PM10：分级 + 四档色条 + 游标） */
    home_card_update();

    /* 首页右侧 CABIN CONDITION：首行是舱内 PM2.5 的分级词，与结果页环境条首列同源
     * （都用 kAirMetrics[0] 的 PM2.5 阈值表定档）；其余四行按哨兵判空。
     * 这里只换文字不设颜色：五行的白字统一由 make_cabin_row 一次设定，无数据时的
     * "--" 也保持白色，与下面四行的 update_cabin_metric 行为一致。按档位着色只用在
     * 结果页那条小环境条上。 */
    if(g_cab_pm25_val) {
        if(metric_invalid(snap.pm25_cabin)) {
            lv_label_set_text(g_cab_pm25_val, "--");
        } else {
            const int level = air_classify(kAirMetrics[0], snap.pm25_cabin, nullptr);
            lv_label_set_text(g_cab_pm25_val, kLevelName[level]);
        }
    }
    update_cabin_metric(g_cab_temp_val, g_cab_temp_unit, snap.temperature_c);
    update_cabin_metric(g_cab_o2_val,   g_cab_o2_unit,   snap.oxygen_percent);
    update_cabin_metric(g_cab_hum_val,  g_cab_hum_unit,  snap.humidity_percent);
    update_cabin_metric(g_cab_prs_val,  g_cab_prs_unit,  snap.pressure_kpa);

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

    /* 首页根节点跨过 content_panel_ 的左/顶 padding 铺满整屏，再用自己的 pad_left
     * 把内容区还原成侧栏右侧那块——顶栏按钮行/仪表盘/开门滑块的几何因此不变，只有
     * 摄像头层会再用一次负偏移探进左侧 padding 区做整屏预览。
     * 纵向不还原 pad_top：本页自己那条按钮行就占在那一格，正好落在外壳常驻设备
     * 信息条的下面（两者同高同起点，合成稿 3540:14473 的一整条带子）。
     * 尺寸不能用 LV_PCT(100)：那算的是父内容区，会被 pad_top 扣掉一条。
     * 摄像头层必须留在本节点内：切页是靠 SimplePage::on_exit() 隐藏根节点实现的，
     * 挂到 content_panel_ 上会导致打印中切去设置页时画面仍留在底下。 */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(page, -USER_SIDEBAR_W, -USER_TOPBAR_H);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_left(page, USER_SIDEBAR_W, 0);
    lv_obj_set_style_pad_row(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page_root = page;

    /* 先建首页主区（全屏整机背景浮层，置于最底），再建按钮行——按钮遂浮于背景之上。 */
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
    if(g_print_overlay) lv_obj_remove_flag(g_print_overlay, LV_OBJ_FLAG_HIDDEN);

    /* 六态从「打印中」起步：徽标、色系、底部运行组、状态栏一次刷到位 */
    g_print_phase   = PRINT_PHASE_PRINTING;
    g_purify_stage  = PURIFY_STAGE_IDLE;
    g_print_percent = 0;
    purify_spin_set(false);
    set_pause_ui(false);
    apply_print_phase();

    page_user_home_refresh(true);
    page_user_home_update_camera_preview_layout(UPC_CAMERA_STREAM_WIDTH, 0u);

    /* 进度与两列指标复位：耗材要等打完才有数，先按占位显示 */
    if(g_pc_bar)     lv_bar_set_value(g_pc_bar, 0, LV_ANIM_OFF);
    if(g_pc_percent) lv_label_set_text(g_pc_percent, "0%");
    if(g_pc_time_val) {
        if(file) {
            char dur[32];
            ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
            lv_label_set_text(g_pc_time_val, dur);
        } else {
            lv_label_set_text(g_pc_time_val, "--");
        }
    }
    if(g_pc_material_val)  lv_label_set_text(g_pc_material_val, "--");
    if(g_pc_material_unit) lv_obj_add_flag(g_pc_material_unit, LV_OBJ_FLAG_HIDDEN);

    page_user_home_request_camera_preview();
}

void page_user_home_enter_print_result(const control_print_result_t *result)
{
    if(!result || !g_printing_active || !g_print_overlay) {
        return;
    }

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
    if(g_result_slide) {
        ui_slide_unlock_reset(g_result_slide);
    }

    /* 完成/终止两稿的差异只有徽标档；进度条停在最后一次的百分比（完成态即 100%）。 */
    g_print_phase  = result->terminated ? PRINT_PHASE_TERMINATED : PRINT_PHASE_DONE;
    g_purify_stage = PURIFY_STAGE_IDLE;
    if(!result->terminated) {
        g_print_percent = 100;
    }
    purify_spin_set(false);
    if(g_pc_bar)     lv_bar_set_value(g_pc_bar, g_print_percent, LV_ANIM_OFF);
    if(g_pc_percent) lv_label_set_text_fmt(g_pc_percent, "%d%%", g_print_percent);

    /* 两列换成实际耗时/耗材；manifest 无条目（0 帧）时统一显示 "--" */
    if(g_pc_time_val) {
        if(!result->has_stats) {
            lv_label_set_text(g_pc_time_val, "--");
        } else {
            /* 稿上是 H:MM:SS（"2:23:23"），小时位不补零 */
            const uint32_t s = result->duration_sec;
            lv_label_set_text_fmt(g_pc_time_val, "%u:%02u:%02u",
                                  s / 3600u, (s / 60u) % 60u, s % 60u);
        }
    }
    if(g_pc_material_val) {
        /* material_g 由 config.json 反查，查不到时后端写 0 → 同样按无数据处理。
         * 无数据时连单位一起隐藏，避免出现 "-- G" 这种半截读法。 */
        const bool has_material = result->has_stats && result->material_g > 0.0f;
        if(has_material) {
            lv_label_set_text_fmt(g_pc_material_val, "%.2f",
                                  static_cast<double>(result->material_g));
        } else {
            lv_label_set_text(g_pc_material_val, "--");
        }
        if(g_pc_material_unit) {
            if(has_material) lv_obj_remove_flag(g_pc_material_unit, LV_OBJ_FLAG_HIDDEN);
            else             lv_obj_add_flag(g_pc_material_unit, LV_OBJ_FLAG_HIDDEN);
        }
    }

    apply_print_phase();
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

    /* 浮层整块收起并把六态复位到打印中，下次进打印态从头起步 */
    if(g_print_overlay) lv_obj_add_flag(g_print_overlay, LV_OBJ_FLAG_HIDDEN);
    g_print_phase   = PRINT_PHASE_PRINTING;
    g_purify_stage  = PURIFY_STAGE_IDLE;
    g_print_percent = 0;
    purify_spin_set(false);
    apply_print_phase();

    if(g_print_area)  lv_obj_add_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);
    if(g_normal_area) lv_obj_clear_flag(g_normal_area, LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)  lv_obj_clear_flag(g_door_float,  LV_OBJ_FLAG_HIDDEN);
    if(g_btn_files)   lv_obj_clear_flag(g_btn_files,   LV_OBJ_FLAG_HIDDEN);
    if(g_btn_camera)  lv_obj_clear_flag(g_btn_camera,  LV_OBJ_FLAG_HIDDEN);
    set_pause_ui(false);

    ui_device_state_set_status(UI_DEVICE_STATUS_READY);
    page_user_home_refresh(true);
}

void page_user_home_printing_update(const control_progress_t *p)
{
    if(!p || !g_printing_active) {
        return;
    }

    /* 结束态的卡片已经定格在最终数据上，进度推送迟到几帧也不该把它改回去 */
    if(phase_is_finished()) {
        return;
    }

    g_print_percent = p->progress_percent;
    if(g_pc_bar) {
        lv_bar_set_value(g_pc_bar, p->progress_percent, LV_ANIM_ON);
    }
    if(g_pc_percent) {
        lv_label_set_text_fmt(g_pc_percent, "%d%%", p->progress_percent);
    }
    if(g_pc_time_val) {
        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), p->remaining_minutes);
        lv_label_set_text(g_pc_time_val, dur);
    }
}
