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
#include "ctl_backdrop_img.h"
#include "ui_slide_unlock.hpp"
#include "config/cal_config.hpp"
#include "ui_device_state.hpp"
#include "ui_message_store.hpp"
#include "ui_notice_stack.hpp"
#include "ui_alarm_state.hpp"
#include "ui_clean_state.hpp"
#include "upc_contract.hpp"
#include "config/camera_resolution_profile.hpp"
#include "media_paths.hpp"


#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>

/**
 * @file page_user_home.cpp
 * @brief Snowpod 风格用户首页
 *
 * 视觉层次（对齐设计稿 5277:11571）：
 *   ┌────────────────────────────────────────────────┐
 *   │ (设备信息由外壳的常驻条画)      (通知浮层贴右上) │ ← topbar（空占位带）
 *   │ ┌ HOME PM2.5 ─────┐            ○─ Cabin condition
 *   │ │ Fair (36.5)     │  [Device]     TEMPERATURE  │
 *   │ │ ▂▂▂▂ ●         │               O2 CONC.     │
 *   │ └─────────────────┘               HUMIDITY     │
 *   │                                   PRESSURE     │
 *   │ [Monitor][Start Print]      [滑动开门 >>>]     │ ← 都是浮动
 *   └────────────────────────────────────────────────┘
 *
 * 环境快照与运行状态在 `core/ui_device_state`，本页作为观察者读取；顶栏左半边的
 * 设备名/状态由外壳的常驻条 `ui_device_bar` 画——稿 5277:11571 起顶栏右侧那两颗
 * 按钮已取消，两个入口都搬到左下的方块上，本页那条带子只剩占位作用。
 * 右上角是全局通知浮层 `ui_notice_stack` 的位置，本页只负责在打印态/监控态按环境区
 * 高度把它往下让（见 sync_notice_offset）。
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
    PRINT_PHASE_OPTICAL_CHECK,  /**< OPTICAL PATH CHECK 蓝；打印前光路检测 */
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

void on_clean_rejected(void)
{
    g_result_cleaning = false;
    purify_stage_set(PURIFY_STAGE_IDLE);
    ui_toast_show("Powder cleaning is busy", ui_color_danger, 1800);
}

void on_clean_done(bool success)
{
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

/**
 * 清粉回执：经 `ui_clean_state` 扇出，只处理本页发起的那一路。
 *
 * 那两个 Proxy 回调是单槽的、控制页的手动清粉也要用，所以订阅收到了
 * `core/ui_clean_state`（见该头文件开头的说明）。它保证回调已在 UI 线程，
 * 故这里不再 `lv_async_call`——原先那两个 `*_async` 包装因此收掉了。
 */
void on_clean_state_event(ui_clean_event_t ev, ui_clean_owner_t owner)
{
    if(owner != UI_CLEAN_OWNER_PRINT_RESULT) {
        return;   /* 控制页的手动清粉与结果页无关 */
    }
    switch(ev) {
        case UI_CLEAN_EVT_REJECTED:
            on_clean_rejected();
            break;
        case UI_CLEAN_EVT_DONE:
            /* 协议约定 result_code 只表结束类型，一律按成功处理，见 ui_clean_state */
            on_clean_done(true);
            break;
        case UI_CLEAN_EVT_ACCEPTED:
        default:
            break;   /* 点击时已切 RUNNING，受理无需额外动作 */
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

    ui::UiMsgProxy::instance().register_optical_path_progress(
        [](const middleware::OpticalPathProgressPayload& p) {
            page_user_home_optical_path_update(p.point_index, p.point_total, p.state);
        });

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
                char toast[160];
                std::snprintf(toast, sizeof(toast), "Photo saved to %s", UPC_PHOTO_DIR);
                ui_toast_show(toast, user_color_accent, 1500);
            } else {
                ui_toast_show("Photo save failed", ui_color_danger, 1500);
            }
        });

    /* clean_test 的受理与完成回执走 ui_clean_state（单槽回调与控制页共用）；
     * clean_test_close 的回执只有本页用，仍直接注册。 */
    ui_clean_state_add_observer(on_clean_state_event);

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

/* ---------- 顶部占位带（与外壳常驻设备信息条同高、同一条带子） ---------- */
lv_obj_t *g_topbar       = nullptr;   /**< 占位行根（打印中浮于摄像头之上） */

/* ---------- 首页左卡（HOME 空气质量·可切换 PM2.5/PM10） ---------- */
lv_obj_t *g_home_card_title = nullptr;  /**< "HOME PM2.5" / "HOME PM10" */
lv_obj_t *g_home_card_level = nullptr;  /**< 分级大字 Good/Fair/Poor/Very Poor */
lv_obj_t *g_home_card_value = nullptr;  /**< 灰色小字 "(36.5 ug/m3)" */
lv_obj_t *g_home_seg_track  = nullptr;  /**< 4 段色条容器（用于计算游标 x） */
lv_obj_t *g_home_seg_dot    = nullptr;  /**< 分级游标圆点 */
int       g_home_metric_idx = 0;        /**< 0=PM2.5 1=PM10（切换按钮，重启复位） */

/* ---------- 首页右侧 Cabin condition（4 行数值） ---------- */
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
lv_obj_t *g_action_tiles = nullptr;   /**< 左下两枚入口胶囊那一行（打印中/监控中隐藏） */
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
lv_obj_t *g_pc_card            = nullptr; /**< 卡片根（通知浮层要按它的高度抬起来） */
lv_obj_t *g_pc_badge_icon      = nullptr; /**< 徽标图标盒（转圈/三角/圆圈感叹/勾） */
lv_obj_t *g_pc_badge_text      = nullptr; /**< "PRINTING..." / "PRINT COMPLETE" … */
lv_obj_t *g_pc_layer           = nullptr; /**< 徽标行「当前层/总层」（无总层数时隐藏） */
lv_obj_t *g_pc_percent         = nullptr; /**< 徽标行右端百分比 */
lv_obj_t *g_pc_bar             = nullptr; /**< 进度条 */
lv_obj_t *g_pc_time_cap        = nullptr; /**< "TIME LEFT" ↔ "TIME COST" */
lv_obj_t *g_pc_time_val        = nullptr;
lv_obj_t *g_pc_material_val    = nullptr;
lv_obj_t *g_pc_material_unit   = nullptr; /**< CONSUMABLES 单位 "G"（无数据时隐藏） */
lv_obj_t *g_pc_thumb_img       = nullptr; /**< 打印态卡片缩略图 */
lv_obj_t *g_pc_thumb_icon      = nullptr; /**< 打印态卡片占位图标 */
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
/* 右上环境区（稿 4231:5739）：左边一张 HOME PM2.5 卡 + 右边一条 5 列环境条。
 *
 * 打印态浮层和待机监控层各挂一份，两层在建页时一起创建、靠显隐切换（见
 * page_user_home_create），所以句柄不能是单例——否则后建的那份会把先建的覆盖掉，
 * 先建的那层从此收不到刷新。两份都登记在 kEnvBlocks 里，刷新时一起写。 */
struct env_block_t {
    lv_obj_t *root;       /**< 整块的根：通知浮层要按它的高度往下让（sync_notice_offset） */
    lv_obj_t *pm_title;   /**< "HOME PM2.5" / "HOME PM10" */
    lv_obj_t *pm_level;   /**< 分级词 Good/Fair/Poor/Very Poor */
    lv_obj_t *pm_value;   /**< 灰色小字 "(25.0 ug/m3)" */
    lv_obj_t *temp_val;
    lv_obj_t *o2_val;
    lv_obj_t *hum_val;
    lv_obj_t *prs_val;
};
env_block_t g_env_print   = {};   /**< 打印态浮层那份 */
env_block_t g_env_monitor = {};   /**< 待机监控层那份 */
env_block_t *const kEnvBlocks[] = { &g_env_print, &g_env_monitor };

/* ---------- 待机监控（摄像头全屏）视图 ---------- */
lv_obj_t *g_monitor_area           = nullptr; /**< 监控浮层（默认隐藏，与 normal/print 互斥） */
lv_obj_t *g_monitor_camera_viewport= nullptr;
lv_obj_t *g_monitor_camera_hint    = nullptr;
lv_obj_t *g_monitor_camera_image   = nullptr; /**< 监控视频帧槽（复用同一 CameraPreviewPlayer） */
lv_obj_t *g_monitor_photo_btn      = nullptr; /**< 监控区拍照按钮 */
ui_slide_unlock_t *g_monitor_door  = nullptr; /**< 监控界面的滑动开门 */
bool      g_monitor_active         = false;
bool      g_monitor_photo_pending  = false;   /**< 拍照请求在途，防连按 */

bool thumb_path_exists(const char* path)
{
    if(!path || path[0] == '\0') {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

void apply_camera_preview_layout(uint32_t src_w, uint32_t src_h = 0);
void on_camera_viewport_size_changed(lv_event_t *e);
void enter_monitor_mode(void);
void exit_monitor_mode(void);

/** 预览布局（通用）：宽贴满 viewport；高度按等比缩放后做上下居中裁剪。
 *  打印区与监控区共用同一算法，只是作用于不同的 viewport/image。
 *
 *  裁剪与缩放现在由解码侧的 RGA 在色彩转换那一趟里做掉（见 camera_preview.cpp
 *  的 `resolve_output_geometry()`），所以流起来之后 src_w 就等于 viewport 宽，
 *  下面算出的 zoom 正好是 1.000，lv_image 走无缩放贴图快路径——整屏预览时这一步
 *  就是 8fps 与 30fps 的分界。留着这段 zoom 不是冗余：
 *    - 开流前（占位尺寸）以及解码侧退回 1:1 时，它仍是唯一把画面铺满的手段；
 *    - viewport 事后改尺寸时，它兜住解码几何还没跟上的那一拍（画面对、但慢）。
 *
 *  传入的 src_w 只是开流前的占位值：播放器活着就一律以它的上屏宽度为准，否则
 *  enter_printing_mode / enter_monitor_mode 里写死的 UPC_CAMERA_STREAM_WIDTH
 *  会把 zoom 又按回重采样档。 */
static void cam_apply_layout(lv_obj_t *vp, lv_obj_t *img, uint32_t src_w, uint32_t src_h)
{
    if(!img || !vp) {
        return;
    }
    if(ui::CameraPreviewPlayer::instance().active()) {
        const uint32_t out_w = ui::CameraPreviewPlayer::instance().stream_width();
        if(out_w > 0) {
            src_w = out_w;
        }
    }
    if(src_w == 0) {
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

/** 打印态与监控态两个摄像头层一起刷。
 *
 *  `CameraPreviewPlayer` 在首帧上屏时会回调过来告知输出几何，但它不知道自己这一
 *  路挂在哪个 viewport 上。只刷打印态的话，`enter_monitor_mode()` 里那次
 *  `cam_apply_layout()` 是在开流之前跑的（拿到的还是占位宽），监控层的 zoom 就会
 *  一直停在重采样档，整屏预览在监控态照样掉到 8fps。
 *  对隐藏的那一层多刷一次只是重设几何，没有副作用。 */
void apply_camera_preview_layout_all(uint32_t src_w, uint32_t src_h)
{
    cam_apply_layout(g_camera_viewport, g_camera_image, src_w, src_h);
    cam_apply_layout(g_monitor_camera_viewport, g_monitor_camera_image, src_w, src_h);
}

/* 两个 size_changed 只传开流前的占位宽：播放器活着时由 cam_apply_layout 统一
 * 换成它的上屏宽度，不在调用点各写一份。 */
void on_camera_viewport_size_changed(lv_event_t * /*e*/)
{
    apply_camera_preview_layout(UPC_CAMERA_STREAM_WIDTH);
}

void on_monitor_viewport_size_changed(lv_event_t * /*e*/)
{
    cam_apply_layout(g_monitor_camera_viewport, g_monitor_camera_image,
                     UPC_CAMERA_STREAM_WIDTH, 0);
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
    if(g_print_phase == PRINT_PHASE_OPTICAL_CHECK) {
        return;
    }
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

void on_camera_btn_click(lv_event_t * /*e*/)
{
    enter_monitor_mode();
}

/** 顶部占位带：左半边（设备名 + 状态点 + 状态文字）由外壳的常驻条 ui_device_bar
 *  画，右半边原有的"查看监控"圆钮与"Start Print"胶囊在稿 5277:11571 里已经取消，
 *  改成左下两枚入口胶囊（见 build_action_tiles）。
 *
 *  这条带子现在没有任何内容，但**不能删**：本页根节点是纵向 flex，其余几层
 *  （normal / print / monitor / 开门滑块 / 入口胶囊）全是 FLOATING 不占格位，
 *  它是唯一撑起那一格的子节点，删掉整页纵向布局要重新验证。行高仍取
 *  USER_TOPBAR_H，与常驻条同高、同起点。 */
lv_obj_t *build_topbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_TOPBAR_H);
    lv_obj_set_style_pad_hor(bar, USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(bar, USER_TOPBAR_PAD_VER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 纯布局容器，去掉 lv_obj_create 默认带的 CLICKABLE：打印态下 print_area 被
     * move_background() 压到本行之下（见 page_user_home_enter_printing_mode），
     * 这条 100%×TOPBAR_H 的带子若可点，会把结果页贴顶那颗圆钮和 HOME PM2.5 卡的
     * 触摸全部吃掉。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    return bar;
}

/* 蓝/红对角渐变（稿 4231:5721 / 4232:6251 的底部圆钮，139.4° 两停靠点）。
 * LVGL 的简易渐变只有水平/垂直两向，斜向用水平近似——圆钮只有 64px 看不出角度差。
 * 首页左下那枚"Start Print"胶囊是同一对蓝停靠点，但它走公共组件
 * ui_user_make_primary_pill_btn（那里另有一份真斜向 lv_grad_dsc_t），不吃这两个值。 */
constexpr uint32_t kCtlBlueA = 0x4054fd;
constexpr uint32_t kCtlBlueB = 0x4d80ee;
constexpr uint32_t kCtlRedA  = 0xfd4242;
constexpr uint32_t kCtlRedB  = 0xfe6868;

/* ============================================================================
 *  首页 normal 视图（对齐 Figma 5277:11571 待机中）
 *    左上：HOME 空气质量磨砂卡（PM2.5/PM10 可切换 + 四档色条 + 游标）
 *    右侧：Cabin condition 悬浮 4 行（TEMPERATURE/O2/HUMIDITY/PRESSURE）
 *    左下：两枚入口胶囊（Monitor / Start Print，稿 5339:27275）
 *    右下：滑动开门
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

/** 从 NULL 结尾的候选表里挑出边长最接近 sz 的那份原生位图。
 *
 *  **图标按档位出多份原生尺寸、运行时不缩放，是刻意的**。LVGL 的
 *  `lv_draw_sw_transform` 是 2 抽头点采样、不做面积平均，缩小时步长大于一个源
 *  像素就会整行整列跳过源数据：原先一张 40×40 喂三档，32 档实测横杠 alpha 在
 *  225..251 之间起伏、26 档 188..220 且峰值到不了实心，屏上就是毛刺加断续。
 *  资源现在按三档屏各出一份（生成脚本的 `METRIC_SIZES`），尺寸走
 *  `USER_METRIC_ICON_SZ`，正常都能精确命中、走不到缩放。
 *  同一套做法见 `ui_message_store.cpp` 的 `ui_message_severity_icon()`。
 *
 *  取最接近而不是只认精确相等，是为了监控态那枚返回钮——它的图标边长跟的是
 *  `USER_PRINT_CTL_BTN / 2`（48/32/25），1024 档的 25 与本表的 26 差 1px。
 */
const lv_image_dsc_t *pick_icon(const lv_image_dsc_t *const *set, int32_t sz)
{
    const lv_image_dsc_t *best = set[0];
    int32_t best_d = INT32_MAX;
    for(const lv_image_dsc_t *const *p = set; *p != nullptr; ++p) {
        const int32_t d = LV_ABS((int32_t)(*p)->header.w - sz);
        if(d < best_d) { best_d = d; best = *p; }
    }
    return best;
}

/** dim 着色的指标图标：取 set 里与 sz 等宽的那份，1:1 画进 sz 方盒、居中。
 *  尺寸对不上（改了主题却没同步生成脚本）才退回缩放，保证形不崩、只是糊。 */
lv_obj_t *make_dim_icon(lv_obj_t *parent, const lv_image_dsc_t *const *set, int32_t sz)
{
    const lv_image_dsc_t *src = pick_icon(set, sz);

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
    const int32_t sw = (int32_t)src->header.w;
    if(sw != sz) {
        lv_image_set_pivot(img, sw / 2, sw / 2);
        lv_image_set_scale(img, (uint32_t)(256 * sz / sw));
    }
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

/* 左卡：HOME 空气质量磨砂卡（对齐 Figma 5277:11664）。 */
lv_obj_t *build_home_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(card, USER_HOME_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* 内边距稿上是 20，取 USER_PAD（24）差 4px——20 不落在 16/24/32 这套梯度上，
     * 为它单开一个字段不值当。行距稿上是 24，正好是 USER_PAD。 */
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_set_style_pad_row(card, USER_PAD, 0);
    /* 稿上这张卡 fills 是空的、只有一层 backdrop blur(25px)，LVGL 没有背景模糊，
     * 仍用半透明深底 + 细白描边近似——纯靠模糊的话浅色背景处文字读不清。 */
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_radius(card, USER_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 稿 5339:27207 把这张卡摆在上半区（左下让给两枚入口胶囊），不再垂直居中。
     * 顶距原先写成 USER_TOPBAR_H + USER_PAD_LG*2，那是按旧稿 5277:11664 的 208 折算的；
     * 新稿量出来是 241，而间距梯度改成 32 后那个式子只算得 192、离稿更远，故独立成
     * USER_HOME_CARD_TOP。左边距 32 是稿上内容区的统一左内距，正好等于 USER_PAD_LG，
     * 但父节点 area 现在铺满整屏（背景要跨到屏幕左缘），所以左对齐还得补一个侧栏宽
     * ——同 build_monitor_area 里的 EXIT 钮。 */
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, USER_PAD_LG + USER_SIDEBAR_W,
                 USER_HOME_CARD_TOP);

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

    make_dim_icon(head, ic_home_set, USER_METRIC_ICON_SZ);

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
    /* 可点方框比图标本身大一圈（1280 档 32→40），给指尖留余量 */
    lv_obj_set_size(toggle, USER_METRIC_ICON_SZ * 5 / 4, USER_METRIC_ICON_SZ * 5 / 4);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(toggle, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(toggle, on_home_metric_toggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *tg_icon = make_dim_icon(toggle, ic_switch_set, USER_METRIC_ICON_SZ);
    lv_obj_center(tg_icon);

    /* 分隔线（稿 5277:11674 是白 30%，比旧稿那条亮一档） */
    lv_obj_t *div = lv_obj_create(card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(div, user_color_text, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_30, 0);

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
    /* 稿 5339:27457 这一格高 24：轨道 8 居中（上下各 8），游标 16 居中（上下各 4）。 */
    lv_obj_set_height(seg_wrap, 24);
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
    lv_obj_set_size(g_home_seg_dot, 16, 16);   /* 稿 5339:27463 */
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

/* Cabin 单行：[图标] 标签 ...(撑开)... 数值 + 单位（稿 5277:11591 起四行都带单位）。 */
lv_obj_t *make_cabin_row(lv_obj_t *parent, const lv_image_dsc_t *const *icon,
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

    make_dim_icon(row, icon, USER_METRIC_ICON_SZ);

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

    /* 单位可选：不带单位的行传 unit_text=nullptr，此时不建 label 并回传空指针，
     * 下游 update_cabin_metric() 靠判空决定要不要跟着数值一起隐藏。 */
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

/* 右侧 CABIN CONDITION：头部 + 引线分隔线 + 4 行（悬浮文本，无卡片底）。
 *
 * 稿 5339:27390/27436/27389/27442 的几何（1280×800 原生）：
 *   文字区（标题 + 四行）x=946 宽 294，右缘 1240（距屏右 40）
 *   分隔线              x=766 宽 474 —— 比文字区左缘再往左探 180，右端与文字区齐
 *   线左端的空心圆环     x=750 直径 16，圆心落在线上
 * 于是面板整体宽 = 引线段(180) + 圆环(16) + 文字区(294) = 490，标题与四行数据都相对
 * 面板左缘缩进 196，只有分隔线那一行独占全宽。
 *
 * 纵向节奏（相对标题顶）：标题 0..32、线 56、四行 80..256。圆环在稿里是绝对定位、
 * 骑在线上不占流高，而这里它在 flex 流里、把那一格撑成 16 高，所以三段间距统一取
 * USER_PAD_SM(16) 而不是稿上标注的 24——算下来线正好落在 56、四行正好从 80 起，
 * 与稿逐像素一致。**别改成 USER_PAD**，那会把四行整体推低 15px。 */
lv_obj_t *build_cabin_panel(lv_obj_t *parent)
{
    /* 圆环直径：1280 档 USER_STATUS_DOT_SZ*4/3 正好是稿上的 16。 */
    const int32_t ring_sz = USER_STATUS_DOT_SZ * 4 / 3;
    const int32_t indent  = USER_HOME_CABIN_LEAD_W + ring_sz;

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(panel, USER_HOME_CABIN_W + indent);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(panel, LV_ALIGN_RIGHT_MID, -USER_HOME_CABIN_PAD_RIGHT, 0);

    /* 头部：[cabin 图标] CABIN CONDITION —— 靠 margin_left 缩回文字区左缘。 */
    lv_obj_t *head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, USER_PAD_SM, 0);
    lv_obj_set_style_margin_left(head, indent, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    make_dim_icon(head, ic_cabin_set, USER_METRIC_ICON_SZ);
    lv_obj_t *htitle = lv_label_create(head);
    lv_label_set_text(htitle, "CABIN CONDITION");
    lv_obj_set_style_text_color(htitle, user_color_text, 0);
    lv_obj_set_style_text_opa(htitle, LV_OPA_50, 0);
    lv_obj_set_style_text_font(htitle, user_font_text(), 0);
    lv_obj_set_style_text_letter_space(htitle, 1, 0);

    /* 引线那一行：[空心圆环][分隔线吃掉剩余宽]，整行独占面板全宽。
     * 圆环与线都靠交叉轴居中，所以线恰好穿过圆心。 */
    lv_obj_t *lead = lv_obj_create(panel);
    lv_obj_remove_style_all(lead);
    lv_obj_set_size(lead, LV_PCT(100), ring_sz);
    lv_obj_set_flex_flow(lead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lead, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(lead, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lead, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ring = lv_obj_create(lead);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, ring_sz, ring_sz);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, user_color_text, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_30, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *div = lv_obj_create(lead);
    lv_obj_remove_style_all(div);
    lv_obj_set_height(div, 1);
    lv_obj_set_flex_grow(div, 1);
    lv_obj_set_style_bg_color(div, user_color_text, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_20, 0);

    /* 四行数据缩回文字区：套一层 pad_left 容器，而**不是**给行本身加左边距——行是
     * LV_PCT(100) 宽的，直接加边距会连宽度一起被推出面板右缘。
     * 舱内 PM2.5 那行按稿 5277:11590 去掉，只留温度/氧浓度/湿度/气压。 */
    lv_obj_t *rows = lv_obj_create(panel);
    lv_obj_remove_style_all(rows);
    lv_obj_set_width(rows, LV_PCT(100));
    lv_obj_set_height(rows, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(rows, indent, 0);
    lv_obj_set_style_pad_row(rows, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE);

    make_cabin_row(rows, ic_temp_set,     "TEMP",     "\xC2\xB0""C", &g_cab_temp_val, &g_cab_temp_unit);
    make_cabin_row(rows, ic_o2_set,       "O2 CONC.", "%",           &g_cab_o2_val,   &g_cab_o2_unit);
    make_cabin_row(rows, ic_humidity_set, "HUMIDITY", "%",           &g_cab_hum_val,  &g_cab_hum_unit);
    make_cabin_row(rows, ic_pressure_set, "PRESSURE", "kPa",         &g_cab_prs_val,  &g_cab_prs_unit);
    return panel;
}

/** 取当前 profile 的首页背景图。1280 档用「背景图1」——新首页稿 5339:27207 那层背景就是
 *  它的构图，设备右缘上屏落在 910、与 CABIN 文字左缘 946 之间正好是稿上的 36px 间隙。
 *  1024 与 1920 档用「背景图2」：它自带一段左移，配合那两档尚未折算到稿值的 CABIN 宽度
 *  才不会让设备压进文字（换成背景图1 的话 1024 档要压 3px、1920 档压 11px）。
 *
 *  背景图2 那段左移**在生成时又被右移 23px 抵掉一部分**：当初只校对了右侧那条间隙，
 *  没量左侧——机身左缘落在 394，而 HOME 卡右缘是 399，实机上压进卡里 5px。右移后
 *  1024 档两侧变成 18 / 19px。推导与 1920 档的连带影响见 tools/gen_home_bg.py 的 PROFILES。
 *
 *  两个节点怎么选的、哪档换图要连带改什么，见 tools/gen_home_bg.py 的注释。 */
const lv_image_dsc_t *home_bg_dsc()
{
    return (g_ui->profile_id == UI_RES_1280) ? &home_bg_1280 : &home_bg_1024;
}

/** 中部主区：全屏整机背景（RGB565）+ 左卡（空气质量）+ 右侧 CABIN 面板。
 *  area 是**铺满整屏**的浮层（含状态栏区域与侧栏之下那条），整机背景一直顶到最上；
 *  状态栏在其后创建 → 浮于背景之上，顶部不再留黑边。
 *
 *  用负偏移 + LV_HOR_RES 跨回本页根节点的 pad_left，与打印态/监控态的摄像头层同一套
 *  做法（见 build_printing_area）。**这一点不是可选的**：稿里背景铺满整屏 1280、侧栏是
 *  20% 黑的浮层压在背景之上，而背景层若只铺内容区，同一张 1.6 的图塞进更窄的框再从
 *  x=USER_SIDEBAR_W 起画，等效把设备整体往右推（1280 档 64px）。以前那笔账是靠
 *  gen_home_bg.py 把设备左移若干像素烤进资源来抵的，现在跨回整屏后 COVER 的结果与稿
 *  逐像素对齐，那个补偿已归零。改回只铺内容区就必须把补偿一起算回来。 */
lv_obj_t *build_main_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_HOR_RES, LV_PCT(100));
    lv_obj_set_pos(area, -USER_SIDEBAR_W, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 整机背景大图：铺满 area（现在就是整屏）、等比 cover、居中。资源按 profile 出两份
     * （见 tools/gen_home_bg.py），各自就是该档的屏幕原尺寸，所以 COVER 都落到 1.000、
     * 零重采样：1280 档 1280×800 严丝合缝，1024 档 1024×640 只纵向上下各裁 20px。
     * 1280 档原先是拿 1024×640 放大 1.25× 的，那是它发软的原因。
     * 旧注释里说的「只铺内容区、靠资源里 20px 左移补偿」已随 area 跨回整屏而作废，
     * 详见 app/ui/UI.md「首页待机布局」。 */
    lv_obj_t *bg = lv_image_create(area);
    lv_image_set_src(bg, home_bg_dsc());
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(bg, LV_IMAGE_ALIGN_COVER);
    lv_obj_center(bg);

    build_home_card(area);
    build_cabin_panel(area);

    return area;
}

/** 右下角浮动 "Slide to Unlock" 滑动解锁条（真正可拖动；滑到底触发开门） */
lv_obj_t *build_door_floating(lv_obj_t *parent)
{
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text  = "SLIDE TO UNLOCK";   /* 箭头由控件绘制 3 个 chevron */
    cfg.on_unlocked = on_door_slide_unlocked;
    cfg.user_data  = nullptr;
    /* 稿 5339:27265 的开门滑块：1280 档 400×80、圆头 64 内缩 8，与打印态取件胶囊同一套
     * 几何。走 USER_DOOR_SLIDE_* 而不是 USER_SLIDE_*——后者还被延时摄影页的拖动预览框
     * 和放版弹窗的最小宽钳制用着，见 ui_theme.hpp 的说明。 */
    cfg.track_w    = USER_DOOR_SLIDE_W;
    cfg.track_h    = USER_DOOR_SLIDE_H;
    cfg.knob_sz    = USER_DOOR_SLIDE_KNOB;
    cfg.custom_accent = true;                /* 设计稿为红色渐变滑块 */
    cfg.accent_color  = user_color_status_err;
    g_door_slide = ui_slide_unlock_create(parent, &cfg);

    lv_obj_t *root = ui_slide_unlock_get_root(g_door_slide);
    if(root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_FLOATING);
        /* 稿上这条退屏边 30/32，与左下入口胶囊行同一个退让量（USER_PAD_LG）。 */
        lv_obj_align(root, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_LG, -USER_PAD_LG);
    }
    return root;
}

/* ---------------------------------------------------------------------------
 *  左下两枚入口胶囊（稿 5339:27275）。稿 5277:11571 起顶栏右侧那颗"查看监控"圆钮
 *  与"Start Print"胶囊取消、两个入口搬到左下；稿 5339:27275 又把它们从方块改成
 *  一行等高胶囊：
 *    ┌───────────────────┐ ┌──────────────────────────┐
 *    │ [摄像机] Monitor    │ │ [文件夹]      Start Print  │
 *    └───────────────────┘ └──────────────────────────┘
 *  左边灰玻璃钮按内容撑宽，右边蓝钮 flex_grow 吃掉整行剩余宽（稿上蓝钮是 flex-1），
 *  整行总宽走 USER_HOME_ACTION_ROW_W。两枚都是全圆胶囊、等高 USER_BTN_PRIMARY_H×5/4
 *  （90/65/55，即 ui_user_make_primary_pill_btn 逼近稿上 64@1280 用的那个算法）。
 *  旧方块上那行静态提示语 "Place the build plate first" 在新稿里已删除。
 *
 *  稿上的描边是渐变（灰玻璃白 20%→10%→10%→20%，蓝钮白 40%→0%→40%），LVGL 画不了
 *  渐变边框，各取一个中间值当单色；灰玻璃那圈 inset 阴影与背景模糊同样画不出来，
 *  略去（同 make_glass_round_btn）。
 * ------------------------------------------------------------------------- */

/** 灰玻璃胶囊（稿 5339:27276「Button / On Light」的玻璃档）：图标 + 单行文字水平居中。
 *  材质与 make_glass_round_btn 同一份（0x757575 @ 20% + 白 20% 描边），只是形状从
 *  圆钮变胶囊。只有首页这一处在用，故不进 ui_user_widgets。 */
lv_obj_t *make_glass_pill_btn(lv_obj_t *parent, const lv_image_dsc_t *icon,
                              const char *text, lv_event_cb_t cb)
{
    /* 高度与图标边长都照抄 ui_user_make_primary_pill_btn 的算法（不是随手取的比例）：
     * 同一行里两枚胶囊必须严格等高，换成别的常量哪怕只差几像素也会看出来。 */
    const int32_t h = USER_BTN_PRIMARY_H * 5 / 4;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x757575), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_20, 0);
    /* 稿上是左 24 / 右 32@1280 的不对称内边距，这里两侧统一取 USER_PAD（1280 档 24）：
     * 左侧正好是稿值，右侧少 8px。同一行的蓝钮是公共组件（历史页空态也在用）、同样
     * 对称内边距，为这 8px 给它开一套不对称参数不值当。 */
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    make_btn_icon(btn, icon, USER_BTN_PRIMARY_H * 5 / 8);   /* 稿上 32@1280 = 胶囊高的一半 */

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    return btn;
}

lv_obj_t *build_action_tiles(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_FLOATING);
    /* 整行定宽（稿上外层容器就是定宽，灰钮 hug、蓝钮吃剩余），高度由胶囊撑。 */
    lv_obj_set_size(row, USER_HOME_ACTION_ROW_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* 稿上这一行的左缘距侧栏 32、底缘距屏底 32，与右下开门滑块同一个退让量
     * （USER_PAD_LG）。横向只退这一个值——本页根节点的 pad_left 已经让开侧栏，
     * 不用再补侧栏宽度（同 build_door_floating）。 */
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, USER_PAD_LG, -USER_PAD_LG);

    make_glass_pill_btn(row, &ic_camera, "Monitor", on_camera_btn_click);

    /* 蓝钮与打印历史空态那枚 Start Print 共用组件，保证两处是同一枚按钮；本处按稿
     * 定宽 + 图标贴左、文字贴右（spread），宽度交给 flex_grow 吃掉行内剩余。 */
    lv_obj_t *print = ui_user_make_primary_pill_btn(row, &ic_folder, "Start Print",
                                                    on_files_click, 0, true);
    lv_obj_set_flex_grow(print, 1);

    return row;
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

print_phase_t  g_print_phase   = PRINT_PHASE_PRINTING;
purify_stage_t g_purify_stage  = PURIFY_STAGE_IDLE;
int            g_print_percent = 0;   /**< 最后一次进度百分比（结束态沿用它显示） */

/* 定义都在本块下半段（要先有控件句柄），这里前置声明给事件回调用。 */
void apply_print_phase(void);
void purify_stage_set(purify_stage_t stage);

/** 圆形玻璃钮（稿 3847:9438 那档灰玻璃）：打印态/监控态右侧中部的拍照钮、
 *  监控态左下的返回钮都用它。out_btn 回传钮本体用于置灰。
 *  稿上还有一圈 inset 阴影，LVGL 没有内阴影，略去。 */
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
    lv_obj_set_style_border_opa(btn, LV_OPA_50, 0);
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

/** 控件组的玻璃底衬（稿 4796:6172 运行组 / 4231:6034 取件组的 Union 布尔形状）。
 *
 *  稿上这条形状是「圆 blob + 细腰 + 圆/胶囊」并集后又给结果路径加了圆角平滑，腰部
 *  是一段内凹圆滑过渡；填黑 50%，上面还叠一条 1px 白 10% 的内描边。这两样 LVGL 都
 *  画不出来：圆角矩形只能凸，且半透明块相互重叠时——注意 lv_obj_set_style_opa() 是
 *  逐控件继承的，触发整棵子树合成一次的是 opa_layered——交叠处会叠深，看着不像一个
 *  整体。所以底衬改由 tools/gen_ctl_backdrop.py 从设计稿 SVG 出图，运行时一张
 *  ARGB8888 一次混合，填充色和描边都已烤进像素，不用再调不透明度。
 *
 *  图按 profile 分三档预生成，尺寸与 blob/gap/pill 严格对齐，故不缩放。 */
lv_obj_t *make_ctl_backdrop(lv_obj_t *parent, const lv_image_dsc_t *dsc)
{
    lv_obj_t *bd = lv_image_create(parent);
    lv_image_set_src(bd, dsc);
    lv_obj_set_pos(bd, 0, 0);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    return bd;
}

/* 底衬几何：稿 4231:5716 里圆钮 64 坐在直径 80 的 blob 上、两块之间留 8 的缝，
 * 即 blob = btn×5/4、gap = btn/8。blob 的 +2 是四舍五入——直接整除的话 1024 档
 * （btn=51）会截成 63，比稿少 1px，底衬图就对不齐了。
 * 这两个算法必须与 gen_ctl_backdrop.py 里的同名换算保持一致。 */
int32_t ctl_blob(int32_t btn) { return (btn * 5 + 2) / 4; }
int32_t ctl_gap(int32_t btn)  { return btn / 8; }

/** 取当前 profile 的底衬图。run=true 要运行组那张（两端都是圆），否则取件组。 */
const lv_image_dsc_t *ctl_backdrop_dsc(bool run)
{
    switch(g_ui->profile_id) {
        case UI_RES_1920: return run ? &img_ctl_bd_run_1920 : &img_ctl_bd_collect_1920;
        case UI_RES_1280: return run ? &img_ctl_bd_run_1280 : &img_ctl_bd_collect_1280;
        default:          return run ? &img_ctl_bd_run_1024 : &img_ctl_bd_collect_1024;
    }
}

/* Purify：触发打印结果页清粉测试（后端走 clean_test_start/close + TLV 0x22）。
 * 一次打印结果仅允许成功清粉一次。 */
void on_result_purify_click(lv_event_t * /*e*/)
{
    if(g_result_cleaning || g_result_purify_done) {
        return;
    }
    g_result_cleaning = true;
    /* PRINT_RESULT 这一档带清粉录像，与控制页的手动清粉（不录像）区分开 */
    ui_clean_state_request(UI_CLEAN_OWNER_PRINT_RESULT, g_result_job_name);
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

/* 滑动取件：退出打印状态并走开门流程（与首页「SLIDE TO UNLOCK」同一套弹窗）。 */
void on_result_collect_unlocked(void * /*u*/)
{
    if(g_result_cleaning || g_purify_stage == PURIFY_STAGE_RUNNING) {
        ui_toast_show("Air purifying, please wait", user_color_accent, 1200);
        return;
    }
    control_print_result_collect();
}

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

    /* 稿 4972:27560：数值 16、单位 10、两者间距 4。Montserrat 没有 10 号（见
     * rongsu_ui.defaults 打开的字号表），单位取最近的 caption_sm（1280 档 12）。
     * 间距那项 USER_PAD_SM/4 在 1280 档正好是 4，本来就对。 */
    lv_obj_t *val = lv_label_create(vrow);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_text_sm(), 0);
    if(out_val) *out_val = val;

    if(unit) {
        lv_obj_t *u = lv_label_create(vrow);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_color(u, user_color_text, 0);
        lv_obj_set_style_text_opa(u, LV_OPA_50, 0);
        lv_obj_set_style_text_font(u, user_font_caption_sm(), 0);
    }
    return vrow;
}

/** 环境条列骨架：上下两行居中（上为文字 caption，下为数值行）。
 *  列宽走 USER_ENV_COL_W（稿 64@1280）；该字段为 0 的档位装不下稿的等比列宽，退回
 *  按内容自适应，判据见 ui_theme.hpp 那个字段的注释。 */
lv_obj_t *result_env_col_base(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, USER_ENV_COL_W > 0 ? USER_ENV_COL_W : LV_SIZE_CONTENT);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 4, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

/** 文字列（稿 4231:5749 那条小条里每列都是这种）：一行文字 caption + 一行数值。
 *  unit 为 nullptr 时数值行不带单位——现存四列都带单位，这条分支留给以后新增的
 *  分级词那类无单位列（原先的舱内 PM2.5 列走的就是它，已删）。 */
lv_obj_t *result_env_text_col(lv_obj_t *parent, const char *caption,
                              const char *unit, lv_obj_t **out_val)
{
    lv_obj_t *col = result_env_col_base(parent);
    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text, 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_50, 0);
    /* 稿这行是 14，但工程用 Montserrat：`Humidity` @14 要 66.6px、装不进稿的 64 定宽，
     * 降一档到 caption_sm（1280 档 12）后是 57.1，四列最宽也只有 62.1。 */
    lv_obj_set_style_text_font(cap, user_font_caption_sm(), 0);
    result_env_value_row(col, unit, out_val);
    return col;
}

void result_pm_card_update(void);   /* 定义见下方数据刷新区 */

/* 结果页 HOME PM2.5 卡：点卡片本身切换 PM2.5/PM10。
 * 与首页左卡共用 g_home_metric_idx，两处一起刷新以免回到首页时不一致。 */
void on_result_pm_card_click(lv_event_t * /*e*/)
{
    g_home_metric_idx = (g_home_metric_idx + 1) % 2;
    home_card_update();
    result_pm_card_update();
}

/** 顶部左侧 HOME PM2.5 卡（稿 4972:27560 里的 5275:10870）：标题行 + 分级词 + 括号数值。
 *
 *  **宽度仍用内容自适应，不取稿的 157 定宽**：最坏文案 `Very Poor (150.0 ug/m3)` 按稿字号
 *  （分级词 16 + 括注 12、间距 4）实测要 165.1px，而 157 扣掉两侧 16 的内边距只剩 125，
 *  差 40px——降字号救不回来。根因是稿上那行是短文案 `Fair (25μg/m³)`，没考虑最长分级词
 *  加三位小数的读数。同一类取舍见 build_result_env_bar。
 *
 *  eb 收本卡的三个 label 句柄——打印态与监控态各建一份，不能写同一组全局。 */
lv_obj_t *build_result_pm_card(lv_obj_t *parent, env_block_t *eb)
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

    /* 标题行：标题 + 24 的切换图标。稿上这行是 space-between、图标贴卡片右缘，但本卡
     * 是内容自适应宽（见函数头），撑不出可供 space-between 分配的余量，所以退成「紧挨
     * 排列 + 一个间距」。图标只是可点性的提示，整张卡本身才是点击目标。 */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

    eb->pm_title = lv_label_create(head);
    lv_label_set_text(eb->pm_title, kAirMetrics[0].title);
    lv_obj_set_style_text_color(eb->pm_title, user_color_text, 0);
    lv_obj_set_style_text_opa(eb->pm_title, LV_OPA_50, 0);
    lv_obj_set_style_text_font(eb->pm_title, user_font_caption(), 0);

    /* 稿 4972:27108 这颗是 24 见方，正好是首页那颗（32）的 3/4，三档整除；
     * 这三档（36/24/19）在 ic_switch_set 里也各有一份原生位图 */
    make_dim_icon(head, ic_switch_set, USER_METRIC_ICON_SZ * 3 / 4);

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

    /* 稿：分级词 16、括注 10（Montserrat 无 10 号，取 caption_sm）。 */
    eb->pm_level = lv_label_create(lrow);
    lv_label_set_text(eb->pm_level, "--");
    lv_obj_set_style_text_color(eb->pm_level, user_color_text, 0);
    lv_obj_set_style_text_font(eb->pm_level, user_font_text_sm(), 0);

    eb->pm_value = lv_label_create(lrow);
    lv_label_set_text(eb->pm_value, "");
    lv_obj_set_style_text_color(eb->pm_value, user_color_text, 0);
    lv_obj_set_style_text_opa(eb->pm_value, LV_OPA_50, 0);
    lv_obj_set_style_text_font(eb->pm_value, user_font_caption_sm(), 0);
    return card;
}

/** 顶部环境条：Temp./O2/Humidity/Pressure 四列，caption 都是文字。
 *
 *  以新版稿 4972:27560（里的 5275:10879）为准，**那一版本身就是四列**——舱内 PM2.5 那列
 *  是设计在这一版去掉的，与首页右侧 CABIN 面板早先按稿 5277:11590 删掉同名那行是同一个
 *  动作。旧版 frame（4231:5749 / 4972:31258）还是五列，对稿时注意别照旧版加回来。
 *
 *  几何全取新版稿：内边距 8、列间距 12、每列定宽 USER_ENV_COL_W（64@1280），
 *  整条宽度因此落在稿的 308。列宽在装不下的档位退回自适应，见那个字段的注释。 */
lv_obj_t *build_result_env_bar(lv_obj_t *parent, env_block_t *eb)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 内边距稿 8 = USER_PAD_SM/2；列间距稿 12 = USER_PAD_SM*3/4（1280 档正好整除）。 */
    lv_obj_set_style_pad_all(panel, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(panel, USER_PAD_SM * 3 / 4, 0);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_30, 0);
    lv_obj_set_style_radius(panel, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    result_env_text_col(panel, "Temp.",    "\xC2\xB0""C", &eb->temp_val);
    result_env_text_col(panel, "O2",       "%",           &eb->o2_val);
    result_env_text_col(panel, "Humidity", "%",           &eb->hum_val);
    result_env_text_col(panel, "Pressure", "kpa",         &eb->prs_val);
    return panel;
}

/** 右上环境区整体（PM2.5 卡 + 5 列条），贴 parent 右上角。
 *  打印态浮层与待机监控层各调一次，各传自己那份 env_block_t。 */
lv_obj_t *build_env_wrap(lv_obj_t *parent, env_block_t *eb)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(wrap, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    /* 稿 4972:31316 里这一块在 x=699 y=32，右缘落在 1248 —— 即距屏右和距屏顶都是 32，
     * 与右侧中部的拍照钮、底部两组控件同一个退让量（USER_PAD_LG）。 */
    lv_obj_align(wrap, LV_ALIGN_TOP_RIGHT, -USER_PAD_LG, USER_PAD_LG);

    /* 通知浮层也贴右上角，靠 sync_notice_offset 按本块的高度往下让，故要存句柄 */
    eb->root = wrap;

    build_result_pm_card(wrap, eb);
    build_result_env_bar(wrap, eb);
    return wrap;
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

    /* caption 用比 caption 再小一档的字号：CONSUMABLES 有 11 个字母，卡片内两列
     * 均分后按 user_font_caption() 排会溢出被裁（1024 档尤其明显）。 */
    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption_sm(), 0);
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
    g_pc_thumb_img = lv_image_create(thumb);
    lv_image_set_src(g_pc_thumb_img, nullptr);
    lv_obj_set_size(g_pc_thumb_img, LV_PCT(100), LV_PCT(100));
    lv_obj_align(g_pc_thumb_img, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_inner_align(g_pc_thumb_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(g_pc_thumb_img, LV_OBJ_FLAG_HIDDEN);

    g_pc_thumb_icon = lv_label_create(thumb);
    lv_label_set_text(g_pc_thumb_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(g_pc_thumb_icon, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_pc_thumb_icon, user_font_value(), 0);
    lv_obj_center(g_pc_thumb_icon);

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

    /* 层号紧贴百分比左侧，摆在徽标行而不是下面新开一列：右列 1280 档只有 268 宽，
     * 两列均分后 CONSUMABLES 已经要靠 caption_sm 才不被裁，再插第三列每列剩不到
     * 90，那个词必溢出。徽标行有 spacer 撑出的空档，放得下 "999/999"。
     * 用 dim 色 + 小一档字号压住层级，别跟同色的状态词/百分比抢眼。 */
    g_pc_layer = lv_label_create(head);
    lv_label_set_text(g_pc_layer, "");
    lv_obj_set_style_text_color(g_pc_layer, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_pc_layer, user_font_caption_sm(), 0);
    lv_obj_set_style_text_letter_space(g_pc_layer, 1, 0);
    lv_obj_add_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);

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

/** 运行组（稿 4231:5716）：暂停/恢复钮 + 终止钮，各坐在一颗玻璃 blob 上，
 *  底衬是稿 4796:6172 那条把两颗连起来的 Union 形状，出图见 make_ctl_backdrop。
 *  这里的 blob/gap 必须和 gen_ctl_backdrop.py 的 PROFILES 算法一致，否则对不齐。 */
lv_obj_t *build_print_controls_run(lv_obj_t *parent)
{
    const int32_t btn  = USER_PRINT_CTL_BTN;
    const int32_t blob = ctl_blob(btn);
    const int32_t gap  = ctl_gap(btn);
    const int32_t pad  = (blob - btn) / 2;
    const int32_t w    = blob * 2 + gap;

    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, w, blob);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    make_ctl_backdrop(grp, ctl_backdrop_dsc(true));

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
 *  底衬同样出自设计稿的 Union 形状（见 make_ctl_backdrop）。胶囊本体交给
 *  ui_slide_unlock——它的红 knob + 提示文字 + 三箭头正是稿上那半条，只是轨道底
 *  改由底衬来画。blob/gap/pill 必须和 gen_ctl_backdrop.py 的 PROFILES 一致。 */
lv_obj_t *build_print_controls_collect(lv_obj_t *parent)
{
    const int32_t btn  = USER_PRINT_CTL_BTN;
    const int32_t blob = ctl_blob(btn);
    const int32_t gap  = ctl_gap(btn);
    const int32_t pad  = (blob - btn) / 2;
    const int32_t pill = USER_PRINT_PILL_W;
    const int32_t w    = blob + gap + pill;

    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, w, blob);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    make_ctl_backdrop(grp, ctl_backdrop_dsc(false));

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
    /* 圆头直接给 btn：稿 4972:32141 那颗红钮在组内 x=96，而滑块本身从 blob+gap=88 起，
     * 即圆头相对轨道内缩 8、直径 64 —— 与左边吹粉钮坐在 blob 上的 pad 是同一个数。
     * 不给这个值的话控件会走「按 track_h 推导」的老规则，推出 72/内缩 4。 */
    cfg.knob_sz          = btn;
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
    make_glass_round_btn(layer, &ic_photo, on_result_photo_click,
                         &g_result_photo_btn);
    lv_obj_add_flag(g_result_photo_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_result_photo_btn, LV_ALIGN_RIGHT_MID, -USER_PAD_LG, 0);

    /* 右上环境区（六态都有） */
    build_env_wrap(layer, &g_env_print);

    /* 左下：stats-card 贴底。它上方那摞通知卡不在本层——那是全局的
     * ui_notice_stack，进出打印态时按本卡高度把它抬起来（见 sync_notice_offset）。
     * layer 已随摄像头层铺到屏幕左边缘，左对齐的东西要补上侧栏宽度才不会钻到
     * 侧栏底下（侧栏是浮层且可点，压住的部分连触摸都收不到）。 */
    const int32_t left_ofs = USER_PAD_LG + USER_SIDEBAR_W;
    g_pc_card = build_print_stats_card(layer);
    lv_obj_add_flag(g_pc_card, LV_OBJ_FLAG_FLOATING);
    /* 稿 4972:31292 是 x=160 y=636：距侧栏 32、底缘 768 即距屏底 32。 */
    lv_obj_align(g_pc_card, LV_ALIGN_BOTTOM_LEFT, left_ofs, -USER_PAD_LG);

    /* 右下：两组控件同一位置互斥显示 */
    g_ctl_run = build_print_controls_run(layer);
    lv_obj_add_flag(g_ctl_run, LV_OBJ_FLAG_FLOATING);
    /* 稿：运行组 4972:31262 在 x=1080 y=688、取件组 4972:32136 在 x=760 y=688，
     * 两者右缘都是 1248、底缘都是 768 —— 距屏右/屏底各 32。 */
    lv_obj_align(g_ctl_run, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_LG, -USER_PAD_LG);

    g_ctl_collect = build_print_controls_collect(layer);
    lv_obj_add_flag(g_ctl_collect, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_ctl_collect, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_LG, -USER_PAD_LG);

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
    case PRINT_PHASE_OPTICAL_CHECK:
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
    const BadgeSpec kBadges[5] = {
        { &ic_badge_spin,  "PRINTING..."         },
        { &ic_badge_play,  "PAUSE"               },
        { &ic_badge_alert, "PRINT TERMINATED"    },
        { &ic_badge_check, "PRINT COMPLETE"      },
        { &ic_badge_spin,  "OPTICAL PATH CHECK"  },
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

    /* 光路检测阶段：暂停钮灰掉（运动包还没开始，暂停没有意义）；终止钮保持可点。 */
    if(g_btn_pause) {
        const bool pause_ok = (g_print_phase != PRINT_PHASE_OPTICAL_CHECK);
        if(pause_ok) {
            lv_obj_add_flag(g_btn_pause, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(g_btn_pause, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_flag(g_btn_pause, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_state(g_btn_pause, LV_STATE_DISABLED);
        }
    }

    /* 两条提示只在结束态出现（稿上打印中/暂停两张都没有）：吹粉前催吹粉，
     * 吹粉完成后无论 PM 高低都提醒戴口罩。它们走的是全局消息中心而不是本层
     * 自己的一条 bar——这样离开首页也看得到，且与后端上报的告警排在同一摞里。
     * push 是幂等的，本函数被反复调用不会把卡片刷来刷去。 */
    const bool masked = finished && (g_purify_stage == PURIFY_STAGE_DONE);
    const bool nag_purify = finished && !masked;
    if(g_printing_active && masked) {
        ui_message_push((uint16_t)middleware::DeviceEventCode::UiWearMask, 0);
    } else {
        ui_message_retract((uint16_t)middleware::DeviceEventCode::UiWearMask);
    }
    if(g_printing_active && nag_purify) {
        ui_message_push((uint16_t)middleware::DeviceEventCode::UiPurifyBeforeOpen, 0);
    } else {
        ui_message_retract((uint16_t)middleware::DeviceEventCode::UiPurifyBeforeOpen);
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

/** 通知浮层默认贴屏右上角，首页打印态与监控态的右上角另有常驻环境区（HOME PM2.5
 *  卡 + 5 列环境条），得把浮层压到它下面去。待机态右上角是空的，给 0。 */
void sync_notice_offset(void)
{
    const env_block_t *eb = nullptr;
    if(g_printing_active)     eb = &g_env_print;
    else if(g_monitor_active) eb = &g_env_monitor;

    int32_t ofs = 0;
    if(eb && eb->root) {
        /* 环境区高是 LV_SIZE_CONTENT 算出来的，取值前先逼一次布局，
         * 否则首次进打印态拿到 0，整栈会压在环境区上。 */
        lv_obj_update_layout(eb->root);
        ofs = lv_obj_get_height(eb->root) + USER_PAD_SM / 2;
    }
    ui_notice_stack_set_top_offset(ofs);
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
 *  待机"查看监控"（稿 3122:13224）：全屏摄像头 + 右上环境区 + 左下返回钮
 *  + 右下滑动开门 + 右侧中部拍照钮
 *
 *  右上那两块与右侧拍照钮都与打印态浮层共用同一份构造函数，几何和配色因此天然
 *  一致；两层同时存在于对象树里，所以环境区的句柄各存一份 env_block_t。
 * ============================================================================ */

void on_monitor_exit_click(lv_event_t * /*e*/)
{
    exit_monitor_mode();
}

/* 左下返回钮（稿 4232:6233）：与拍照钮同一枚玻璃圆钮，图标换成 home。
 * 旧版是「< EXIT」文字胶囊，功能不变，只是外观按稿改成圆钮。 */
lv_obj_t *build_monitor_exit_button(lv_obj_t *parent)
{
    lv_obj_t *btn = nullptr;
    /* 钮内图标边长是 USER_PRINT_CTL_BTN/2（48/32/25），从 home 的多档表里取最近的
     * 那份——1920/1280 精确命中，1024 档是 26 缩到 25。 */
    make_glass_round_btn(parent, pick_icon(ic_home_set, USER_PRINT_CTL_BTN / 2),
                         on_monitor_exit_click, &btn);
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

    /* 右上：环境区（与打印态同一套构造） */
    build_env_wrap(area, &g_env_monitor);

    /* 左下：返回钮（补上侧栏宽度，避免被浮层侧栏压住而点不到） */
    lv_obj_t *exit_btn = build_monitor_exit_button(area);
    lv_obj_add_flag(exit_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(exit_btn, LV_ALIGN_BOTTOM_LEFT, USER_PAD_LG + USER_SIDEBAR_W,
                 -USER_PAD_LG);

    /* 右下：滑动开门（与首页同尺寸/配色） */
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text     = "SLIDE TO UNLOCK";
    cfg.on_unlocked   = on_monitor_door_unlocked;
    cfg.user_data     = nullptr;
    cfg.track_w       = USER_DOOR_SLIDE_W;
    cfg.track_h       = USER_DOOR_SLIDE_H;
    cfg.knob_sz       = USER_DOOR_SLIDE_KNOB;
    cfg.custom_accent = true;
    cfg.accent_color  = user_color_status_err;
    g_monitor_door = ui_slide_unlock_create(area, &cfg);
    lv_obj_t *door_root = ui_slide_unlock_get_root(g_monitor_door);
    if(door_root) {
        lv_obj_add_flag(door_root, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(door_root, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_LG, -USER_PAD_LG);
    }

    /* 右侧中部拍照钮：与打印态同位同款（稿 3847:9438） */
    make_glass_round_btn(area, &ic_photo, on_monitor_photo_click,
                         &g_monitor_photo_btn);
    lv_obj_add_flag(g_monitor_photo_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_monitor_photo_btn, LV_ALIGN_RIGHT_MID, -USER_PAD_LG, 0);

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

    if(g_normal_area)   lv_obj_add_flag(g_normal_area,  LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)    lv_obj_add_flag(g_door_float,   LV_OBJ_FLAG_HIDDEN);
    if(g_action_tiles)  lv_obj_add_flag(g_action_tiles, LV_OBJ_FLAG_HIDDEN);
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

    if(g_monitor_area)  lv_obj_add_flag(g_monitor_area,    LV_OBJ_FLAG_HIDDEN);
    if(g_normal_area)   lv_obj_clear_flag(g_normal_area,   LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)    lv_obj_clear_flag(g_door_float,    LV_OBJ_FLAG_HIDDEN);
    if(g_action_tiles)  lv_obj_clear_flag(g_action_tiles,  LV_OBJ_FLAG_HIDDEN);

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
/* 刷新一份 HOME PM2.5 卡。打印态与监控态各有一份，两份内容始终一致
 * （共用 g_home_metric_idx），所以只是把同样的文案写两遍。 */
void result_pm_card_update(void)
{
    const AirMetric &m = kAirMetrics[g_home_metric_idx];
    const ui_device_snapshot_t &snap = *ui_device_state_get();
    const float v = (g_home_metric_idx == 0) ? snap.pm25_indoor
                                             : snap.pm10_indoor;
    const bool  bad   = metric_invalid(v);
    const int   level = bad ? 0 : air_classify(m, v, nullptr);

    for(env_block_t *eb : kEnvBlocks) {
        if(!eb->pm_title) {
            continue;   /* 该层还没建起来 */
        }
        lv_label_set_text(eb->pm_title, m.title);
        if(bad) {
            lv_label_set_text(eb->pm_level, "--");
            lv_obj_set_style_text_color(eb->pm_level, user_color_text_dim, 0);
            lv_label_set_text(eb->pm_value, "");
        } else {
            lv_label_set_text(eb->pm_level, kLevelName[level]);
            lv_obj_set_style_text_color(eb->pm_level, user_color_text, 0);
            lv_label_set_text_fmt(eb->pm_value, "(%.1f ug/m3)", v);
        }
    }
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
    apply_camera_preview_layout_all(src_w, src_h);
}

lv_obj_t *page_user_home_print_preview_parent(void)
{
    if(!g_print_area) {
        return nullptr;
    }
    return g_print_area;
}

void page_user_home_refresh(bool /*force*/)
{
    /* 设备名/WiFi/状态点那一组不在本页——外壳的常驻条自己观察同一份快照。 */
    const ui_device_snapshot_t &snap = *ui_device_state_get();

    /* 换页时 PageManager 会把通知栈的让位量清零，回到本页要按当前态再让一次。
     * 本函数就是首页的 on_enter，进出打印态/监控态也都会走到这儿。 */
    sync_notice_offset();

    /* 首页左卡（HOME 空气质量·可切换 PM2.5/PM10：分级 + 四档色条 + 游标） */
    home_card_update();

    /* 首页右侧 Cabin condition：稿 5277:11590 起舱内 PM2.5 那行已去掉，四行都按哨兵
     * 判空。舱内 PM2.5 仍在打印态/监控态那条 5 列环境小条里保留一列（见下方 kEnvBlocks
     * 循环），按档位着色也只用在那条上。 */
    update_cabin_metric(g_cab_temp_val, g_cab_temp_unit, snap.temperature_c);
    update_cabin_metric(g_cab_o2_val,   g_cab_o2_unit,   snap.oxygen_percent);
    update_cabin_metric(g_cab_hum_val,  g_cab_hum_unit,  snap.humidity_percent);
    update_cabin_metric(g_cab_prs_val,  g_cab_prs_unit,  snap.pressure_kpa);

    /* 右上环境区：打印态浮层与待机监控层各一份，两份内容相同，一起刷。
     * 舱内 PM2.5 那一列已删（见 build_result_env_bar），所以 snap.pm25_cabin 在 UI 侧
     * 不再有消费点——字段仍留在 ui_device_snapshot_t 里，后端那边还在用（Glavo 的清粉
     * 阈值判定、MQTT 遥测）。 */
    result_pm_card_update();
    for(env_block_t *eb : kEnvBlocks) {
        update_cabin_metric(eb->temp_val, nullptr, snap.temperature_c);
        update_cabin_metric(eb->o2_val,   nullptr, snap.oxygen_percent);
        update_cabin_metric(eb->hum_val,  nullptr, snap.humidity_percent);
        update_cabin_metric(eb->prs_val,  nullptr, snap.pressure_kpa);
    }
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
    /* 纵向抵消的是 content_panel_ 的 pad_top，那一格现在是 USER_CONTENT_TOP
     * 而非设备条高（两者已拆开给值），用错会让整页竖直错开二者之差。 */
    lv_obj_set_pos(page, -USER_SIDEBAR_W, -USER_CONTENT_TOP);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_left(page, USER_SIDEBAR_W, 0);
    lv_obj_set_style_pad_row(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page_root = page;

    /* 先建首页主区（全屏整机背景浮层，置于最底），再建顶部占位带——后建的浮于背景之上。 */
    g_normal_area = build_main_area(page);
    g_topbar = build_topbar(page);

    /* 打印中模式：摄像头 + 双底条；与 g_normal_area 互斥显示。 */
    g_print_area = build_printing_area(page);
    lv_obj_add_flag(g_print_area, LV_OBJ_FLAG_HIDDEN);

    /* 待机查看监控层：摄像头全屏 + 右上环境面板 + 左下 EXIT + 右下开门；默认隐藏。 */
    g_monitor_area = build_monitor_area(page);
    lv_obj_add_flag(g_monitor_area, LV_OBJ_FLAG_HIDDEN);

    /* 底部两组浮动控件：相对于 page 绝对定位，左下入口胶囊行 + 右下开门滑块。
     * 建在 print/monitor 两层之后，待机态才压得住它们（那两层默认隐藏，
     * 进态时还会 move_background 再压一次）。 */
    g_door_float   = build_door_floating(page);
    g_action_tiles = build_action_tiles(page);

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

    if(g_normal_area)  lv_obj_add_flag(g_normal_area,  LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)   lv_obj_add_flag(g_door_float,   LV_OBJ_FLAG_HIDDEN);
    if(g_action_tiles) lv_obj_add_flag(g_action_tiles, LV_OBJ_FLAG_HIDDEN);
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
    /* 层号先隐掉：总层数要等第一帧 Progress 才知道，且上一单的层号不能留在这 */
    if(g_pc_layer) {
        lv_label_set_text(g_pc_layer, "");
        lv_obj_add_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);
    }
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
    if(g_pc_thumb_img && g_pc_thumb_icon) {
        const bool show_real_thumb = file && thumb_path_exists(file->thumb_print);
        if(show_real_thumb) {
            lv_image_set_src(g_pc_thumb_img, file->thumb_print_src);
            lv_obj_clear_flag(g_pc_thumb_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_pc_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_image_set_src(g_pc_thumb_img, nullptr);
            lv_obj_add_flag(g_pc_thumb_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(g_pc_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

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

    /* 光路检测借用了层号位显示点位计数（"2/5"）。它跟层号同形不同义，留到结果页
     * 会被读成"第 2 层 / 共 5 层"，所以从检测态直接跳结果页时要清掉。
     * 正常打印中途终止时层号是真数据，保留。 */
    if(g_print_phase == PRINT_PHASE_OPTICAL_CHECK && g_pc_layer) {
        lv_label_set_text(g_pc_layer, "");
        lv_obj_add_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);
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

    if(g_print_area)   lv_obj_add_flag(g_print_area,     LV_OBJ_FLAG_HIDDEN);
    if(g_normal_area)  lv_obj_clear_flag(g_normal_area,  LV_OBJ_FLAG_HIDDEN);
    if(g_door_float)   lv_obj_clear_flag(g_door_float,   LV_OBJ_FLAG_HIDDEN);
    if(g_action_tiles) lv_obj_clear_flag(g_action_tiles, LV_OBJ_FLAG_HIDDEN);
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
    /* 光路检测占用徽标行的层号位（3/5）和 0% 进度条，正式进度来了再让位。 */
    if(g_print_phase == PRINT_PHASE_OPTICAL_CHECK) {
        return;
    }

    g_print_percent = p->progress_percent;
    if(g_pc_bar) {
        lv_bar_set_value(g_pc_bar, p->progress_percent, LV_ANIM_ON);
    }
    if(g_pc_percent) {
        lv_label_set_text_fmt(g_pc_percent, "%d%%", p->progress_percent);
    }
    /* 后端给不出总层数（OVF / 无层标记）时整条隐掉，不留 "--/--" 占位 */
    if(g_pc_layer) {
        if(p->total_layers > 0) {
            lv_label_set_text_fmt(g_pc_layer, "%u/%u",
                                  static_cast<unsigned>(p->current_layer),
                                  static_cast<unsigned>(p->total_layers));
            lv_obj_remove_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_pc_time_val) {
        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), p->remaining_minutes);
        lv_label_set_text(g_pc_time_val, dur);
    }
}

void page_user_home_optical_path_update(uint8_t point_index, uint8_t point_total,
                                       uint8_t state)
{
    if(!g_printing_active || !g_print_overlay || phase_is_finished()) {
        return;
    }

    if(state == static_cast<uint8_t>(middleware::OpticalPathPointState::Complete)) {
        g_print_phase = PRINT_PHASE_PRINTING;
        if(g_pc_layer) {
            lv_label_set_text(g_pc_layer, "");
            lv_obj_add_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);
        }
        apply_print_phase();
        return;
    }

    if(g_print_phase != PRINT_PHASE_OPTICAL_CHECK) {
        g_print_phase = PRINT_PHASE_OPTICAL_CHECK;
        g_print_percent = 0;
        if(g_pc_bar) {
            lv_bar_set_value(g_pc_bar, 0, LV_ANIM_OFF);
        }
        if(g_pc_percent) {
            lv_label_set_text(g_pc_percent, "0%");
        }
        apply_print_phase();
    }

    if(g_pc_layer && point_total > 0) {
        const unsigned shown = static_cast<unsigned>(point_index) + 1u;
        lv_label_set_text_fmt(g_pc_layer, "%u/%u", shown,
                              static_cast<unsigned>(point_total));
        lv_obj_remove_flag(g_pc_layer, LV_OBJ_FLAG_HIDDEN);
    }
}
