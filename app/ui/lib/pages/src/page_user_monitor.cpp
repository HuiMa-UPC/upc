#include "page_user_monitor.hpp"

#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_segmented.hpp"
#include "ui_feedback.hpp"
#include "ui_slide_unlock.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_msg_proxy.hpp"
#include "debug_msg_proxy.hpp"
#include "camera_msg_proxy.hpp"
#include "camera_preview.hpp"
#include "ui_device_state.hpp"
#include "ui_clean_state.hpp"
#include "ui_message_store.hpp"
#include "control.hpp"
#include "home_metric_icons.h"
#include "upc_contract.hpp"
#include "config/camera_resolution_profile.hpp"
#include "media_paths.hpp"

#include <cmath>
#include <cstdio>

/**
 * @file page_user_monitor.cpp
 * @brief 监控页：左摄像头 + 右三 Tab 面板
 *
 * 版式（稿 1280×800 坐标，其余档位按 profile 折算）：
 *
 *   0    128                          860                1280
 *   ┌────┬─────────────────────────────┬───────────────────┐ 0
 *   │    │ ● 吹粉中                    │ [Job][Status][Man]│   ← 32..96
 *   │侧  │                             │ ┌───────────────┐ │
 *   │栏  │        摄像头画面            │ │  模型缩略图    │ │   ← 120..330
 *   │    │                             │ └───────────────┘ │
 *   │(外 │                             │  Laptop stand.stl │
 *   │壳  │                             │  ⟳ Printing   20% │
 *   │画) │                             │  ▬▬▬▬▬▬▬▬▬▬▬     │   ← 432..456
 *   │    │                             │ ┌─────┐ ┌─────┐   │
 *   │    │                             │ │Time │ │Cons.│   │   ← 480..578
 *   │    │ ☀        📷  ⛶             │ └─────┘ └─────┘   │   ← 浮钮 690..762
 *   │    │                             │ [ ⏸ ] [ ⏹ ]      │   ← 684..768
 *   └────┴─────────────────────────────┴───────────────────┘ 800
 *
 * 三处与首页不同的结构性约定，改之前先看：
 *
 * 1. **本页不显示常驻设备条**（`apply_device_bar()` 里按页名排除），所以页根只要跨过
 *    `pad_left` 出血到屏幕左边缘，纵向不用抵消 `pad_top`——那个值此时已经是 0。
 * 2. **摄像头输出几何恒按全屏宽算**，见 `cam_apply_layout()`。
 * 3. **Manual 页用的是 `DebugMsgProxy` 的单槽回调**（axis / device / soc_led），调试页
 *    也在用同一组，所以进页注册、离页清空（`page_user_monitor_exit()`），与
 *    `page_user_debug_exit()` 对称。照明钮控 SOC 六路舱灯，进页 Get mask 对齐。
 */

namespace {

/* ========================================================================== *
 *  常量与几何
 * ========================================================================== */

/* 状态行/进度条的四档语义色，与原首页打印态同源（稿 4156:9739 一族）。 */
constexpr uint32_t kPhaseBlue      = 0x6476ff;   /* 状态词 + 百分比 + 进度条 */
constexpr uint32_t kPhaseBlueFillB = 0x586bff;   /* 进度条填充右端 */
constexpr uint32_t kPhaseOrange    = 0xff7843;
constexpr uint32_t kPhaseOrangeB   = 0xff8454;
constexpr uint32_t kPhaseGreen     = 0x37e47b;
constexpr uint32_t kPhaseGreenB    = 0x5ceb95;

/* 底部按钮两色（稿 6338:27240 的 paint2 / paint4）。 */
constexpr uint32_t kBtnBlue = 0x4054fd;
constexpr uint32_t kBtnRed  = 0xfd4242;

/* 照明钮点亮那档：黄底 + 白灯泡（稿黄 6338:27049；白图标比深墨更好看）。 */
constexpr uint32_t kLightOnBg   = 0xffcd42;
constexpr uint32_t kLightOnIcon = 0xffffff;

/* Manual 的 Plate Position 选中档（稿 6307:40977）。 */
constexpr uint32_t kPlateOn = 0x485dff;

/* Manual 点按步距（mm）。Z 按 μm 下发，可以取小数；S 协议是 int16，只能是整数 mm。 */
constexpr float kManualZStepMm = 0.1f;
constexpr float kManualSStepMm = 1.0f;

/* Manual 长按点动速度（协议 int16，mm/s）。 */
constexpr float kManualZHoldSpeed = 2.0f;
constexpr float kManualSHoldSpeed = 30.0f;

/* Status 页 PM 色条四段（稿 6338:28064）。 */
constexpr uint32_t kBarColors[4] = { 0x55d273, 0xf3cd3a, 0xf5803b, 0xea4948 };

/** 右栏内容区宽（稿 356@1280）。 */
int32_t panel_content_w(void) { return USER_MON_PANEL_W - USER_PAD_LG * 2; }

/** 摄像头层的**全屏**宽。非全屏时视口收窄，但出图几何一直按这个值算。 */
int32_t cam_full_w(void) { return LV_HOR_RES - USER_SIDEBAR_W; }

/** 摄像头浮钮里图标的边长：稿 72 钮配 32 图标，跟 USER_METRIC_ICON_SZ 走。 */
int32_t glass_icon_sz(void) { return USER_METRIC_ICON_SZ; }

/* ========================================================================== *
 *  状态
 * ========================================================================== */

/** 右栏三个 Tab。顺序即稿上从左到右。 */
enum tab_t { TAB_JOB = 0, TAB_STATUS, TAB_MANUAL, TAB_COUNT };

/** 打印阶段五档。与原首页 `print_phase_t` 一一对应，文案按新稿改成了句首大写。 */
enum phase_t {
    PHASE_IDLE = 0,       /**< 没有打印任务：Job 页是空态 */
    PHASE_PREPARING,      /**< Preparing / 起印环境检查步骤文案；蓝 */
    PHASE_PRINTING,       /**< Printing...       蓝 */
    PHASE_PAUSED,         /**< Pause             蓝 */
    PHASE_TERMINATED,     /**< Print Terminated  橙 */
    PHASE_DONE,           /**< Print Complete    绿 */
};

/** 结束态下的吹粉子阶段，决定吹粉钮转不转与状态栏文案。 */
enum purify_stage_t {
    PURIFY_IDLE = 0,
    PURIFY_RUNNING,
    PURIFY_DONE,
};

/** 底部按钮组四档。由「当前 Tab + 当前 phase」共同决定，见 apply_bottom()。 */
enum bottom_mode_t {
    BOTTOM_PRINT = 0,   /**< 整条 PRINT：待机且非 Manual 页 */
    BOTTOM_RUN,         /**< 暂停/恢复 + 终止：打印中 */
    BOTTOM_RETRIEVE,    /**< 吹粉 + RETRIEVE：打印结束 */
    BOTTOM_MANUAL,      /**< 吹粉 + OPEN：Manual 页 */
};

lv_obj_t *g_root        = nullptr;
lv_obj_t *g_cam_layer   = nullptr;  /**< 摄像头视口（随全屏切换改宽，负责裁剪） */
lv_obj_t *g_cam_stage   = nullptr;  /**< 恒定全屏宽的中间层，见 cam_apply_layout */
lv_obj_t *g_cam_image   = nullptr;
lv_obj_t *g_cam_hint    = nullptr;
lv_obj_t *g_chip        = nullptr;  /**< 画面左上的状态胶囊（吹粉中等） */
lv_obj_t *g_chip_label  = nullptr;
lv_obj_t *g_light_btn   = nullptr;
lv_obj_t *g_light_icon  = nullptr;
lv_obj_t *g_snap_btn    = nullptr;
lv_obj_t *g_fs_btn      = nullptr;

lv_obj_t *g_panel       = nullptr;
lv_obj_t *g_tabs        = nullptr;
lv_obj_t *g_view_job    = nullptr;
lv_obj_t *g_view_status = nullptr;
lv_obj_t *g_view_manual = nullptr;

/* Job 页 */
lv_obj_t *g_job_empty      = nullptr;  /**< 「No print job」空态块，与 g_job_card 互斥 */
lv_obj_t *g_job_card       = nullptr;
lv_obj_t *g_job_thumb_img  = nullptr;
lv_obj_t *g_job_thumb_mask = nullptr;  /**< 按进度升高的遮罩，模拟「打到第几层」 */
lv_obj_t *g_job_thumb_icon = nullptr;
lv_obj_t *g_job_name       = nullptr;
lv_obj_t *g_job_badge      = nullptr;  /**< 状态行图标（转圈/三角/勾） */
lv_obj_t *g_job_state      = nullptr;  /**< 状态词 */
lv_obj_t *g_job_extra      = nullptr;  /**< 层号 / 光路检测点位，贴百分比左侧 */
lv_obj_t *g_job_percent    = nullptr;
lv_obj_t *g_job_bar        = nullptr;
lv_obj_t *g_job_divider    = nullptr;  /**< 完成态用它顶掉进度条（稿 6338:27946） */
lv_obj_t *g_job_time_cap   = nullptr;
lv_obj_t *g_job_time_val   = nullptr;
lv_obj_t *g_job_cons_val   = nullptr;
lv_obj_t *g_job_cons_unit  = nullptr;

/* Status 页 */
lv_obj_t *g_pm_title  = nullptr;
lv_obj_t *g_pm_level  = nullptr;
lv_obj_t *g_pm_value  = nullptr;
lv_obj_t *g_pm_dot    = nullptr;   /**< 色条上的游标 */
lv_obj_t *g_pm_track  = nullptr;
lv_obj_t *g_st_temp   = nullptr;
lv_obj_t *g_st_o2     = nullptr;
lv_obj_t *g_st_hum    = nullptr;
lv_obj_t *g_st_prs    = nullptr;
uint8_t   g_pm_metric = 0;         /**< 0 = PM2.5，1 = PM10 */

/* Manual 页 */
lv_obj_t *g_plate_seg   = nullptr;
lv_obj_t *g_plate_val   = nullptr;
lv_obj_t *g_jog_up      = nullptr;
lv_obj_t *g_jog_down    = nullptr;
lv_obj_t *g_s_val       = nullptr;
lv_obj_t *g_door_btn    = nullptr;  /**< Inner Door 电源钮 */
bool      g_inner_door_open = false; /**< env bit1 全开；亮=全开，灭=非全开 */
bool      g_outer_door_open = false; /**< 由 env 快照 / 开门 Rsp 乐观置位同步 */
/** 外门未开时点了 Inner Door：弹窗外门开到位后自动续跑开/关。 */
bool      g_pending_inner_door = false;
/** pending 时打算开还是关（跟点按瞬间的传感器态取反）。 */
bool      g_pending_inner_want_open = true;
/** 内门套件进行中：禁钮，防连点排队第二次开/关。 */
bool      g_inner_door_busy = false;
/** Manual 吹粉：外门未关时记下意图，关到位后再开清粉。 */
bool      g_pending_purify = false;
/** 正在长按点动的轴（DebugAxis 数值），-1 = 没有。 */
int       g_hold_axis = -1;

/* 底部按钮组 */
lv_obj_t *g_bottom      = nullptr;
lv_obj_t *g_btn_print   = nullptr;
lv_obj_t *g_btn_pause   = nullptr;
lv_obj_t *g_pause_bars  = nullptr;
lv_obj_t *g_pause_tri   = nullptr;
lv_obj_t *g_btn_stop    = nullptr;
lv_obj_t *g_btn_purify  = nullptr;
lv_obj_t *g_purify_icon = nullptr;
lv_obj_t *g_btn_wide    = nullptr;   /**< RETRIEVE / OPEN 共用那枚 256 宽的深色钮 */
lv_obj_t *g_btn_wide_lbl = nullptr;

/* 弹窗 */
ui_modal_dialog_t *g_retrieve_dlg  = nullptr;
ui_slide_unlock_t *g_retrieve_slide = nullptr;
ui_modal_dialog_t *g_outer_door_dlg = nullptr;       /**< Inner Door：请先开外门 */
ui_modal_dialog_t *g_purify_door_dlg = nullptr;     /**< Manual 吹粉：请先关外门 */

tab_t          g_tab           = TAB_JOB;
phase_t        g_phase         = PHASE_IDLE;
purify_stage_t g_purify        = PURIFY_IDLE;
int            g_percent       = 0;
bool           g_printing      = false;
bool           g_fullscreen    = false;
bool           g_light_on      = true;  /* 上电六路舱灯默认全开；进页 Get 再对齐 */
bool           g_snap_pending  = false;
bool           g_purify_busy   = false;
bool           g_purify_done   = false;
bool           g_page_visible  = false;
char           g_job_label[96] = {0};
/** PREPARING 态自定义标题；空则回落到默认 "Preparing..."。由 print_prep 进度写入。 */
char           g_prep_title[48] = {0};

void apply_phase(void);
void apply_bottom(void);
void apply_view(void);
void status_update(void);
void cam_apply_layout(uint32_t src_w, uint32_t src_h);
void chip_set(const char *text);
ui_clean_owner_t purify_owner(void);

/* ========================================================================== *
 *  小工具
 * ========================================================================== */

inline bool metric_invalid(float v) { return std::isnan(v) || v < 0.0f; }

bool phase_running(void)
{
    return g_phase == PHASE_PREPARING || g_phase == PHASE_PRINTING ||
           g_phase == PHASE_PAUSED;
}

bool phase_finished(void)
{
    return g_phase == PHASE_TERMINATED || g_phase == PHASE_DONE;
}

lv_color_t phase_accent(void)
{
    switch(g_phase) {
    case PHASE_TERMINATED: return lv_color_hex(kPhaseOrange);
    case PHASE_DONE:       return lv_color_hex(kPhaseGreen);
    default:               return lv_color_hex(kPhaseBlue);
    }
}

/** 白卡：稿上右栏那批卡片统一「白底 + 16 圆角 + 一道很淡的投影」。 */
lv_obj_t *make_card(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, h > 0 ? h : LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, USER_MON_RADIUS, 0);
    lv_obj_set_style_bg_color(c, user_light_card, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    /* 白卡压在 #f5f5f5 上，没有这层影子边界看不出来（同首页四宫格）。 */
    lv_obj_set_style_shadow_width(c, 3, 0);
    lv_obj_set_style_shadow_ofs_y(c, 2, 0);
    lv_obj_set_style_shadow_color(c, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(c, 10, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/** A8 图标 + 着色，画进 sz 见方的盒子里。资源按档位出原生尺寸，上屏 1:1。 */
lv_obj_t *make_icon(lv_obj_t *parent, const lv_image_dsc_t *const *set,
                    int32_t sz, lv_color_t color)
{
    return ui_user_make_icon_img(parent, ui_user_pick_icon(set, sz), sz, color);
}

/** 摄像头上那三颗玻璃浮钮（稿 72 见方、圆角 20、白 15% + 白渐变描边）。 */
lv_obj_t *make_glass_btn(lv_obj_t *parent, const lv_image_dsc_t *const *icon,
                         lv_event_cb_t cb, lv_obj_t **out_icon)
{
    const int32_t sz = USER_LIGHT_BTN_SZ;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, 38, 0);          /* 稿 15% */
    lv_obj_set_style_bg_opa(btn, 70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, sz * 5 / 18, 0); /* 稿 20/72 */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = make_icon(btn, icon, glass_icon_sz(), lv_color_white());
    lv_obj_center(ic);
    if(out_icon) *out_icon = ic;
    return btn;
}

/* ========================================================================== *
 *  摄像头层
 * ========================================================================== */

/**
 * 上屏几何。**宽度恒取 `cam_full_w()`，不跟当前视口走**——这是全屏切换那条口径的
 * 落地点：稿上两态的画面倍率完全一致（渲染尺寸 1149 对 1153），全屏只是把视口从
 * 732 拉到 1152、多露一截横向视野，画面本身不缩放。
 *
 * 结构上靠 `g_cam_stage` 保证：它是 `g_cam_image` 的父节点、宽度恒为全屏宽，而
 * `CameraPreviewPlayer::resolve_output_geometry()` 正是按 `lv_obj_get_parent(image_)`
 * 的尺寸算输出的（见 camera_preview.cpp）。**别把 image 直接挂到 g_cam_layer 上**，
 * 那样非全屏时解码侧会按 732 出图，一切全屏画面就跳一下倍率。
 *
 * 代价是非全屏时多解码/多缩约 36% 的像素（1152 对 732），`decode 5s:` 的 `cvt_avg_us`
 * 会相应涨——这是换「切换不跳画面」付的账，上板要盯一眼。
 */
void cam_apply_layout(uint32_t src_w, uint32_t src_h)
{
    if(!g_cam_image || !g_cam_stage) {
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
    (void)src_h;

    const int32_t w = cam_full_w();
    const int32_t h = LV_VER_RES;
    lv_obj_set_size(g_cam_image, w, h);
    lv_obj_center(g_cam_image);
    lv_image_set_inner_align(g_cam_image, LV_IMAGE_ALIGN_CENTER);
    const int64_t zoom = static_cast<int64_t>(w) * 256 / static_cast<int32_t>(src_w);
    lv_image_set_scale(g_cam_image, zoom > 0 ? static_cast<uint32_t>(zoom) : LV_SCALE_NONE);
    lv_obj_invalidate(g_cam_image);
}

/** 按当前全屏档摆视口与三颗浮钮。摄像头画面本身不动，见 cam_apply_layout。 */
void apply_fullscreen(void)
{
    if(!g_cam_layer) {
        return;
    }
    const int32_t vp_w = g_fullscreen ? cam_full_w()
                                      : cam_full_w() - USER_MON_PANEL_W;
    lv_obj_set_size(g_cam_layer, vp_w, LV_VER_RES);
    if(g_panel) {
        if(g_fullscreen) lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_remove_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    }

    /* 三颗浮钮都贴视口自身的边，所以换档后只要重对齐一次。
     * **退让量照稿原样、不归一**：三颗的底距都是 38（稿上 y 一律 690），左边照明钮的
     * 左距是 32，而右边两颗的右距非全屏 38、全屏 48——稿上就是这几个不整齐的数。 */
    const int32_t inset_b  = USER_PAD_LG + USER_PAD_SM * 3 / 8;   /* 稿 38 */
    const int32_t inset_r  = g_fullscreen ? (USER_PAD_LG + USER_PAD_SM)  /* 全屏稿 48 */
                                          : inset_b;
    const int32_t gap = USER_PAD_SM;                              /* 两颗之间稿 16 */
    if(g_light_btn) {
        lv_obj_align(g_light_btn, LV_ALIGN_BOTTOM_LEFT, USER_PAD_LG, -inset_b);
    }
    if(g_fs_btn) {
        lv_obj_align(g_fs_btn, LV_ALIGN_BOTTOM_RIGHT, -inset_r, -inset_b);
    }
    if(g_snap_btn) {
        lv_obj_align(g_snap_btn, LV_ALIGN_BOTTOM_RIGHT,
                     -(inset_r + USER_LIGHT_BTN_SZ + gap), -inset_b);
    }
    cam_apply_layout(UPC_CAMERA_STREAM_WIDTH, 0u);
}

void on_fullscreen_click(lv_event_t * /*e*/)
{
    g_fullscreen = !g_fullscreen;
    apply_fullscreen();
}

/** 按六路舱灯 mask 刷照明钮：任一亮=开，全关=关（与调试页 Cabin LED 同源）。 */
void apply_light_btn_from_mask(uint8_t mask)
{
    const bool on = (mask != 0);
    g_light_on = on;
    if(g_light_icon) {
        lv_obj_t *img = lv_obj_get_child(g_light_icon, 0);
        if(img) {
            /* 开：白灯泡压在黄底上；关：白图标压在半透明白底上。 */
            lv_obj_set_style_image_recolor(
                img, on ? lv_color_hex(kLightOnIcon) : lv_color_white(), 0);
        }
    }
    if(g_light_btn) {
        lv_obj_set_style_bg_color(g_light_btn,
                                  on ? lv_color_hex(kLightOnBg) : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(g_light_btn, on ? LV_OPA_COVER : 38, 0);
    }
}

/** 打印会话中（Prepare / 打印 / 暂停 / 结果页）照明钮只读，不可切换。 */
void apply_light_btn_interactive(void)
{
    if(!g_light_btn) {
        return;
    }
    if(g_printing) {
        lv_obj_remove_flag(g_light_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(g_light_btn, LV_OPA_60, 0);
    } else {
        lv_obj_add_flag(g_light_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(g_light_btn, LV_OPA_COVER, 0);
    }
}

void on_light_click(lv_event_t * /*e*/)
{
    if(g_printing) {
        return; /* Prepare / 打印中不允许切舱灯 */
    }
    /* 舱内灯已改 SOC sysfs（调试页六路 lgt*），不再下发 MCU Device/Light */
    const bool on = !g_light_on;
    auto &proxy = ui::DebugMsgProxy::instance();
    for(uint8_t i = 0; i < middleware::kDebugSocLedCount; ++i) {
        proxy.send_soc_led_set(i, on);
    }
    apply_light_btn_from_mask(on ? static_cast<uint8_t>(0x3Fu) : 0u);
}

void on_snapshot_click(lv_event_t * /*e*/)
{
    if(g_snap_pending) {
        return;
    }
    g_snap_pending = true;
    if(g_snap_btn) {
        lv_obj_add_state(g_snap_btn, LV_STATE_DISABLED);
    }
    ui::CameraMsgProxy::instance().send_snapshot_req();
    ui_toast_show("Capturing photo...", user_color_accent, 1200);
}

/** 开流。已出帧则只 reattach；半死/失败则 Stop→Start（见 ensure_running）。 */
void cam_ensure_running(void)
{
    auto& player = ui::CameraPreviewPlayer::instance();
    player.ensure_running(g_cam_image, g_cam_hint, "monitor");
    cam_apply_layout(UPC_CAMERA_STREAM_WIDTH, 0u);
}

/** 画面左上那枚状态胶囊（稿 6339:30346 的「吹粉中」）。text=nullptr 收起。 */
void chip_set(const char *text)
{
    if(!g_chip) {
        return;
    }
    if(!text) {
        lv_obj_add_flag(g_chip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(g_chip_label, text);
    lv_obj_remove_flag(g_chip, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *build_cam_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(layer, USER_SIDEBAR_W, 0);
    lv_obj_set_size(layer, cam_full_w() - USER_MON_PANEL_W, LV_VER_RES);
    lv_obj_set_style_bg_color(layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    /* 恒定全屏宽的中间层：解码侧按它的尺寸出图，视口只负责裁。见 cam_apply_layout。 */
    g_cam_stage = lv_obj_create(layer);
    lv_obj_remove_style_all(g_cam_stage);
    lv_obj_set_size(g_cam_stage, cam_full_w(), LV_VER_RES);
    lv_obj_center(g_cam_stage);
    lv_obj_set_style_bg_opa(g_cam_stage, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_cam_stage, LV_OBJ_FLAG_SCROLLABLE);

    g_cam_hint = lv_label_create(g_cam_stage);
    lv_label_set_text(g_cam_hint,
                      LV_SYMBOL_VIDEO "  Camera not connected\n(live cabin feed will appear here)");
    lv_obj_set_style_text_color(g_cam_hint, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_cam_hint, user_font_text(), 0);
    lv_obj_set_style_text_align(g_cam_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(g_cam_hint);

    g_cam_image = lv_image_create(g_cam_stage);
    lv_obj_add_flag(g_cam_image, LV_OBJ_FLAG_HIDDEN);

    /* 状态胶囊：白 70% + 一颗主色圆点（稿 (160,32) 102×46 rx12）。 */
    g_chip = lv_obj_create(layer);
    lv_obj_remove_style_all(g_chip);
    lv_obj_add_flag(g_chip, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(g_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_chip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_chip, 179, 0);        /* 稿 70% */
    lv_obj_set_style_radius(g_chip, USER_MON_RADIUS * 3 / 4, 0);  /* 稿 12 */
    lv_obj_set_style_pad_hor(g_chip, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(g_chip, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(g_chip, USER_PAD_SM / 2, 0);
    lv_obj_set_flex_flow(g_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_chip, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g_chip, LV_ALIGN_TOP_LEFT, USER_PAD_LG, USER_PAD_LG);
    lv_obj_add_flag(g_chip, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dot = lv_obj_create(g_chip);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, USER_STATUS_DOT_SZ * 2 / 3, USER_STATUS_DOT_SZ * 2 / 3);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kPlateOn), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    g_chip_label = lv_label_create(g_chip);
    lv_label_set_text(g_chip_label, "");
    lv_obj_set_style_text_color(g_chip_label, user_light_text, 0);
    lv_obj_set_style_text_font(g_chip_label, user_font_text_sm(), 0);

    g_light_btn = make_glass_btn(layer, ic_light_set, on_light_click, &g_light_icon);
    /* 上电默认全开：建钮后先按开态画，进页 Get 再校正 */
    apply_light_btn_from_mask(static_cast<uint8_t>(0x3Fu));
    g_snap_btn  = make_glass_btn(layer, ic_snapshot_set, on_snapshot_click, nullptr);
    g_fs_btn    = make_glass_btn(layer, ic_fullscreen_set, on_fullscreen_click, nullptr);

    return layer;
}

/* ========================================================================== *
 *  Job 页
 * ========================================================================== */

/** 缩略图遮罩：按进度从下往上「填」出已打印的部分（稿 6338:27400 的半填充模型）。
 *
 *  **不是真的分层渲染**：后端还没有切片预览图，Job 卡里放的是文件列表那张静态
 *  缩略图（`control_file_t::thumb_print`），这里再压一层齐色遮罩，高度 =
 *  (100 − 进度)%，从顶往下盖住「还没打到」的部分。形似而已，够表达进度。 */
void thumb_progress_apply(int percent)
{
    if(!g_job_thumb_mask) {
        return;
    }
    if(percent < 0)   percent = 0;
    if(percent > 100) percent = 100;
    lv_obj_set_height(g_job_thumb_mask, lv_pct(100 - percent));
    if(percent >= 100) lv_obj_add_flag(g_job_thumb_mask, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_remove_flag(g_job_thumb_mask, LV_OBJ_FLAG_HIDDEN);
}

/** Time used / Total Cons. 两张白卡里的一张。 */
lv_obj_t *build_stat_card(lv_obj_t *parent, const char *caption,
                          lv_obj_t **out_cap, lv_obj_t **out_val,
                          const char *unit, lv_obj_t **out_unit)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_height(c, USER_MON_STAT_H);
    lv_obj_set_style_radius(c, USER_MON_RADIUS, 0);
    lv_obj_set_style_bg_color(c, user_light_card, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(c, 3, 0);
    lv_obj_set_style_shadow_ofs_y(c, 2, 0);
    lv_obj_set_style_shadow_color(c, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(c, 10, 0);
    lv_obj_set_style_pad_all(c, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(c);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x8c8c87), 0);   /* 稿值 */
    lv_obj_set_style_text_font(cap, user_font_text_sm(), 0);
    if(out_cap) *out_cap = cap;

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, USER_PAD_SM / 4, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_light_text, 0);
    lv_obj_set_style_text_font(val, user_font_title(), 0);
    if(out_val) *out_val = val;

    if(unit) {
        lv_obj_t *u = lv_label_create(row);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_color(u, user_light_text, 0);
        lv_obj_set_style_text_opa(u, LV_OPA_50, 0);
        lv_obj_set_style_text_font(u, user_font_caption(), 0);
        if(out_unit) *out_unit = u;
    }
    return c;
}

lv_obj_t *build_view_job(lv_obj_t *parent)
{
    lv_obj_t *v = lv_obj_create(parent);
    lv_obj_remove_style_all(v);
    lv_obj_set_width(v, LV_PCT(100));
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(v, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 空态（稿 6338:27049）：整块垂直居中的两行字 ---- */
    g_job_empty = lv_obj_create(v);
    lv_obj_remove_style_all(g_job_empty);
    lv_obj_set_width(g_job_empty, LV_PCT(100));
    lv_obj_set_flex_grow(g_job_empty, 1);
    lv_obj_set_flex_flow(g_job_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_job_empty, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_job_empty, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(g_job_empty, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_job_empty, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *e1 = lv_label_create(g_job_empty);
    lv_label_set_text(e1, "No print job");
    lv_obj_set_style_text_color(e1, user_light_text, 0);
    lv_obj_set_style_text_font(e1, user_font_title(), 0);

    lv_obj_t *e2 = lv_label_create(g_job_empty);
    lv_label_set_text(e2, "Add a model to get started");
    lv_obj_set_style_text_color(e2, user_light_text, 0);
    lv_obj_set_style_text_opa(e2, LV_OPA_50, 0);
    lv_obj_set_style_text_font(e2, user_font_text_sm(), 0);

    /* ---- 有任务：缩略图 + 文件名 + 状态行 + 进度条 + 两张数据卡 ---- */
    g_job_card = lv_obj_create(v);
    lv_obj_remove_style_all(g_job_card);
    lv_obj_set_width(g_job_card, LV_PCT(100));
    lv_obj_set_height(g_job_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_job_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_job_card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g_job_card, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(g_job_card, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_job_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *thumb = lv_obj_create(g_job_card);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, LV_PCT(100), USER_MON_THUMB_H);
    lv_obj_set_style_radius(thumb, USER_MON_RADIUS, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);
    lv_obj_set_style_bg_color(thumb, user_light_text, 0);
    lv_obj_set_style_bg_opa(thumb, 38, 0);   /* 稿 #333145 @15% */
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);

    g_job_thumb_img = lv_image_create(thumb);
    lv_image_set_src(g_job_thumb_img, nullptr);
    lv_obj_set_size(g_job_thumb_img, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(g_job_thumb_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(g_job_thumb_img);
    lv_obj_add_flag(g_job_thumb_img, LV_OBJ_FLAG_HIDDEN);

    /* 进度遮罩压在缩略图之上、从顶往下盖，见 thumb_progress_apply。 */
    g_job_thumb_mask = lv_obj_create(thumb);
    lv_obj_remove_style_all(g_job_thumb_mask);
    lv_obj_set_width(g_job_thumb_mask, LV_PCT(100));
    lv_obj_set_height(g_job_thumb_mask, LV_PCT(100));
    lv_obj_align(g_job_thumb_mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(g_job_thumb_mask, lv_color_hex(0xd4d4d6), 0);
    lv_obj_set_style_bg_opa(g_job_thumb_mask, 210, 0);
    lv_obj_clear_flag(g_job_thumb_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_job_thumb_mask, LV_OBJ_FLAG_HIDDEN);

    g_job_thumb_icon = lv_label_create(thumb);
    lv_label_set_text(g_job_thumb_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(g_job_thumb_icon, user_light_text, 0);
    lv_obj_set_style_text_opa(g_job_thumb_icon, LV_OPA_40, 0);
    lv_obj_set_style_text_font(g_job_thumb_icon, user_font_metric(), 0);
    lv_obj_center(g_job_thumb_icon);

    g_job_name = lv_label_create(g_job_card);
    lv_label_set_text(g_job_name, "--");
    lv_label_set_long_mode(g_job_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_job_name, LV_PCT(100));
    lv_obj_set_style_text_color(g_job_name, user_light_text, 0);
    lv_obj_set_style_text_font(g_job_name, user_font_title(), 0);

    /* 状态行：图标 + 状态词 …… 层号 + 百分比（稿上三者同色） */
    lv_obj_t *row = lv_obj_create(g_job_card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    g_job_badge = ui_user_make_icon_img(row, &ic_badge_spin,
                                        USER_METRIC_ICON_SZ * 3 / 4,
                                        lv_color_hex(kPhaseBlue));

    g_job_state = lv_label_create(row);
    lv_label_set_text(g_job_state, "Printing...");
    lv_obj_set_style_text_font(g_job_state, user_font_title(), 0);

    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    /* 层号 / 光路检测点位。取 dim 色 + 小一档字号，别跟同色的状态词与百分比抢视线。 */
    g_job_extra = lv_label_create(row);
    lv_label_set_text(g_job_extra, "");
    lv_obj_set_style_text_color(g_job_extra, user_light_text, 0);
    lv_obj_set_style_text_opa(g_job_extra, LV_OPA_40, 0);
    lv_obj_set_style_text_font(g_job_extra, user_font_caption(), 0);
    lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);

    g_job_percent = lv_label_create(row);
    lv_label_set_text(g_job_percent, "0%");
    lv_obj_set_style_text_font(g_job_percent, user_font_title(), 0);

    g_job_bar = lv_bar_create(g_job_card);
    lv_obj_remove_style_all(g_job_bar);
    lv_obj_set_size(g_job_bar, LV_PCT(100), USER_MON_BAR_H);
    lv_obj_set_style_bg_color(g_job_bar, user_light_text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_job_bar, 26, LV_PART_MAIN);        /* 稿 10% */
    lv_obj_set_style_radius(g_job_bar, USER_MON_BAR_H / 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_job_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(g_job_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_job_bar, USER_MON_BAR_H / 3, LV_PART_INDICATOR);
    lv_bar_set_range(g_job_bar, 0, 100);
    lv_bar_set_value(g_job_bar, 0, LV_ANIM_OFF);

    /* 完成态稿上没有进度条，换成一条 1px 分隔线（6338:27946 的 y=440）。 */
    g_job_divider = lv_obj_create(g_job_card);
    lv_obj_remove_style_all(g_job_divider);
    lv_obj_set_size(g_job_divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(g_job_divider, user_light_text, 0);
    lv_obj_set_style_bg_opa(g_job_divider, 26, 0);
    lv_obj_set_style_margin_ver(g_job_divider, USER_PAD_SM / 2, 0);
    lv_obj_add_flag(g_job_divider, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *stats = lv_obj_create(g_job_card);
    lv_obj_remove_style_all(stats);
    lv_obj_set_width(stats, LV_PCT(100));
    lv_obj_set_height(stats, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(stats, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(stats, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    build_stat_card(stats, "Time left", &g_job_time_cap, &g_job_time_val,
                    nullptr, nullptr);
    build_stat_card(stats, "Total Cons.", nullptr, &g_job_cons_val,
                    "G", &g_job_cons_unit);
    return v;
}

/* ========================================================================== *
 *  Status 页
 * ========================================================================== */

struct AirMetric {
    const char *title;
    float b_good, b_fair, b_poor, pseudo_max;
};

/* 阈值与首页 HOME 卡同一张表（稿分级表：PM2.5 35/75/150，PM10 50/150/350）。 */
const AirMetric kAirMetrics[2] = {
    { "PM2.5", 35.0f,  75.0f, 150.0f, 300.0f },
    { "PM10",  50.0f, 150.0f, 350.0f, 700.0f },
};
const char *const kLevelName[4] = { "Good", "Fair", "Poor", "Very Poor" };

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

void on_pm_toggle(lv_event_t * /*e*/)
{
    g_pm_metric = (g_pm_metric + 1) % 2;
    status_update();
}

/** 两个字号盒底对齐时，小字的基线会偏低，差值正好是两份字体 `base_line`（下伸部）
 *  之差，补成小字的 `pad_bottom` 抬回去。与首页 `make_metric_cell()` 同一个式子。 */
int32_t baseline_pad(const lv_font_t *big, const lv_font_t *small)
{
    const int32_t d = (int32_t)big->base_line - (int32_t)small->base_line;
    return d > 0 ? d : 0;
}

/* Status 页的一格指标卡（2×2 网格里的一张，稿 6338:28064 的 170×113）。
 *
 * **这一格与首页四宫格是稿上的同一个组件**，字号必须同一套：caption 20 配小后缀 13
 * （Montserrat 无 13 号，取最近的 caption 14）、数值 32 配单位 20。早先这里照 Job 卡
 * 那套给了 16 / 20 / 16，整体小一档半，上板一眼就能看出来。
 *
 * caption 与读数都可能是「大字 + 小一号后缀」的组合，LVGL 一个 label 只能一种字体，
 * 所以各自拆成两个 label 横排、用 baseline_pad() 把基线找齐。
 *
 * @param cap_sm caption 的小字后缀，可为 NULL（只有 Temp. 那格有）
 * @param unit   读数后面的单位
 */
lv_obj_t *make_status_cell(lv_obj_t *parent, const char *label, const char *cap_sm,
                           const char *unit, int32_t w, int32_t h, lv_obj_t **out_val)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_radius(cell, USER_MON_RADIUS, 0);
    lv_obj_set_style_bg_color(cell, user_light_card, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(cell, 3, 0);
    lv_obj_set_style_shadow_ofs_y(cell, 2, 0);
    lv_obj_set_style_shadow_color(cell, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(cell, 10, 0);
    lv_obj_set_style_pad_all(cell, USER_PAD_SM, 0);   /* 稿 16 */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    /* caption 行。**交叉轴必须是 END**：baseline_pad() 补的是「盒底对齐时两个字号的
     * 基线差」，只有底对齐才成立；CENTER 下反而会把小字往上抬半个补偿量。 */
    lv_obj_t *head = lv_obj_create(cell);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(head);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_color(cap, user_light_text, 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_60, 0);
    lv_obj_set_style_text_font(cap, user_font_title(), 0);

    if(cap_sm) {
        lv_obj_t *cs = lv_label_create(head);
        lv_label_set_text(cs, cap_sm);
        lv_obj_set_style_text_color(cs, user_light_text, 0);
        lv_obj_set_style_text_opa(cs, LV_OPA_60, 0);
        lv_obj_set_style_text_font(cs, user_font_caption(), 0);
        lv_obj_set_style_pad_bottom(cs, baseline_pad(user_font_title(),
                                                     user_font_caption()), 0);
    }

    lv_obj_t *row = lv_obj_create(cell);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_light_value, 0);
    lv_obj_set_style_text_font(val, user_font_metric(), 0);
    if(out_val) *out_val = val;

    lv_obj_t *u = lv_label_create(row);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_color(u, user_light_value, 0);
    lv_obj_set_style_text_font(u, user_font_title(), 0);
    lv_obj_set_style_pad_bottom(u, baseline_pad(user_font_metric(),
                                                user_font_title()), 0);
    return cell;
}

lv_obj_t *build_view_status(lv_obj_t *parent)
{
    lv_obj_t *v = lv_obj_create(parent);
    lv_obj_remove_style_all(v);
    lv_obj_set_width(v, LV_PCT(100));
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(v, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- PM 卡（稿 6338:28064，356×210）：标题行 + 分级大字 + 读数 + 四段色条 ----
     *
     * **定高不靠内容撑**：稿上色条与卡底之间留了 24 的空，内容自适应会把卡收矮一圈。
     * 横向内距取稿的 24（色条左缘 916 − 卡左缘 892），纵向上 16 下 24——稿上就是
     * 不对称的。三段之间只留 4，段与色条之间由色条自己的 margin_top 拉开。 */
    lv_obj_t *card = make_card(v, USER_MON_PM_H);
    lv_obj_set_style_pad_hor(card, USER_PAD, 0);     /* 稿 24（色条左缘 916 − 卡左 892）*/
    lv_obj_set_style_pad_top(card, USER_PAD_SM, 0);  /* 稿 16 */
    lv_obj_set_style_pad_bottom(card, USER_PAD, 0);  /* 稿 24，见下面占位那段 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 4, 0);

    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hl = lv_obj_create(head);
    lv_obj_remove_style_all(hl);
    lv_obj_set_size(hl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hl, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hl, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(hl, LV_OBJ_FLAG_SCROLLABLE);

    make_icon(hl, ic_home_set, USER_METRIC_ICON_SZ * 3 / 4, user_light_text);
    g_pm_title = lv_label_create(hl);
    lv_label_set_text(g_pm_title, kAirMetrics[0].title);
    lv_obj_set_style_text_color(g_pm_title, user_light_text, 0);
    lv_obj_set_style_text_opa(g_pm_title, LV_OPA_60, 0);
    lv_obj_set_style_text_font(g_pm_title, user_font_title(), 0);   /* 稿 20 */

    /* 切换钮：稿 (1188,124) 48×48 rx16，#333145@4% */
    lv_obj_t *sw = lv_button_create(head);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, USER_METRIC_ICON_SZ * 3 / 2, USER_METRIC_ICON_SZ * 3 / 2);
    lv_obj_set_style_radius(sw, USER_MON_RADIUS, 0);
    lv_obj_set_style_bg_color(sw, user_light_text, 0);
    lv_obj_set_style_bg_opa(sw, 10, 0);
    lv_obj_set_style_bg_opa(sw, 30, LV_STATE_PRESSED);
    lv_obj_add_event_cb(sw, on_pm_toggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(make_icon(sw, ic_switch_set, USER_METRIC_ICON_SZ * 3 / 4,
                            user_light_text));

    g_pm_level = lv_label_create(card);
    lv_label_set_text(g_pm_level, "--");
    lv_obj_set_style_text_color(g_pm_level, user_light_value, 0);
    lv_obj_set_style_text_font(g_pm_level, user_font_metric(), 0);   /* 稿 32 */

    g_pm_value = lv_label_create(card);
    lv_label_set_text(g_pm_value, "");
    lv_obj_set_style_text_color(g_pm_value, user_light_text, 0);
    lv_obj_set_style_text_opa(g_pm_value, LV_OPA_60, 0);
    lv_obj_set_style_text_font(g_pm_value, user_font_body(), 0);     /* 稿 24 */

    /* 四段色条 + 游标。**这里的四段是彩色的**（稿 #55D273/#F3CD3A/#F5803B/#EA4948），
     * 与首页 HOME 卡那条「白色四段、不透明度递增」不是一回事——那张卡是蓝底，白段
     * 才看得见；本卡是白底，只能靠颜色分档。
     *
     * **行高按游标直径给、色条只占中间 8**，不是让行高等于色条高：游标比色条高一圈
     * （稿上色条 y286..293、圆点 ~y283..303），行高若只有 8，LVGL 会把圆点按父内容区
     * 裁成一条 8 高的白片——实机上表现为「游标整个不见了」。踩过一次。 */
    const int32_t bar_h = USER_PAD_SM / 2;             /* 稿 8 */
    const int32_t dot_sz = USER_PAD_SM * 5 / 4;        /* 稿 20 */

    /* 撑开的占位：把色条顶到内容区底部。**比给色条写死 margin_top 稳**——三段文字的
     * 行高是字体算出来的，写死间距一旦加起来超过定高，色条会被卡片裁掉半截；用
     * flex_grow 则无论行高怎么变，色条都贴着 pad_bottom 那条线。
     * 卡底内距取 24：稿上色条 y286..293、卡底 323，20 高的行正好落在 279..299。 */
    lv_obj_t *spacer = lv_obj_create(card);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    g_pm_track = lv_obj_create(card);
    lv_obj_remove_style_all(g_pm_track);
    lv_obj_set_width(g_pm_track, LV_PCT(100));
    lv_obj_set_height(g_pm_track, dot_sz);
    lv_obj_set_flex_flow(g_pm_track, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_pm_track, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_pm_track, USER_PAD_SM / 4, 0);
    lv_obj_set_style_bg_opa(g_pm_track, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_pm_track, LV_OBJ_FLAG_SCROLLABLE);
    for(uint32_t c : kBarColors) {
        lv_obj_t *seg = lv_obj_create(g_pm_track);
        lv_obj_remove_style_all(seg);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_height(seg, bar_h);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }
    g_pm_dot = lv_obj_create(g_pm_track);
    lv_obj_remove_style_all(g_pm_dot);
    lv_obj_add_flag(g_pm_dot, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(g_pm_dot, dot_sz, dot_sz);
    lv_obj_set_style_radius(g_pm_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_pm_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_pm_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_pm_dot, 2, 0);
    lv_obj_set_style_border_color(g_pm_dot, user_light_text, 0);
    lv_obj_set_style_border_opa(g_pm_dot, LV_OPA_30, 0);

    /* ---- 2×2 指标网格（稿 170×113，缝 16）---- */
    const int32_t gap = USER_PAD_SM;
    const int32_t cw  = (panel_content_w() - gap) / 2;
    const int32_t ch  = USER_MON_CELL_H;

    lv_obj_t *grid = lv_obj_create(v);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, gap, 0);
    lv_obj_set_style_pad_row(grid, gap, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* 四项的 caption / 小后缀 / 单位与首页四宫格逐字一致（稿上就是同一个组件）。 */
    make_status_cell(grid, "Temp.", " (\xC2\xB0""C)", "\xC2\xB0", cw, ch, &g_st_temp);
    make_status_cell(grid, "O2",       nullptr, "%",   cw, ch, &g_st_o2);
    make_status_cell(grid, "Humidity", nullptr, "%",   cw, ch, &g_st_hum);
    make_status_cell(grid, "Pressure", nullptr, "kpa", cw, ch, &g_st_prs);
    return v;
}

void status_update(void)
{
    if(!g_pm_title) {
        return;
    }
    const AirMetric &m = kAirMetrics[g_pm_metric];
    const ui_device_snapshot_t &snap = *ui_device_state_get();
    const float v = (g_pm_metric == 0) ? snap.pm25_indoor : snap.pm10_indoor;

    lv_label_set_text(g_pm_title, m.title);
    if(metric_invalid(v)) {
        lv_label_set_text(g_pm_level, "--");
        lv_label_set_text(g_pm_value, "");
        if(g_pm_dot) lv_obj_add_flag(g_pm_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        float frac = 0.0f;
        const int level = air_classify(m, v, &frac);
        lv_label_set_text(g_pm_level, kLevelName[level]);
        lv_label_set_text_fmt(g_pm_value, "%.0f ug/m3", static_cast<double>(v));
        if(g_pm_dot && g_pm_track) {
            lv_obj_remove_flag(g_pm_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(g_pm_track);
            const int32_t tw  = lv_obj_get_width(g_pm_track);
            const int32_t dot = lv_obj_get_width(g_pm_dot);
            int32_t dx = static_cast<int32_t>(frac * static_cast<float>(tw)) - dot / 2;
            if(dx < 0)          dx = 0;
            if(dx > tw - dot)   dx = tw - dot;
            lv_obj_align(g_pm_dot, LV_ALIGN_LEFT_MID, dx, 0);
        }
    }

    struct { lv_obj_t *val; float v; } kCells[4] = {
        { g_st_temp, snap.temperature_c    },
        { g_st_o2,   snap.oxygen_percent   },
        { g_st_hum,  snap.humidity_percent },
        { g_st_prs,  snap.pressure_kpa     },
    };
    for(auto &c : kCells) {
        if(!c.val) {
            continue;
        }
        lv_obj_t *unit = lv_obj_get_sibling(c.val, 1);
        if(metric_invalid(c.v)) {
            lv_label_set_text(c.val, "--");
            if(unit) lv_obj_add_flag(unit, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text_fmt(c.val, "%.1f", static_cast<double>(c.v));
            if(unit) lv_obj_remove_flag(unit, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ========================================================================== *
 *  墨色滑块弹窗（取件 6338:27929 / 请先开外门 6339:28971）
 * ========================================================================== */

/**
 * 两个弹窗是同一副骨架：448 白卡 + 标题 + 居中副标题 + 400×94 的墨色滑条，取件那个
 * 底下再多一枚满宽 Cancel。滑条的尺寸与首页开门滑块相同（94 高、84 旋钮、内缩 5），
 * 所以直接复用 USER_HOME_SLIDE_H / USER_ACTION_BTN_SZ；配色与圆角（24 / 20，比首页
 * 小一圈）由控件的 INK 档自己给。
 *
 * 间距按稿（1280 档）：副标题 → 滑条 36、滑条 → Cancel 16。弹窗外壳的 body 默认行距
 * 是 USER_PAD（24），这里压回 USER_PAD_SM（16），副标题那段再用滑条的 margin_top 补
 * 到 36。Cancel 不进 footer：footer 自带 USER_PAD_SM 的 pad_top，叠上卡片行距会变成 32。
 *
 * @param on_cancel  非空才加 Cancel（外门弹窗稿上没有）
 * @param out_slide  可空；弹窗存续期间要拿滑条句柄（例如复位）时传入
 * @return 弹窗句柄；打开失败返回 nullptr
 */
ui_modal_dialog_t *open_ink_slide_dlg(const char *title, const char *sub_text,
                                      const char *hint, ui_slide_unlock_cb_t on_slide,
                                      lv_event_cb_t on_cancel,
                                      ui_slide_unlock_t **out_slide)
{
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(title, USER_MON_DLG_W, false);
    cfg.light_theme = true;
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!dlg) {
        return nullptr;
    }
    lv_obj_set_style_pad_row(dlg->body, USER_PAD_SM, 0);

    lv_obj_t *sub = ui_user_make_body_label(dlg->body, sub_text);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    ui_slide_unlock_cfg_t sc = {};
    sc.hint_text   = hint;
    sc.on_unlocked = on_slide;
    sc.track_w     = USER_MON_DLG_W - USER_PAD * 2;
    sc.track_h     = USER_HOME_SLIDE_H;
    sc.knob_sz     = USER_ACTION_BTN_SZ;
    sc.theme       = UI_SLIDE_THEME_INK;
    sc.knob_icon   = ui_user_pick_icon(ic_door_panel_set, USER_ACTION_BTN_SZ * 8 / 21);
    ui_slide_unlock_t *slide = ui_slide_unlock_create(dlg->body, &sc);
    if(slide) {
        lv_obj_set_style_margin_top(ui_slide_unlock_get_root(slide), USER_PAD_SM * 5 / 4, 0);
    }
    if(out_slide) {
        *out_slide = slide;
    }

    if(on_cancel) {
        /* 借页脚按钮的配色 / 字号 / 高（#333145@10%、22 号、72），几何按稿改：满宽、
         * 圆角 16（页脚那套是 12，这张卡稿上就是 16）。 */
        lv_obj_t *cancel = ui_user_make_confirm_footer_btn(dlg->body, "Cancel", false,
                                                           lv_color_white(), true,
                                                           on_cancel, nullptr);
        lv_obj_set_flex_grow(cancel, 0);
        lv_obj_set_width(cancel, LV_PCT(100));
        lv_obj_set_style_radius(cancel, USER_LIGHT_RADIUS * 2 / 3, 0);
    }
    return dlg;
}

/* ========================================================================== *
 *  Manual 页
 * ========================================================================== */

/** Inner Door 用的「请先开外门」滑条弹窗。 */
void close_outer_door_dlg(void)
{
    if(g_outer_door_dlg) {
        ui_modal_dialog_close(g_outer_door_dlg);
        g_outer_door_dlg = nullptr;
    }
}

void close_purify_door_dlg(void)
{
    if(g_purify_door_dlg) {
        ui_modal_dialog_close(g_purify_door_dlg);
        g_purify_door_dlg = nullptr;
    }
}

/** 跟 env 快照对齐外门开合；进页期间可能还没走过 observer。
 *  快照门状态无效时保留本页记忆（含开门 Rsp 乐观置位），避免 valid=0 把已开外门清掉
 *  导致 Inner Door 一直弹「请先开外门」。 */
void sync_outer_door_from_snap(void)
{
    const ui_device_snapshot_t *snap = ui_device_state_get();
    if(snap && snap->door_state_valid) {
        g_outer_door_open = snap->outer_door_open;
    }
}

/** Manual / 结果页共用的开一轮清粉（调用前已确认可发）。 */
void start_purify_now(void)
{
    g_pending_purify = false;
    close_purify_door_dlg();
    g_purify_busy = true;
    ui_clean_state_request(purify_owner(), g_job_label);
    g_purify = PURIFY_RUNNING;
    apply_phase();
}

/** 外门关到位后：若有 pending Manual 吹粉，续跑一次。 */
void try_resume_pending_purify(void)
{
    if(!g_pending_purify || g_outer_door_open ||
       g_purify == PURIFY_RUNNING || g_purify_busy) {
        return;
    }
    start_purify_now();
}

void on_purify_door_confirm_ok(lv_event_t * /*e*/)
{
    sync_outer_door_from_snap();
    if(g_outer_door_open) {
        ui_toast_show("Close the outer door fully", ui_color_danger, 1800);
        return;
    }
    start_purify_now();
}

void on_purify_door_confirm_cancel(lv_event_t * /*e*/)
{
    g_pending_purify = false;
    close_purify_door_dlg();
}

/** Manual 吹粉须外门关：未关则弹确认，关到位后再开。 */
void show_purify_door_dlg(void)
{
    if(g_purify_door_dlg) {
        return;
    }
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Close the outer door", USER_DLG_W, true);
    cfg.light_theme = true;
    cfg.with_close_btn = false;
    g_purify_door_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_purify_door_dlg) {
        return;
    }
    lv_obj_t *sub = ui_user_make_body_label(
        g_purify_door_dlg->body,
        "Powder cleaning needs the outer door closed. Close it fully, then confirm.");
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_flow(g_purify_door_dlg->footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_purify_door_dlg->footer, USER_PAD, 0);
    lv_obj_set_flex_align(g_purify_door_dlg->footer, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ok = ui_user_make_confirm_footer_btn(
        g_purify_door_dlg->footer, "Confirm",
        true, user_light_nav_on, true,
        on_purify_door_confirm_ok, nullptr);
    lv_obj_set_flex_grow(ok, 0);
    lv_obj_set_width(ok, LV_PCT(100));
    lv_obj_t *cancel = ui_user_make_confirm_footer_btn(
        g_purify_door_dlg->footer, "Cancel",
        false, user_light_nav_on, true,
        on_purify_door_confirm_cancel, nullptr);
    lv_obj_set_flex_grow(cancel, 0);
    lv_obj_set_width(cancel, LV_PCT(100));
}

void apply_inner_door_btn(void)
{
    if(!g_door_btn) {
        return;
    }
    lv_obj_set_style_bg_color(g_door_btn,
        g_inner_door_open ? lv_color_hex(kBtnBlue) : user_light_text, 0);
    lv_obj_set_style_bg_opa(g_door_btn, g_inner_door_open ? LV_OPA_COVER : 10, 0);
    lv_obj_t *ic = lv_obj_get_child(g_door_btn, 0);
    lv_obj_t *img = ic ? lv_obj_get_child(ic, 0) : nullptr;
    if(img) {
        lv_obj_set_style_image_recolor(
            img, g_inner_door_open ? lv_color_white() : user_light_text, 0);
    }
    if(g_inner_door_busy) {
        lv_obj_add_state(g_door_btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(g_door_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(g_door_btn, LV_OPA_50, 0);
    } else {
        lv_obj_clear_state(g_door_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(g_door_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(g_door_btn, LV_OPA_COVER, 0);
    }
}

void set_inner_door_busy(bool busy)
{
    if(g_inner_door_busy == busy) {
        apply_inner_door_btn();
        return;
    }
    g_inner_door_busy = busy;
    apply_inner_door_btn();
}

/** 全开→关套件；非全开→开套件。调用前须已确认外门开。 */
void request_inner_door_toggle(void)
{
    if(g_inner_door_busy) {
        return;
    }
    const ui_device_snapshot_t *snap = ui_device_state_get();
    const bool open_now =
        (snap && snap->door_state_valid) ? snap->inner_door_open : g_inner_door_open;
    const bool want_open = !open_now;
    /* 乐观禁钮 + 刷新外观；套件结束 Glavo 推 busy=0。 */
    g_inner_door_open = want_open;
    set_inner_door_busy(true);
    ui::DebugMsgProxy::instance().send_inner_door(
        want_open ? middleware::DebugMotorAction::Forward
                  : middleware::DebugMotorAction::Reverse);
}

/** 外门开到位后：若有 pending Inner Door，续跑一次。 */
void try_resume_pending_inner_door(void)
{
    if(!g_pending_inner_door || !g_outer_door_open || g_inner_door_busy) {
        return;
    }
    g_pending_inner_door = false;
    g_inner_door_open = g_pending_inner_want_open;
    set_inner_door_busy(true);
    ui::DebugMsgProxy::instance().send_inner_door(
        g_pending_inner_want_open ? middleware::DebugMotorAction::Forward
                                  : middleware::DebugMotorAction::Reverse);
}

void on_outer_door_slide(void * /*u*/)
{
    close_outer_door_dlg();
    control_open_door_request();
}

/** 请先开外门（稿 6339:28971）。稿上只有标题 + 副标题 + 滑条，没有 Cancel。
 *  副标题稿里还是占位文案（「打开外门，文案不知道」），这里先写成一句动作提示，
 *  控制在 1280 档一行放得下。 */
void show_outer_door_dlg(void)
{
    if(g_outer_door_dlg) {
        return;
    }
    g_outer_door_dlg = open_ink_slide_dlg("Open the outer door first",
                                          "Slide to unlock and open the outer door.",
                                          "SLIDE TO UNLOCK", on_outer_door_slide,
                                          nullptr, nullptr);
}

/** 外门没开就拦下并弹提示。返回 true 表示可以继续。 */
bool require_outer_door(void)
{
    sync_outer_door_from_snap();
    if(g_outer_door_open) {
        return true;
    }
    show_outer_door_dlg();
    return false;
}

void on_plate_changed(lv_event_t *e)
{
    const uint8_t idx = ui_segmented_get_active(lv_event_get_target_obj(e));
    if(idx == UI_SEGMENTED_NONE) {
        return;
    }
    /* Top→Z=1mm，Bottom→Z=-10mm，绝对到位由 Glavo 按当前 Z 算增量。
     * Level 发 Z 点动停止，打断还在跑的 Top/Bottom 位移。点已选中项不发事件，所以只有
     * 高亮在 Top/Bottom 时才会走到 Level 这一支。 */
    auto &proxy = ui::DebugMsgProxy::instance();
    if(idx == 0) {
        proxy.send_axis_goto(middleware::DebugAxis::Z, 1.0f);
    } else if(idx == 1) {
        proxy.send_axis_hold_stop(middleware::DebugAxis::Z);
    } else if(idx == 2) {
        proxy.send_axis_goto(middleware::DebugAxis::Z, -10.0f);
    }
}

/** 停掉长按点动。没在长按就什么都不发：点按之后补停可能把那一步增量打断。 */
void manual_hold_stop(void)
{
    if(g_hold_axis < 0) {
        return;
    }
    ui::DebugMsgProxy::instance().send_axis_hold_stop(
        static_cast<middleware::DebugAxis>(g_hold_axis));
    g_hold_axis = -1;
}

/** Z 点动接手前调用。高亮在 Top/Bottom = 上一条 Z 命令是到位、之后没点动过，那段位移
 *  可能还在跑：先发点动停止再接手，高亮退回 Level。只有这时才发停止，连续点按不会
 *  互相打断；已到位时这条停止对静止的轴无害。下标 0/1/2 = Top/Level/Bottom。 */
void plate_leave_preset(void)
{
    const uint8_t cur = ui_segmented_get_active(g_plate_seg);
    if(cur != 0 && cur != 2) {
        return;
    }
    ui::DebugMsgProxy::instance().send_axis_hold_stop(middleware::DebugAxis::Z);
    ui_segmented_set_active(g_plate_seg, 1);
}

/**
 * Manual 点动钮（Z 上下、S −/+）：点按走一步（kManualZStepMm / kManualSStepMm），
 * 按住满长按时间转速度点动、松手停。
 * user_data = axis * 4 + (dir + 1)，见 attach_jog_events()。
 *
 * 点按挂 SHORT_CLICKED 而不是 CLICKED：LVGL 长按松手后照样发 CLICKED，
 * 用它会在停车之后再多走一步。
 */
void on_manual_jog(lv_event_t *e)
{
    const auto ud = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const auto axis = static_cast<middleware::DebugAxis>(ud / 4);
    const int dir = static_cast<int>(ud % 4) - 1;
    auto &proxy = ui::DebugMsgProxy::instance();
    switch(lv_event_get_code(e)) {
    case LV_EVENT_SHORT_CLICKED:
        if(axis == middleware::DebugAxis::Z) {
            plate_leave_preset();
        }
        /* Z 走 μm AXIS_INC（见 Glavo handle_debug_axis_jog），S 走 int16 mm。 */
        proxy.send_axis_jog(axis, middleware::DebugJogMode::Click, dir,
                            axis == middleware::DebugAxis::Z ? kManualZStepMm
                                                             : kManualSStepMm);
        break;
    case LV_EVENT_LONG_PRESSED:
        manual_hold_stop();
        if(axis == middleware::DebugAxis::Z) {
            plate_leave_preset();
        }
        proxy.send_axis_jog(axis, middleware::DebugJogMode::Hold, dir,
                            axis == middleware::DebugAxis::Z ? kManualZHoldSpeed
                                                             : kManualSHoldSpeed);
        g_hold_axis = static_cast<int>(axis);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        manual_hold_stop();
        break;
    default:
        break;
    }
}

/** 给点动钮挂上点按 + 长按四个事件。 */
void attach_jog_events(lv_obj_t *btn, middleware::DebugAxis axis, int dir)
{
    void *ud = reinterpret_cast<void *>(static_cast<intptr_t>(
        static_cast<int>(axis) * 4 + (dir < 0 ? 0 : 2)));
    lv_obj_add_event_cb(btn, on_manual_jog, LV_EVENT_SHORT_CLICKED, ud);
    lv_obj_add_event_cb(btn, on_manual_jog, LV_EVENT_LONG_PRESSED,  ud);
    lv_obj_add_event_cb(btn, on_manual_jog, LV_EVENT_RELEASED,      ud);
    lv_obj_add_event_cb(btn, on_manual_jog, LV_EVENT_PRESS_LOST,    ud);
}

/** S 轴复位 = 回 Home。别改成设零：MCU 只有 Z 轴有工作零点，S 轴的设零会被 Glavo 丢掉。 */
void on_s_home(lv_event_t * /*e*/)
{
    ui::DebugMsgProxy::instance().send_axis_home(middleware::DebugAxis::S);
}

void on_inner_door(lv_event_t * /*e*/)
{
    if(g_inner_door_busy) {
        return;
    }
    if(!require_outer_door()) {
        const ui_device_snapshot_t *snap = ui_device_state_get();
        const bool open_now =
            (snap && snap->door_state_valid) ? snap->inner_door_open : g_inner_door_open;
        g_pending_inner_want_open = !open_now;
        g_pending_inner_door = true;
        return;
    }
    g_pending_inner_door = false;
    request_inner_door_toggle();
}

/** Manual 卡里那种「标题 + 右侧读数」的表头。
 *  稿上这一行是**定高 32**（1280 档）、文字竖直居中：卡顶 120 + 内距 17 → 行 137..169，
 *  「Plate Position」的墨迹中心 152.5 正在行心。按内容撑只有字体行高 18（montserrat_16），
 *  卡会矮一截。标题与读数都是 20 号（1024 档折 16），读数 #333145 @ 50%（稿面实测 148,148,162）。 */
lv_obj_t *manual_card_head(lv_obj_t *card, const char *title, lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, USER_MON_ROW_H / 2);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, user_light_text, 0);
    lv_obj_set_style_text_font(t, user_font_title(), 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "0");
    lv_obj_set_style_text_color(val, user_light_text, 0);
    lv_obj_set_style_text_opa(val, LV_OPA_50, 0);
    lv_obj_set_style_text_font(val, user_font_title(), 0);
    if(out_val) *out_val = val;
    return row;
}

/** Manual 里那种浅底圆角钮（上/下 jog），点按 / 长按事件见 attach_jog_events()。 */
lv_obj_t *make_jog_btn(lv_obj_t *parent, const lv_image_dsc_t *const *icon,
                       middleware::DebugAxis axis, int dir)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, USER_MON_ROW_H * 7 / 8);   /* 稿 56 */
    lv_obj_set_style_radius(b, USER_MON_RADIUS, 0);
    lv_obj_set_style_bg_color(b, user_light_text, 0);
    lv_obj_set_style_bg_opa(b, 13, 0);
    lv_obj_set_style_bg_opa(b, 40, LV_STATE_PRESSED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    attach_jog_events(b, axis, dir);
    lv_obj_center(make_icon(b, icon, USER_METRIC_ICON_SZ * 3 / 4, user_light_text));
    return b;
}

/** Manual 里那种 64 圆钮（S 轴的 −/+/复位、Inner Door 的电源）。
 *  cb 为空时不挂 CLICKED：S 轴 −/+ 由调用方另挂 attach_jog_events()。 */
lv_obj_t *make_round_btn(lv_obj_t *parent, const lv_image_dsc_t *const *icon,
                         lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, USER_PRINT_CTL_BTN, USER_PRINT_CTL_BTN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, user_light_text, 0);
    lv_obj_set_style_bg_opa(b, 10, 0);
    lv_obj_set_style_bg_opa(b, 30, LV_STATE_PRESSED);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_center(make_icon(b, icon, USER_METRIC_ICON_SZ * 3 / 4, user_light_text));
    return b;
}

lv_obj_t *build_view_manual(lv_obj_t *parent)
{
    lv_obj_t *v = lv_obj_create(parent);
    lv_obj_remove_style_all(v);
    lv_obj_set_width(v, LV_PCT(100));
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(v, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Plate Position（稿 6307:40977 的 (892,120) 356×258）----
     * 卡内纵向：内距 16 + 表头 32 + 24 + 分段 88 + 24 + 上下钮 56 + 内距 16（稿 17，差 1）。
     * 两道 24 稿上一样（表头 169 → 分段 193、分段 281 → 上下钮 305），用同一个 pad_row；
     * 早先给的 USER_PAD_SM / 2（8）让卡矮了 48，底部按钮上方空出一大块。 */
    lv_obj_t *c1 = make_card(v, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c1, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(c1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c1, USER_MON_ROW_H * 3 / 8, 0);

    manual_card_head(c1, "Plate Position", &g_plate_val);

    static const ui_segmented_item_t kPlateItems[3] = {
        { "Top",    ic_plate_top_set    },
        { "Level",  ic_plate_level_set  },
        { "Bottom", ic_plate_bottom_set },
    };
    ui_segmented_cfg_t pc = {};
    pc.items          = kPlateItems;
    pc.count          = 3;
    pc.track_w        = 0;                       /* 铺满卡内容区 */
    pc.track_h        = USER_MON_ROW_H * 11 / 8; /* 稿 88 */
    pc.track_radius   = USER_MON_RADIUS;
    pc.pill_radius    = USER_MON_RADIUS;
    pc.inset          = 0;                       /* 稿上胶囊与轨道同高同顶 */
    pc.gap            = USER_PAD_SM * 5 / 8;     /* 稿 10 */
    pc.track_color    = user_light_text;
    pc.track_opa      = 13;
    pc.pill_color     = lv_color_hex(kPlateOn);
    pc.pill_opa       = LV_OPA_COVER;
    pc.color_on       = lv_color_white();
    pc.color_off      = user_light_text;
    pc.color_disabled = user_light_text;
    pc.font           = user_font_text_sm();
    pc.icon_sz        = USER_METRIC_ICON_SZ;
    pc.icon_gap       = USER_PAD_SM / 4;
    pc.selected       = 1;                       /* Level：启动时不知道 Z 在哪，不冒充停在 Top */
    g_plate_seg = ui_segmented_create(c1, &pc);
    lv_obj_add_event_cb(g_plate_seg, on_plate_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *jog = lv_obj_create(c1);
    lv_obj_remove_style_all(jog);
    lv_obj_set_width(jog, LV_PCT(100));
    lv_obj_set_height(jog, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(jog, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(jog, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(jog, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(jog, LV_OBJ_FLAG_SCROLLABLE);
    g_jog_up   = make_jog_btn(jog, ic_chevron_up_set,   middleware::DebugAxis::Z,  1);
    g_jog_down = make_jog_btn(jog, ic_chevron_down_set, middleware::DebugAxis::Z, -1);

    /* ---- 铺粉刮刀 S 轴（稿 (892,394) 356×154）----
     * 稿上标题写的是「打印平面高度（S轴）」，那是把 Z 卡标题复制过去忘改了；S 轴
     * 实际是铺粉刮刀，沿用调试页与旧控制页的 `Powder Scraper (S)`，别照稿改回去。 */
    /* 卡内纵向：内距 16 + 表头 32 + 24 + 圆钮 64 + 内距 16（稿 154，差 2）。 */
    lv_obj_t *c2 = make_card(v, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c2, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c2, USER_MON_ROW_H * 3 / 8, 0);

    manual_card_head(c2, "Powder Scraper (S)", &g_s_val);
    lv_label_set_text(g_s_val, "0.000 mm");

    lv_obj_t *srow = lv_obj_create(c2);
    lv_obj_remove_style_all(srow);
    lv_obj_set_width(srow, LV_PCT(100));
    lv_obj_set_height(srow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(srow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);
    attach_jog_events(make_round_btn(srow, ic_minus_set, nullptr),
                      middleware::DebugAxis::S, -1);
    attach_jog_events(make_round_btn(srow, ic_plus_set, nullptr),
                      middleware::DebugAxis::S, 1);
    make_round_btn(srow, ic_reset_set, on_s_home);

    /* ---- Inner Door（稿 (892,564) 356×98）---- */
    lv_obj_t *c3 = make_card(v, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c3, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(c3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c3, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dl = lv_label_create(c3);
    lv_label_set_text(dl, "Inner Door");
    lv_obj_set_style_text_color(dl, user_light_text, 0);
    lv_obj_set_style_text_font(dl, user_font_title(), 0);   /* 与前两张卡的表头同号 */

    g_door_btn = make_round_btn(c3, ic_power_set, on_inner_door);
    return v;
}

/* ========================================================================== *
 *  底部按钮组
 * ========================================================================== */

void on_print_click(lv_event_t * /*e*/)
{
    control_start();
}

/** 暂停/恢复：乐观切换图标并发请求，后端回执经 register_print_pause_done 校正。
 *
 *  **发不出去时必须回滚并明说**：Pause/Resume 不在 IPC 可靠白名单里（过期的暂停
 *  补发比丢弃更危险），发失败就是真的没人收。若界面仍停在 Pause 而振镜还在扫，
 *  那是最坏的一种误导。 */
void on_pause_click(lv_event_t * /*e*/)
{
    if(g_phase == PHASE_PREPARING) {
        return;
    }
    const bool want_pause = (g_phase != PHASE_PAUSED);
    const phase_t prev = g_phase;
    g_phase = want_pause ? PHASE_PAUSED : PHASE_PRINTING;
    apply_phase();

    const bool sent = want_pause
        ? ui::UiMsgProxy::instance().send_print_pause()
        : ui::UiMsgProxy::instance().send_print_resume();
    if(!sent) {
        g_phase = prev;
        apply_phase();
        ui_toast_show(want_pause ? "Pause failed, please retry"
                                 : "Resume failed, please retry",
                      ui_color_danger, 2000);
    }
}

void on_stop_click(lv_event_t * /*e*/)
{
    control_terminate_request();
}

/** 本次吹粉算谁发起的。两档语义不同，**别混**：
 *  - 打印结束后那次走 `PRINT_RESULT`，带清粉录像、接进同一条 timelapse、进打印历史；
 *  - Manual 页那次走 `MANUAL`，纯手动清一遍，不录像不落盘（须外门关）。
 *  判据就是当前在哪个 Tab——Manual 在打印态是禁用的，两者不会重叠。 */
ui_clean_owner_t purify_owner(void)
{
    return g_tab == TAB_MANUAL ? UI_CLEAN_OWNER_MANUAL : UI_CLEAN_OWNER_PRINT_RESULT;
}

void on_purify_click(lv_event_t * /*e*/)
{
    /* 清粉中再点 = Close 停风扇/停粉；空闲再点 = 再开一轮。与结果页同一套可终止逻辑。 */
    if(g_purify == PURIFY_RUNNING || g_purify_busy) {
        ui_clean_state_stop();
        return;
    }
    /* Manual：须外门关。未关弹确认，关到位后（Confirm 或传感器关边沿）再开。 */
    if(g_tab == TAB_MANUAL) {
        sync_outer_door_from_snap();
        if(g_outer_door_open) {
            g_pending_purify = true;
            show_purify_door_dlg();
            return;
        }
        g_pending_purify = false;
    }
    start_purify_now();
}

void close_retrieve_dlg(void)
{
    if(g_retrieve_dlg) {
        ui_modal_dialog_close(g_retrieve_dlg);
        g_retrieve_dlg  = nullptr;
        g_retrieve_slide = nullptr;
    }
}

void on_retrieve_cancel(lv_event_t * /*e*/)
{
    close_retrieve_dlg();
}

void on_retrieve_slide(void * /*u*/)
{
    close_retrieve_dlg();
    control_print_result_collect();
}

/** 取件弹窗（稿 6338:27929）：标题 + 说明 + SLIDE TO RETRIEVE + Cancel。
 *  改版前这是浮层里一条常驻的 `SWIPE TO COLLECT`，新稿收成「按钮 → 弹窗」两步。
 *
 *  RETRIEVE 在完成与终止两态都会出（phase_finished()），稿只画了完成态那一版。终止态
 *  沿用同一张卡，只换标题和副标题——不然 Job 卡写着 Print Terminated，弹窗却说 complete。 */
void show_retrieve_dlg(void)
{
    if(g_retrieve_dlg) {
        return;
    }
    if(g_purify == PURIFY_RUNNING) {
        ui_toast_show("Air purifying, please wait", user_color_accent, 1200);
        return;
    }
    const bool stopped = (g_phase == PHASE_TERMINATED);
    g_retrieve_dlg = open_ink_slide_dlg(
        stopped ? "Print terminated" : "Print complete",
        stopped ? "Job stopped. Slide to retrieve the part."
                : "Job complete. Slide to retrieve the part.",
        "SLIDE TO RETRIEVE", on_retrieve_slide, on_retrieve_cancel, &g_retrieve_slide);
}

void on_wide_click(lv_event_t * /*e*/)
{
    if(g_tab == TAB_MANUAL) {
        control_open_door_request();
    } else {
        show_retrieve_dlg();
    }
}

/** 吹粉图标一圈 2s 逆时针转（吹粉中）。 */
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
    lv_image_set_pivot(img, lv_obj_get_width(img) / 2, lv_obj_get_height(img) / 2);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_exec_cb(&a, purify_spin_exec);
    lv_anim_set_values(&a, 3600, 0);
    lv_anim_set_duration(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

lv_obj_t *build_bottom(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, USER_ACTION_BTN_SZ);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 深色主钮（PRINT）。四档里只有它是整条 356 宽。 */
    g_btn_print = lv_button_create(row);
    lv_obj_remove_style_all(g_btn_print);
    lv_obj_set_flex_grow(g_btn_print, 1);
    lv_obj_set_height(g_btn_print, LV_PCT(100));
    lv_obj_set_style_radius(g_btn_print, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(g_btn_print, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(g_btn_print, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_print, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(g_btn_print, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_btn_print, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_btn_print, USER_PAD_SM, 0);
    lv_obj_clear_flag(g_btn_print, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_btn_print, on_print_click, LV_EVENT_CLICKED, nullptr);
    make_icon(g_btn_print, ic_folder_set, USER_METRIC_ICON_SZ, lv_color_white());
    lv_obj_t *pl = lv_label_create(g_btn_print);
    lv_label_set_text(pl, "PRINT");
    lv_obj_set_style_text_color(pl, lv_color_white(), 0);
    lv_obj_set_style_text_font(pl, user_font_title(), 0);

    /* 暂停/恢复：稿上图标是两条竖杠（运行中）与一枚实心三角（暂停中），
     * 都是纯几何，直接用 LVGL 图元画，不出图标资源。 */
    g_btn_pause = lv_button_create(row);
    lv_obj_remove_style_all(g_btn_pause);
    lv_obj_set_flex_grow(g_btn_pause, 1);
    lv_obj_set_height(g_btn_pause, LV_PCT(100));
    lv_obj_set_style_radius(g_btn_pause, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(g_btn_pause, lv_color_hex(kBtnBlue), 0);
    lv_obj_set_style_bg_opa(g_btn_pause, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_pause, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(g_btn_pause, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_btn_pause, on_pause_click, LV_EVENT_CLICKED, nullptr);

    g_pause_bars = lv_obj_create(g_btn_pause);
    lv_obj_remove_style_all(g_pause_bars);
    lv_obj_set_size(g_pause_bars, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_pause_bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_pause_bars, USER_PAD_SM / 2, 0);   /* 稿 9 */
    lv_obj_set_style_bg_opa(g_pause_bars, LV_OPA_TRANSP, 0);
    lv_obj_center(g_pause_bars);
    for(int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_obj_create(g_pause_bars);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, USER_PAD_SM / 3, USER_PAD_SM * 5 / 4);   /* 稿 5×20 */
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
    /* 恢复态那枚实心三角沿用手维护的单档 ic_play（不是多档 set，没有 _set 变体）。 */
    g_pause_tri = ui_user_make_icon_img(g_btn_pause, &ic_play,
                                        USER_METRIC_ICON_SZ * 3 / 4, lv_color_white());
    lv_obj_center(g_pause_tri);
    lv_obj_add_flag(g_pause_tri, LV_OBJ_FLAG_HIDDEN);

    g_btn_stop = lv_button_create(row);
    lv_obj_remove_style_all(g_btn_stop);
    lv_obj_set_flex_grow(g_btn_stop, 1);
    lv_obj_set_height(g_btn_stop, LV_PCT(100));
    lv_obj_set_style_radius(g_btn_stop, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(g_btn_stop, lv_color_hex(kBtnRed), 0);
    lv_obj_set_style_bg_opa(g_btn_stop, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_stop, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(g_btn_stop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_btn_stop, on_stop_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sq = lv_obj_create(g_btn_stop);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, USER_PAD_SM * 5 / 4, USER_PAD_SM * 5 / 4);   /* 稿 20×20 */
    lv_obj_set_style_radius(sq, 2, 0);
    lv_obj_set_style_bg_color(sq, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_center(sq);

    /* 吹粉：84 见方的蓝方钮（稿 (892,684) 83×83 rx23.5）。 */
    g_btn_purify = lv_button_create(row);
    lv_obj_remove_style_all(g_btn_purify);
    lv_obj_set_size(g_btn_purify, USER_ACTION_BTN_SZ, LV_PCT(100));
    lv_obj_set_style_radius(g_btn_purify, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(g_btn_purify, lv_color_hex(kBtnBlue), 0);
    lv_obj_set_style_bg_opa(g_btn_purify, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_purify, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(g_btn_purify, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_btn_purify, on_purify_click, LV_EVENT_CLICKED, nullptr);
    g_purify_icon = make_icon(g_btn_purify, ic_purify_ring_set,
                              USER_METRIC_ICON_SZ, lv_color_white());
    lv_obj_center(g_purify_icon);

    /* RETRIEVE / OPEN 共用：稿上两态都是 256 宽的深色钮，只差图标与文案。 */
    g_btn_wide = lv_button_create(row);
    lv_obj_remove_style_all(g_btn_wide);
    lv_obj_set_flex_grow(g_btn_wide, 1);
    lv_obj_set_height(g_btn_wide, LV_PCT(100));
    lv_obj_set_style_radius(g_btn_wide, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(g_btn_wide, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(g_btn_wide, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_wide, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(g_btn_wide, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_btn_wide, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_btn_wide, USER_PAD_SM, 0);
    lv_obj_clear_flag(g_btn_wide, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_btn_wide, on_wide_click, LV_EVENT_CLICKED, nullptr);
    make_icon(g_btn_wide, ic_door_panel_set, USER_METRIC_ICON_SZ, lv_color_white());
    g_btn_wide_lbl = lv_label_create(g_btn_wide);
    lv_label_set_text(g_btn_wide_lbl, "OPEN");
    lv_obj_set_style_text_color(g_btn_wide_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_btn_wide_lbl, user_font_title(), 0);

    return row;
}

/** 底部四档按哪个 Tab + 哪个 phase 决定，见 bottom_mode_t。 */
bottom_mode_t bottom_mode(void)
{
    if(g_tab == TAB_MANUAL)  return BOTTOM_MANUAL;
    if(phase_running())      return BOTTOM_RUN;
    if(phase_finished())     return BOTTOM_RETRIEVE;
    return BOTTOM_PRINT;
}

void apply_bottom(void)
{
    if(!g_bottom) {
        return;
    }
    const bottom_mode_t m = bottom_mode();
    auto show = [](lv_obj_t *o, bool on) {
        if(!o) return;
        if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(g_btn_print,  m == BOTTOM_PRINT);
    show(g_btn_pause,  m == BOTTOM_RUN);
    show(g_btn_stop,   m == BOTTOM_RUN);
    show(g_btn_purify, m == BOTTOM_RETRIEVE || m == BOTTOM_MANUAL);
    show(g_btn_wide,   m == BOTTOM_RETRIEVE || m == BOTTOM_MANUAL);

    if(g_btn_wide_lbl) {
        lv_label_set_text(g_btn_wide_lbl, m == BOTTOM_MANUAL ? "OPEN" : "RETRIEVE");
    }
    /* 光路检测期间暂停没有意义（运动包还没开始），灰掉；终止钮保持可点。 */
    if(g_btn_pause) {
        const bool ok = (g_phase != PHASE_PREPARING);
        if(ok) {
            lv_obj_add_flag(g_btn_pause, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(g_btn_pause, LV_OPA_COVER, 0);
        } else {
            lv_obj_remove_flag(g_btn_pause, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(g_btn_pause, LV_OPA_40, 0);
        }
    }
}

/* ========================================================================== *
 *  态切换
 * ========================================================================== */

/** 暂停钮的两根竖条 ⇄ 实心三角。后端回执也走这里，保证与真实状态一致。 */
void set_pause_ui(bool paused)
{
    if(g_pause_bars) {
        if(paused) lv_obj_add_flag(g_pause_bars, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_remove_flag(g_pause_bars, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_pause_tri) {
        if(paused) lv_obj_remove_flag(g_pause_tri, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(g_pause_tri, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 一处收口：状态行文案与配色、进度条、两列语义、底部按钮、状态栏、状态胶囊。 */
void apply_phase(void)
{
    if(!g_job_state) {
        return;
    }
    const lv_color_t accent = phase_accent();
    const bool finished = phase_finished();

    struct BadgeSpec { const lv_image_dsc_t *icon; const char *text; };
    static const BadgeSpec kBadges[6] = {
        { &ic_badge_spin,  ""                  },   /* IDLE：Job 页此时是空态 */
        { &ic_badge_spin,  "Preparing..."      },
        { &ic_badge_spin,  "Printing..."       },
        { &ic_badge_play,  "Pause"             },
        { &ic_badge_alert, "Print Terminated"  },
        { &ic_badge_check, "Print Complete"    },
    };
    const BadgeSpec &b = kBadges[g_phase];
    if(g_job_badge) {
        lv_obj_t *img = lv_obj_get_child(g_job_badge, 0);
        if(img) {
            lv_image_set_src(img, b.icon);
            lv_obj_set_style_image_recolor(img, accent, 0);
        }
    }
    /* PREPARING 优先用 prep 推送的步骤文案；未到首条进度前仍显示 Preparing... */
    if(g_phase == PHASE_PREPARING && g_prep_title[0]) {
        lv_label_set_text(g_job_state, g_prep_title);
    } else {
        lv_label_set_text(g_job_state, b.text);
    }
    lv_obj_set_style_text_color(g_job_state, accent, 0);
    if(g_job_percent) {
        lv_obj_set_style_text_color(g_job_percent, accent, 0);
    }

    if(g_job_bar) {
        uint32_t fill_b = kPhaseBlueFillB;
        lv_color_t fill_a = accent;
        if(g_phase == PHASE_TERMINATED)  fill_b = kPhaseOrangeB;
        else if(g_phase == PHASE_DONE)   fill_b = kPhaseGreenB;
        else                             fill_a = lv_color_hex(kBtnBlue);
        lv_obj_set_style_bg_color(g_job_bar, fill_a, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(g_job_bar, lv_color_hex(fill_b),
                                       LV_PART_INDICATOR);
    }
    /* 完成/终止态稿上没有进度条，换成一条分隔线。 */
    if(g_job_bar && g_job_divider) {
        if(finished) {
            lv_obj_add_flag(g_job_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(g_job_divider, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_job_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_job_divider, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* 两列语义：打印中给剩余时间 + 耗材占位；结束态才有实际耗时与耗材。
     * caption 照稿写 Time used / Total Cons.，口径仍是原来那套。 */
    if(g_job_time_cap) {
        lv_label_set_text(g_job_time_cap, finished ? "Time used" : "Time left");
    }

    set_pause_ui(g_phase == PHASE_PAUSED);
    purify_spin_set(g_purify == PURIFY_RUNNING);
    chip_set(g_purify == PURIFY_RUNNING ? "Air purifying" : nullptr);

    /* Job 页空态 ⇄ 卡片 */
    if(g_job_empty && g_job_card) {
        const bool has_job = (g_phase != PHASE_IDLE);
        if(has_job) {
            lv_obj_add_flag(g_job_empty, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(g_job_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_job_empty, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_job_card, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 整个打印会话（含 Preparing / 结果页）禁用 Manual，避免轴/门误操作。
     * 置灰当前选中项时控件会自己退到第一个可用项并发一次 VALUE_CHANGED。 */
    if(g_tabs) {
        ui_segmented_set_item_enabled(g_tabs, TAB_MANUAL, !g_printing);
    }

    apply_bottom();
    apply_light_btn_interactive();

    /* 两条提示走全局消息中心而不是本页自己的一条 bar：离开本页也看得到，且与后端
     * 上报的告警排在同一摞里。**必须是 sync 不能是 emit**——本函数打印期间每秒好
     * 几次，emit 会把 toast 反复弹起来。 */
    const bool masked = finished && (g_purify == PURIFY_DONE);
    if(g_printing && masked) {
        ui_message_sync((uint16_t)middleware::DeviceEventCode::UiWearMask, 0);
    } else {
        ui_message_retract((uint16_t)middleware::DeviceEventCode::UiWearMask);
    }
    if(g_printing && finished && !masked) {
        ui_message_sync((uint16_t)middleware::DeviceEventCode::UiPurifyBeforeOpen, 0);
    } else {
        ui_message_retract((uint16_t)middleware::DeviceEventCode::UiPurifyBeforeOpen);
    }

    if(!g_printing) {
        return;
    }
    ui_device_status_t st;
    if(g_purify == PURIFY_RUNNING)   st = UI_DEVICE_STATUS_PURIFYING;
    else if(g_purify == PURIFY_DONE) st = UI_DEVICE_STATUS_PURIFY_DONE;
    else switch(g_phase) {
        case PHASE_PAUSED:     st = UI_DEVICE_STATUS_PAUSED; break;
        case PHASE_TERMINATED: st = UI_DEVICE_STATUS_PRINT_TERMINATED; break;
        case PHASE_DONE:       st = UI_DEVICE_STATUS_PRINT_DONE; break;
        default:               st = UI_DEVICE_STATUS_PRINTING; break;
    }
    ui_device_state_set_status(st);
}

void purify_stage_set(purify_stage_t s)
{
    g_purify = s;
    apply_phase();
}

void apply_view(void)
{
    auto show = [](lv_obj_t *o, bool on) {
        if(!o) return;
        if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(g_view_job,    g_tab == TAB_JOB);
    show(g_view_status, g_tab == TAB_STATUS);
    show(g_view_manual, g_tab == TAB_MANUAL);
    /* 切 Tab、进打印态强制回 Job、Manual 被禁用退位都走这里：点动钮已经看不见了，
     * 长按中的轴立刻停，不等松手。 */
    if(g_tab != TAB_MANUAL) {
        manual_hold_stop();
    }
    apply_bottom();
    if(g_tab == TAB_STATUS) {
        status_update();
    }
}

void on_tab_changed(lv_event_t *e)
{
    const uint8_t idx = ui_segmented_get_active(lv_event_get_target_obj(e));
    if(idx >= TAB_COUNT) {
        return;
    }
    g_tab = static_cast<tab_t>(idx);
    apply_view();
}

/* ========================================================================== *
 *  后端回调
 * ========================================================================== */

void on_device_state(const ui_device_snapshot_t *snap)
{
    if(snap && snap->door_state_valid) {
        const bool was_outer_open = g_outer_door_open;
        g_outer_door_open = snap->outer_door_open;
        /* 套件进行中勿用传感器覆盖乐观态，避免按钮闪一下。 */
        if(!g_inner_door_busy) {
            g_inner_door_open = snap->inner_door_open;
        }
        apply_inner_door_btn();
        if(g_outer_door_open) {
            close_outer_door_dlg();
            try_resume_pending_inner_door();
        } else if(was_outer_open) {
            /* 外门从开→关：取消等外门的 Inner Door 意图；Manual 吹粉若在等关则续跑。
             * Inner Door 仍关着、弹窗等待滑开时保留 pending，否则开外门后续跑不到。 */
            g_pending_inner_door = false;
            try_resume_pending_purify();
        }
    }
    if(g_tab == TAB_STATUS) {
        status_update();
    }
}

/** 清粉回执，经 `ui_clean_state` 扇出。两路都是本页发起的（见 purify_owner），
 *  所以两路都收；别人发起的（目前没有别的发起方）一律忽略。
 *  回调已在 UI 线程，不必再 `lv_async_call`。 */
void on_clean_state_event(ui_clean_event_t ev, ui_clean_owner_t owner)
{
    if(owner != UI_CLEAN_OWNER_PRINT_RESULT && owner != UI_CLEAN_OWNER_MANUAL) {
        return;
    }
    switch(ev) {
    case UI_CLEAN_EVT_REJECTED:
        g_purify_busy = false;
        purify_stage_set(g_purify_done ? PURIFY_DONE : PURIFY_IDLE);
        ui_toast_show("Powder cleaning is busy", ui_color_danger, 1800);
        break;
    case UI_CLEAN_EVT_STOPPED:
        /* 用户中止：风扇停，按钮可再开。曾 MCU 成功过则徽标仍可留 Done（口罩提示）。 */
        g_purify_busy = false;
        purify_stage_set(g_purify_done ? PURIFY_DONE : PURIFY_IDLE);
        break;
    case UI_CLEAN_EVT_DONE:
        g_purify_busy = false;
        /* MCU 完成帧：打印结果路记成功态；Manual 回空闲可再点。 */
        if(owner == UI_CLEAN_OWNER_PRINT_RESULT) {
            g_purify_done = true;
            purify_stage_set(PURIFY_DONE);
        } else {
            purify_stage_set(PURIFY_IDLE);
        }
        break;
    case UI_CLEAN_EVT_ACCEPTED:
    default:
        break;   /* 点击/自动开时已切 RUNNING，受理无需额外动作 */
    }
}

/** 进页注册这一组；离页由 page_user_monitor_exit() 清掉。axis / device / soc_led
 *  都是单槽，调试页也在用，不清会让那边静默失联。照明钮走 soc_led，进页再 Get。 */
void wire_debug_proxy(bool on)
{
    auto &proxy = ui::DebugMsgProxy::instance();
    if(!on) {
        proxy.register_axis(nullptr);
        proxy.register_device(nullptr);
        proxy.register_soc_led(nullptr);
        proxy.register_inner_door_busy(nullptr);
        return;
    }
    proxy.register_axis([](const middleware::DebugAxisStatePayload &p) {
        if(g_plate_val) lv_label_set_text_fmt(g_plate_val, "%.1f", static_cast<double>(p.z_pos_mm));
        if(g_s_val)     lv_label_set_text_fmt(g_s_val, "%.3f mm", static_cast<double>(p.s_pos_mm));
    });
    /* 外门态主路径走 env 快照（on_device_state）；Device_Ntf 若将来有回读仍可覆盖。 */
    proxy.register_device([](const middleware::DebugDeviceStatePayload &p) {
        const bool was_outer_open = g_outer_door_open;
        g_outer_door_open = (p.door_open != 0);
        ui_device_state_set_outer_door_open(g_outer_door_open);
        if(g_outer_door_open) {
            close_outer_door_dlg();
            try_resume_pending_inner_door();
        } else if(was_outer_open) {
            g_pending_inner_door = false;
            try_resume_pending_purify();
        }
    });
    proxy.register_soc_led([](uint8_t mask) {
        apply_light_btn_from_mask(mask);
    });
    proxy.register_inner_door_busy([](bool busy) {
        set_inner_door_busy(busy);
        if(!busy) {
            /* 套件结束：用传感器校正按钮亮灭。 */
            const ui_device_snapshot_t *snap = ui_device_state_get();
            if(snap && snap->door_state_valid) {
                g_inner_door_open = snap->inner_door_open;
                apply_inner_door_btn();
            }
        }
    });
    proxy.request_soc_led_state();
    proxy.request_inner_door_busy();
}

void monitor_backend_init(void)
{
    ui_device_state_add_observer(on_device_state);
    ui_clean_state_add_observer(on_clean_state_event);

    ui::UiMsgProxy::instance().register_print_pause_done([](bool paused) {
        if(!g_printing || phase_finished()) {
            return;
        }
        g_phase = paused ? PHASE_PAUSED : PHASE_PRINTING;
        apply_phase();
    });

    ui::UiMsgProxy::instance().register_optical_path_progress(
        [](const middleware::OpticalPathProgressPayload &p) {
            page_user_monitor_optical_path_update(p.point_index, p.point_total, p.state);
        });

    ui::UiMsgProxy::instance().register_print_prep_progress(
        [](const middleware::PrintPrepProgressPayload &p) {
            page_user_monitor_print_prep_update(p.step, p.step_index, p.step_total,
                                               p.sub_index, p.sub_total);
        });

    ui::CameraMsgProxy::instance().register_snapshot_rsp(
        [](const middleware::CameraSnapshotRspPayload &rsp) {
            g_snap_pending = false;
            if(g_snap_btn) {
                lv_obj_clear_state(g_snap_btn, LV_STATE_DISABLED);
            }
            if(rsp.ok) {
                char toast[160];
                std::snprintf(toast, sizeof(toast), "Photo saved to %s", UPC_PHOTO_DIR);
                ui_toast_show(toast, user_color_accent, 1500);
            } else {
                ui_toast_show("Photo save failed", ui_color_danger, 1500);
            }
        });
}

}  // namespace

/* ========================================================================== *
 *  页面接口
 * ========================================================================== */

lv_obj_t *page_user_monitor_create(lv_obj_t *parent)
{
    /* 整屏出血：跨过 content_panel_ 的 pad_left 把摄像头铺到屏幕左边缘。
     * **纵向不用抵消 pad_top**——本页不显示常驻设备条，apply_device_bar() 已把它收成 0。 */
    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_pos(g_root, -USER_SIDEBAR_W, 0);
    lv_obj_set_size(g_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0xf5f5f5), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);

    g_cam_layer = build_cam_layer(g_root);

    /* ---- 右栏 ---- */
    g_panel = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_panel);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(g_panel, LV_HOR_RES - USER_MON_PANEL_W, 0);
    lv_obj_set_size(g_panel, USER_MON_PANEL_W, LV_VER_RES);
    lv_obj_set_style_bg_color(g_panel, lv_color_hex(0xf5f5f5), 0);
    lv_obj_set_style_bg_opa(g_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_panel, USER_PAD_LG, 0);
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g_panel, USER_PAD_LG / 2, 0);
    lv_obj_clear_flag(g_panel, LV_OBJ_FLAG_SCROLLABLE);

    static const ui_segmented_item_t kTabItems[TAB_COUNT] = {
        { "Job",    nullptr },
        { "Status", nullptr },
        { "Manual", nullptr },
    };
    ui_segmented_cfg_t tc = {};
    tc.items          = kTabItems;
    tc.count          = TAB_COUNT;
    tc.track_w        = 0;
    tc.track_h        = USER_MON_ROW_H;
    tc.track_radius   = USER_MON_RADIUS;
    tc.pill_radius    = USER_MON_RADIUS * 7 / 8;   /* 稿 14/16 */
    tc.inset          = 2;                         /* 稿 892→894 */
    tc.gap            = USER_PAD_SM / 4;           /* 稿 4，由胶囊宽 114.667 反推 */
    tc.track_color    = user_light_text;
    tc.track_opa      = 13;                        /* 稿 5% */
    tc.pill_color     = lv_color_white();
    tc.pill_opa       = LV_OPA_COVER;
    tc.color_on       = user_light_text;
    /* 未选中 / 禁用两档稿上是同色不同不透明度（60% / 20%），而分段控件只吃颜色，
     * 所以把不透明度先折进颜色里——白底上的 #333145 按 alpha 混一遍即可。
     * 禁用那档就是打印中的 Manual。 */
    tc.color_off      = lv_color_mix(user_light_text, lv_color_hex(0xf5f5f5), 153);
    tc.color_disabled = lv_color_mix(user_light_text, lv_color_hex(0xf5f5f5), 51);
    tc.font           = user_font_title();
    tc.selected       = TAB_JOB;
    g_tabs = ui_segmented_create(g_panel, &tc);
    lv_obj_add_event_cb(g_tabs, on_tab_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    g_view_job    = build_view_job(g_panel);
    g_view_status = build_view_status(g_panel);
    g_view_manual = build_view_manual(g_panel);
    g_bottom      = build_bottom(g_panel);

    monitor_backend_init();
    apply_view();
    apply_phase();
    apply_fullscreen();
    return g_root;
}

void page_user_monitor_refresh(bool /*force*/)
{
    g_page_visible = true;
    /* **侧栏保持浅色档，不开 set_sidebar_video_mode()。** 本页画面从 x=128 起，
     * 侧栏背后是页根 #f5f5f5；开整屏视频档会与稿不符。 */
    status_update();
    apply_view();
    apply_phase();
    apply_fullscreen();
    cam_ensure_running();
    wire_debug_proxy(true);
}

void page_user_monitor_exit(void)
{
    g_page_visible = false;
    g_pending_inner_door = false;
    g_pending_purify = false;
    manual_hold_stop();
    wire_debug_proxy(false);
    close_outer_door_dlg();
    close_purify_door_dlg();
    /* 打印态不停流：用户可能只是切去看一眼消息，回来不该黑一下；结果页也要停在
     * 最后一帧上。待机离页同样只 detach 控件、保持 Camera 会话——若 stop→再进
     * start，短时间会叠两封带 SCM_RIGHTS 的 Start_Rsp，曾触发 DMA fd mismatch
     * 黑屏；无 widget 时 ui_timer 自然空转，解码开销可接受。 */
    ui::CameraPreviewPlayer::instance().detach_widgets();
}

void page_user_monitor_update_camera_preview_layout(uint32_t src_w, uint32_t src_h)
{
    cam_apply_layout(src_w, src_h);
}

lv_obj_t *page_user_monitor_print_preview_parent(void)
{
    return g_cam_layer;
}

void page_user_monitor_enter_printing_mode(const control_file_t *file)
{
    if(g_printing) {
        return;
    }
    g_printing     = true;
    g_phase        = PHASE_PRINTING;
    g_purify       = PURIFY_IDLE;
    g_purify_busy  = false;
    g_purify_done  = false;
    g_pending_purify = false;
    close_purify_door_dlg();
    g_percent      = 0;
    g_snap_pending = false;
    g_prep_title[0] = '\0';

    /* 打印态强制回 Job 页：Manual 这时要禁用，停在它上面会看到一个点不动的界面。 */
    g_tab = TAB_JOB;
    if(g_tabs) {
        ui_segmented_set_active(g_tabs, TAB_JOB);
    }

    if(g_job_name) {
        lv_label_set_text(g_job_name, file && file->name[0] ? file->name : "--");
    }
    std::snprintf(g_job_label, sizeof(g_job_label), "%s",
                  file && file->name[0] ? file->name : "");

    if(g_job_bar)     lv_bar_set_value(g_job_bar, 0, LV_ANIM_OFF);
    if(g_job_percent) {
        lv_obj_remove_flag(g_job_percent, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_job_percent, "0%");
    }
    if(g_job_extra) {
        lv_label_set_text(g_job_extra, "");
        lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_job_time_val) {
        if(file) {
            char dur[32];
            ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
            lv_label_set_text(g_job_time_val, dur);
        } else {
            lv_label_set_text(g_job_time_val, "--");
        }
    }
    if(g_job_cons_val)  lv_label_set_text(g_job_cons_val, "--");
    if(g_job_cons_unit) lv_obj_add_flag(g_job_cons_unit, LV_OBJ_FLAG_HIDDEN);

    if(g_job_thumb_img && g_job_thumb_icon) {
        const bool real = file && ui_user_thumb_path_exists(file->thumb_print);
        if(real) {
            lv_image_set_src(g_job_thumb_img, file->thumb_print_src);
            lv_obj_remove_flag(g_job_thumb_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_job_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_image_set_src(g_job_thumb_img, nullptr);
            lv_obj_add_flag(g_job_thumb_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(g_job_thumb_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    thumb_progress_apply(0);

    apply_view();
    apply_phase();
    cam_ensure_running();
}

void page_user_monitor_exit_printing_mode(void)
{
    /* 人还在监控页：预览本来就要接着当待机直播，stop 再 start 会黑一下
     * （Manual OPEN 回 IDLE、打印结束回待机都走这里）。切走了只卸控件，
     * 会话由 detach 保留；真正关流留给进程退出或其它显式 stop。 */
    if(!g_page_visible) {
        ui::CameraPreviewPlayer::instance().detach_widgets();
    }
    g_printing    = false;
    g_purify_busy = false;
    g_purify_done = false;
    g_pending_purify = false;
    close_purify_door_dlg();
    g_phase       = PHASE_IDLE;
    g_purify      = PURIFY_IDLE;
    g_percent     = 0;
    g_prep_title[0] = '\0';
    close_retrieve_dlg();
    apply_phase();
    ui_device_state_set_status(UI_DEVICE_STATUS_READY);

    if(g_page_visible) {
        cam_ensure_running();
    }
}

void page_user_monitor_printing_update(const control_progress_t *p)
{
    if(!p || !g_printing || phase_finished()) {
        return;
    }
    /* 起印环境检查占着状态行与进度条，正式进度来了才让位。 */
    if(g_phase == PHASE_PREPARING) {
        return;
    }
    g_percent = p->progress_percent;
    if(g_job_bar)     lv_bar_set_value(g_job_bar, g_percent, LV_ANIM_ON);
    if(g_job_percent) lv_label_set_text_fmt(g_job_percent, "%d%%", g_percent);
    thumb_progress_apply(g_percent);

    if(g_job_extra) {
        if(p->total_layers > 0) {
            lv_label_set_text_fmt(g_job_extra, "%u/%u",
                                  static_cast<unsigned>(p->current_layer),
                                  static_cast<unsigned>(p->total_layers));
            lv_obj_remove_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_job_time_val) {
        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), p->remaining_minutes);
        lv_label_set_text(g_job_time_val, dur);
    }
}

void page_user_monitor_optical_path_update(uint8_t point_index, uint8_t point_total,
                                           uint8_t state)
{
    if(!g_printing || phase_finished()) {
        return;
    }
    if(state == static_cast<uint8_t>(middleware::OpticalPathPointState::Complete)) {
        /* 光路只是 Prepare 里的一步；后面还可能有测高/氧门/首层。
         * 切 Printing 一律等 print_prep 的 Ready，这里只清右侧 n/5。 */
        if(g_job_extra) {
            lv_label_set_text(g_job_extra, "");
            lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    /* 标题与总进度由 print_prep 驱动；此处只补右侧 n/5，并保证仍在 PREPARING。 */
    if(g_phase != PHASE_PREPARING) {
        g_phase = PHASE_PREPARING;
        apply_phase();
    }
    if(g_job_extra && point_total > 0) {
        lv_label_set_text_fmt(g_job_extra, "%u/%u",
                              static_cast<unsigned>(point_index) + 1u,
                              static_cast<unsigned>(point_total));
        lv_obj_remove_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
    }
}

void page_user_monitor_print_prep_update(uint8_t step, uint8_t step_index, uint8_t step_total,
                                         uint8_t sub_index, uint8_t sub_total)
{
    if(!g_printing || phase_finished()) {
        return;
    }

    if(step == static_cast<uint8_t>(middleware::PrintPrepStep::Ready) || step_total == 0) {
        /* Prepare 与打印是两套进度：环境检查结束回到 0%，再吃正式层进度。 */
        g_prep_title[0] = '\0';
        g_phase   = PHASE_PRINTING;
        g_percent = 0;
        if(g_job_bar) {
            lv_bar_set_value(g_job_bar, 0, LV_ANIM_OFF);
        }
        if(g_job_percent) {
            lv_obj_remove_flag(g_job_percent, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_job_percent, "0%");
        }
        thumb_progress_apply(0);
        if(g_job_extra) {
            lv_label_set_text(g_job_extra, "");
            lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
        }
        apply_phase();
        return;
    }

    const char *title = "Preparing...";
    switch(static_cast<middleware::PrintPrepStep>(step)) {
    case middleware::PrintPrepStep::OuterDoor:
        title = "Checking outer door...";
        break;
    case middleware::PrintPrepStep::InnerDoor:
        title = "Closing inner door...";
        break;
    case middleware::PrintPrepStep::OpticalPath:
        title = "Optical path check...";
        break;
    case middleware::PrintPrepStep::PlateHeight:
        title = "Plate leveling...";
        break;
    case middleware::PrintPrepStep::Oxygen:
        title = "Checking oxygen...";
        break;
    case middleware::PrintPrepStep::FirstLayer:
        title = "First layer check...";
        break;
    case middleware::PrintPrepStep::PlatePresent:
        title = "Checking build plate...";
        break;
    default:
        break;
    }
    std::snprintf(g_prep_title, sizeof(g_prep_title), "%s", title);

    int percent = 0;
    if(step_index >= 1 && step_total >= 1) {
        if(sub_total > 0) {
            /* 已完成步 + 当前步内子进度，均摊到总步数。 */
            percent = static_cast<int>(
                ((static_cast<unsigned>(step_index) - 1u) * 100u +
                 (static_cast<unsigned>(sub_index) + 1u) * 100u /
                     static_cast<unsigned>(sub_total)) /
                static_cast<unsigned>(step_total));
        } else {
            /* 步刚进入：停在该步切片起点。 */
            percent = static_cast<int>(
                (static_cast<unsigned>(step_index) - 1u) * 100u /
                static_cast<unsigned>(step_total));
        }
    }
    if(percent < 0) {
        percent = 0;
    } else if(percent > 100) {
        percent = 100;
    }
    g_percent = percent;
    g_phase   = PHASE_PREPARING;

    if(g_job_bar) {
        lv_bar_set_value(g_job_bar, g_percent, LV_ANIM_OFF);
    }
    if(g_job_percent) {
        lv_label_set_text_fmt(g_job_percent, "%d%%", g_percent);
    }
    /* Prepare 只驱动条与数字；缩略图蒙版表示打印层进度，环境检查阶段保持 0%。 */

    if(step == static_cast<uint8_t>(middleware::PrintPrepStep::OpticalPath) &&
       sub_total > 0 && g_job_extra) {
        lv_label_set_text_fmt(g_job_extra, "%u/%u",
                              static_cast<unsigned>(sub_index) + 1u,
                              static_cast<unsigned>(sub_total));
        lv_obj_remove_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
    } else if(g_job_extra) {
        lv_label_set_text(g_job_extra, "");
        lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
    }

    apply_phase();
}

void page_user_monitor_enter_print_result(const control_print_result_t *result)
{
    if(!result || !g_printing) {
        return;
    }
    /* 进结果页前记下是否还在 Prepare：终止时决定要不要自动清粉。 */
    const bool aborted_during_prep =
        result->terminated && (g_phase == PHASE_PREPARING);

    g_snap_pending = false;
    g_purify_busy  = false;
    g_purify_done  = false;
    if(result->video_dir[0]) {
        std::snprintf(g_job_label, sizeof(g_job_label), "%s", result->video_dir);
    } else {
        std::snprintf(g_job_label, sizeof(g_job_label), "%s", result->job_name);
    }
    if(g_snap_btn) {
        lv_obj_clear_state(g_snap_btn, LV_STATE_DISABLED);
    }

    /* 光路/prep 借用了 extra 位显示点位（"2/5"），跟层号同形不同义，进结果页要清掉。 */
    if(g_phase == PHASE_PREPARING && g_job_extra) {
        lv_label_set_text(g_job_extra, "");
        lv_obj_add_flag(g_job_extra, LV_OBJ_FLAG_HIDDEN);
    }
    g_prep_title[0] = '\0';

    g_phase  = result->terminated ? PHASE_TERMINATED : PHASE_DONE;
    g_purify = PURIFY_IDLE;
    if(result->terminated) {
        /* Terminated：右侧不要进度百分数，缩略图也不要蒙版进度感。 */
        g_percent = 0;
        if(g_job_percent) {
            lv_label_set_text(g_job_percent, "");
            lv_obj_add_flag(g_job_percent, LV_OBJ_FLAG_HIDDEN);
        }
        if(g_job_bar) {
            lv_bar_set_value(g_job_bar, 0, LV_ANIM_OFF);
        }
        thumb_progress_apply(100); /* 隐藏蒙版，露出完整缩略图 */
    } else {
        g_percent = 100;
        if(g_job_percent) {
            lv_obj_remove_flag(g_job_percent, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_job_percent, "100%");
        }
        if(g_job_bar) {
            lv_bar_set_value(g_job_bar, 100, LV_ANIM_OFF);
        }
        thumb_progress_apply(100);
    }

    if(g_job_time_val) {
        if(!result->has_stats) {
            lv_label_set_text(g_job_time_val, "--");
        } else {
            /* 稿上是 H:MM:SS（"2:23:23"），小时位不补零 */
            const uint32_t s = result->duration_sec;
            lv_label_set_text_fmt(g_job_time_val, "%u:%02u:%02u",
                                  s / 3600u, (s / 60u) % 60u, s % 60u);
        }
    }
    if(g_job_cons_val) {
        /* material_g 由 config.json 反查，查不到时后端写 0 → 同样按无数据处理。
         * 无数据时连单位一起藏掉，避免出现 "-- G" 这种半截读法。 */
        const bool has = result->has_stats && result->material_g > 0.0f;
        if(has) {
            lv_label_set_text_fmt(g_job_cons_val, "%.2f",
                                  static_cast<double>(result->material_g));
        } else {
            lv_label_set_text(g_job_cons_val, "--");
        }
        if(g_job_cons_unit) {
            if(has) lv_obj_remove_flag(g_job_cons_unit, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(g_job_cons_unit, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 结果出来了就回 Job 页：用户这时要看的是取件，不是环境读数。 */
    g_tab = TAB_JOB;
    if(g_tabs) {
        ui_segmented_set_active(g_tabs, TAB_JOB);
    }
    apply_view();
    apply_phase();

    /* Prepare 阶段终止：作业未真正开扫，不自动清粉。
     * Complete / 打印中终止仍自动开一轮（带延时抽帧）；钮可再点停/再开。 */
    if(aborted_during_prep) {
        return;
    }
    g_purify_busy = true;
    ui_clean_state_request(UI_CLEAN_OWNER_PRINT_RESULT, g_job_label);
    g_purify = PURIFY_RUNNING;
    apply_phase();
}

void page_user_monitor_reset_result_slide(void)
{
    if(g_retrieve_slide) {
        ui_slide_unlock_reset(g_retrieve_slide);
    }
}
