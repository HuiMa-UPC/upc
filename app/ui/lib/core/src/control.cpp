#include "control.hpp"
#include "ui_msg_proxy.hpp"
#include "camera_msg_proxy.hpp"
#include "msg_port.hpp"
#include "gcode_manifest.hpp"
#include "print_history_manifest.hpp"
#include "debug_msg_proxy.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_user_widgets.hpp"
#include "ui_slide_unlock.hpp"
#include "ui_arc_spinner.hpp"
#include "ui_xfr_dialog.hpp"
#include "ui_device_state.hpp"
#include "ui_alarm_state.hpp"
#include "popup_dialog_img.h"
#include "powder_dialog_img.h"
#include "home_metric_icons.h"

#include "page_user_monitor.hpp"
#include "page_user_files.hpp"
#include "page_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

middleware::PrintFileSource file_tab_to_source(FileTab tab)
{
    return tab == FileTab::Usb ? middleware::PrintFileSource::Usb
                               : middleware::PrintFileSource::Local;
}

/* ============================================================================
 *  状态机全局
 * ============================================================================ */

using DoneCb = void (*)(void *user_data);

lv_obj_t *g_overlay = nullptr;
control_state_t g_state = CONTROL_STATE_IDLE;
ui_modal_dialog_t *g_current_dlg = nullptr;
control_file_t g_active_file = {};

/* Camera 的 manifest 通知早于本侧进入 PRINT_DONE 时的暂存位（见 on_print_video_manifest） */
control_print_result_t g_print_result = {};
bool g_print_result_pending = false;
bool g_print_result_shown = false;
struct CommitHistoryCtx {
    char frames_dir[96];
};

void commit_print_result_history_async(void *user)
{
    std::unique_ptr<CommitHistoryCtx> ctx(static_cast<CommitHistoryCtx*>(user));
    if(!ctx || ctx->frames_dir[0] == '\0') {
        return;
    }
    ui::UiMsgProxy::instance().send_print_video_finalize(ctx->frames_dir);
}

void schedule_commit_print_result_history()
{
    if(g_print_result.video_dir[0] == '\0') {
        return;
    }
    /* 无有效 manifest 条目（如 0 层终止）时，video_dir 可能仍是 gcode 文件名；
     * 这不是 frames_dir，不能发 finalize 请求。 */
    if(std::strchr(g_print_result.video_dir, '.') != nullptr) {
        return;
    }
    auto *ctx = new CommitHistoryCtx{};
    std::snprintf(ctx->frames_dir, sizeof(ctx->frames_dir), "%s", g_print_result.video_dir);
    lv_async_call(commit_print_result_history_async, ctx);
}


/* 门流程来源：区分同一套门弹窗（DOOR_OPENING〔→TRANSFER_BOX〕）的入口与终态。
 *   PrintPrep      —— 打印前置流程，开门成功后进入选文件页；
 *   HomeStandalone —— 首页独立开门，开门成功后返回首页；
 *   PrintCollect   —— 打印结果页滑动取件，取消要退回结果页而不是待机首页。
 *   TransferDemo   —— 调试页一键演示：后台 Z→−100、关内门、开外门，再走与取件相同的转运盒三步。 */
enum class DoorFlowOrigin { PrintPrep, HomeStandalone, PrintCollect, TransferDemo };
DoorFlowOrigin g_door_origin = DoorFlowOrigin::PrintPrep;
/** 外门关着去开内门：转圈标题用 Opening the door...，其余开门入口仍是 Checking PM2.5...。 */
bool g_door_plain_opening_title = false;
/** 演示 Prep 在后台跑（无 overlay）；Rsp 到才进装盒页。只经 set_transfer_demo_pending() 改。 */
bool g_transfer_demo_prep_pending = false;
/** Prep 超时兜底。Glavo 最坏约 66 s（Z 到 −100 settle 60 s + 关内门 2 s + 开外门 3 次各 1 s 与间隔）；
 *  Rsp 丢了 pending 就永远不清——Manual 的 Run 一直灰，control_is_printing() 也一直挡着文件页。 */
constexpr uint32_t TRANSFER_DEMO_PREP_TIMEOUT_MS = 90000u;
lv_timer_t *g_transfer_demo_timer = nullptr;

/* 转运盒流程总开关（设置页 Advanced Print，调试页 Other → Print Gates 同一枚）。
 * 默认开：取件分支插入 TRANSFER_BOX 三步。关掉则取件开门成功即回 IDLE。
 * 纯 UI 本地标志，不发消息也不落盘，重启回到开。 */
bool g_transfer_box_flow = true;

/* 跳过放版预确认开关（调试页 Other → Print Gates → Skip Plate Prompt）。默认开：打印入口
 * 直接进选文件页。关掉后 control_start() 先弹「是否已放入打印版」。纯 UI 本地标志，不发消息也不落盘。 */
bool g_skip_plate_prompt = true;

/** 真实开门失败后最多再试几次（含首次共 DOOR_OPEN_MAX_ATTEMPTS 次）。
 *  须在 wire_msg_proxy 之前声明：开门 Rsp 回调 lambda 会捕获这些名字。 */
constexpr uint32_t DOOR_OPEN_MAX_RETRIES = 2u;
constexpr uint32_t DOOR_OPEN_MAX_ATTEMPTS = DOOR_OPEN_MAX_RETRIES + 1u;
uint32_t g_door_open_fail_count = 0; /**< 本轮开门失败次数（Rsp ok=0） */

/** 转运盒流程是否对当前这趟门流程生效（只认取件分支） */
bool transfer_box_flow_active(void)
{
    return g_transfer_box_flow && g_door_origin == DoorFlowOrigin::PrintCollect;
}

void on_transfer_demo_timeout(lv_timer_t *timer);

/** 改 g_transfer_demo_prep_pending 的唯一入口：起停超时定时器，值变了再通知监控页
 *  （Manual 的 Run、Plate 卡与 OPEN 按它置灰）。enter_state() 每次都会清一遍，不变就不通知。 */
void set_transfer_demo_pending(bool on)
{
    if(g_transfer_demo_timer) {
        lv_timer_delete(g_transfer_demo_timer);
        g_transfer_demo_timer = nullptr;
    }
    if(on) {
        g_transfer_demo_timer = lv_timer_create(on_transfer_demo_timeout,
                                                TRANSFER_DEMO_PREP_TIMEOUT_MS, nullptr);
        lv_timer_set_repeat_count(g_transfer_demo_timer, 1);
    }
    if(g_transfer_demo_prep_pending == on) {
        return;
    }
    g_transfer_demo_prep_pending = on;
    page_user_monitor_transfer_demo_changed();
}

/** Prep 超时：按失败收尾（同 Rsp ok=0 那支）。晚到的 Rsp 会因 pending 已清被忽略。 */
void on_transfer_demo_timeout(lv_timer_t * /*timer*/)
{
    g_transfer_demo_timer = nullptr;
    if(!g_transfer_demo_prep_pending) {
        return;
    }
    set_transfer_demo_pending(false);
    g_door_origin = DoorFlowOrigin::PrintPrep;
    ui_toast_show("Transfer demo timed out", user_color_status_err, 3000);
}

/* 选择文件页的视图代码已拆到 page_user_files.cpp，这里只留数据与流程。
 * 页面当前的选中/页签经 page_user_files_sel_index() / _source() 查询。 */
const char *g_home_section_view = "user_home"; /**< 首页区当前视图："user_home" 或 "user_files" */

ui_modal_dialog_t *g_confirm_dlg = nullptr;
ui_modal_dialog_t *g_prepare_dlg = nullptr;

/* 顶部供应度（耗材/滤芯）。默认值=后端未就绪时的占位（设计稿示例 48% 充足）。 */
middleware::SupplyStatusPayload g_supply = {48.0f, 48.0f, 1, 1};
lv_obj_t *g_supply_mat_bar = nullptr, *g_supply_mat_pct = nullptr, *g_supply_mat_status = nullptr;
lv_obj_t *g_supply_filt_bar = nullptr, *g_supply_filt_pct = nullptr, *g_supply_filt_status = nullptr;

/* 对外快照 + 观察者（见 control.hpp「供应度」一节）：Proxy 那个回调是单槽的，本文件
 * 占着它，所以别的页面只能从这里拿数据。两份值在 apply_supply() 里一处同步。 */
constexpr int kMaxSupplyObservers = 4;
control_supply_t g_supply_pub = { 48.0f, 48.0f, true, true };
control_supply_observer_t g_supply_observers[kMaxSupplyObservers] = { nullptr };

/* ---------- 后端异步状态（UiMsgProxy 适配，仅本文件使用） ---------- */
bool  g_proxy_wired = false;
bool  g_pm25_high = false;
int   g_purify_progress = 0;
bool  g_terminate_pending = false;
int   g_last_print_progress = 0;
uint16_t g_last_cmd_cache_rate = 0;
uint16_t g_last_data_cache_rate = 0;
lv_timer_t *g_print_peer_watch_timer = nullptr;
lv_timer_t *g_print_stall_timer = nullptr;
lv_timer_t *g_offline_exit_timer = nullptr;
lv_timer_t *g_terminate_timeout_timer = nullptr;

constexpr uint32_t PRINT_PEER_WATCH_MS      = 500u;
constexpr uint32_t PRINT_DRAIN_FALLBACK_MS  = 60000u;
constexpr uint32_t PRINT_OFFLINE_EXIT_MS    = 3000u;
constexpr uint32_t TERMINATE_TIMEOUT_MS     = 8000u;

DoneCb g_pm25_cb = nullptr;
void  *g_pm25_u = nullptr;
DoneCb g_open_cb = nullptr;
void  *g_open_u = nullptr;
DoneCb g_purify_cb = nullptr;
void  *g_purify_u = nullptr;
DoneCb g_clean_cb = nullptr;
void  *g_clean_u = nullptr;
DoneCb g_clean_stop_cb = nullptr;
void  *g_clean_stop_u = nullptr;
bool   g_cleaning_active = false;
bool   g_clean_stop_pending = false;

using ProgressCb = void (*)(const control_progress_t *p, void *user_data);
ProgressCb g_job_prog_cb = nullptr;
DoneCb     g_job_done_cb = nullptr;
void      *g_job_u = nullptr;

void (*g_on_action_fail)(const char *message) = nullptr;

std::vector<control_file_t> g_files;

/** USB Fs_DirOpen 请求队列：Glavo 按 FIFO 应答，弹出项须与当前 Tab 一致才刷新 UI */
std::deque<FileTab> g_dir_open_pending;

void show_terminated_result_without_manifest(void);


std::string lvgl_file_src_from_path(const char* path)
{
    if(!path || path[0] == '\0') {
        return {};
    }
    if(std::strncmp(path, "A:/", 3) == 0) {
        return std::string(path);
    }
    if(path[0] == '/') {
        return std::string("A:") + path;
    }
    return std::string("A:/") + path;
}

/* 目录数据到达：换掉列表，并**按文件名**把选中项接回去。
 * 按名字而不是按下标，是因为后端重排或增删之后同一个下标可能已经是别的文件了。 */
void apply_files_list(const std::vector<control_file_t>& files)
{
    const int prev_sel = page_user_files_sel_index();
    char prev_name[64] = {0};
    const bool had_sel = (prev_sel >= 0 &&
                          prev_sel < static_cast<int>(g_files.size()));
    if(had_sel) {
        std::snprintf(prev_name, sizeof(prev_name), "%s", g_files[prev_sel].name);
    }

    g_files = files;

    int sel = -1;
    if(had_sel) {
        for(size_t i = 0; i < g_files.size(); i++) {
            if(std::strcmp(g_files[i].name, prev_name) == 0) {
                sel = static_cast<int>(i);
                g_active_file = g_files[i];
                break;
            }
        }
    }
    page_user_files_set_selection(sel);

    if(page_user_files_active()) {
        page_user_files_rebuild();
    }
}

/** Local：UI 直接读 /userdata/Gcode/config.json（不经 Fs_DirOpen IPC） */
void load_local_files_from_config()
{
    std::vector<upc::GcodeManifestEntry> entries;
    if(!upc::gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, entries)) {
        entries.clear();
    }

    std::vector<control_file_t> files;
    files.reserve(entries.size());
    for(size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        control_file_t f = {};
        f.index = static_cast<uint32_t>(i);
        std::snprintf(f.name, sizeof(f.name), "%s", e.name.c_str());
        std::snprintf(f.path, sizeof(f.path), "%s/%s", UPC_LOCAL_GCODE_DIR, e.name.c_str());
        std::snprintf(f.thumb_small, sizeof(f.thumb_small), "%s", e.thumb_small.c_str());
        std::snprintf(f.thumb_large, sizeof(f.thumb_large), "%s", e.thumb_large.c_str());
        std::snprintf(f.thumb_print, sizeof(f.thumb_print), "%s", e.thumb_print.c_str());
        const std::string small_src = lvgl_file_src_from_path(f.thumb_small);
        const std::string large_src = lvgl_file_src_from_path(f.thumb_large);
        const std::string print_src = lvgl_file_src_from_path(f.thumb_print);
        std::snprintf(f.thumb_small_src, sizeof(f.thumb_small_src), "%s", small_src.c_str());
        std::snprintf(f.thumb_large_src, sizeof(f.thumb_large_src), "%s", large_src.c_str());
        std::snprintf(f.thumb_print_src, sizeof(f.thumb_print_src), "%s", print_src.c_str());
        f.duration_min = e.duration_sec / 60u;
        f.material_g   = e.material_g;
        f.file_size    = e.file_size;
        f.line_count   = e.line_count;
        files.push_back(f);
    }
    apply_files_list(files);
}

void request_fs_dir_open(FileTab tab)
{
    if(tab == FileTab::Local) {
        load_local_files_from_config();
        return;
    }
    g_dir_open_pending.push_back(FileTab::Usb);
    ui::UiMsgProxy::instance().send_fs_dir_open(middleware::PrintFileSource::Usb);
}

void enter_state(control_state_t s);
void refresh_supply_bars(void);
void apply_supply(const middleware::SupplyStatusPayload &p);
void close_confirm_dlg(void);
void show_print_confirm_dialog(void);
void open_user_confirm_dialog(lv_obj_t *parent, const char *title, const char *body,
                              const char *cancel_text, const char *confirm_text,
                              lv_color_t confirm_color, bool light,
                              lv_event_cb_t cancel_cb, lv_event_cb_t confirm_cb);
void close_prepare_dlg(void);
void show_prepare_dlg(void);
void goto_file_selection(void);
lv_obj_t *make_round_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb);
lv_obj_t *make_dlg_wide_button(lv_obj_t *parent, const char *text, bool enabled,
                               lv_event_cb_t cb);

/* 放版弹窗 header 的返回键与关闭键。
 *
 * 稿 6382:48451 / 48694 上这两枚是**光秃的深墨字形**——SVG 里那两个 32×32 的圆角矩形
 * 填的是 white，压在白卡上就是纯触摸目标、看不见。所以这里底透明、只留字形，按下时
 * 才浮出一层 10% 深墨；先前那套「深灰实心圆 + 白字形」是深色档的，翻白卡后会变成白底
 * 上突兀的两颗黑扣子。命中区仍是 USER_BACK_BTN_H 见方，比稿的 32 大，触摸更稳。
 *
 * 选文件页那枚返回钮已随页面搬到 page_user_files.cpp——新稿把它改成了 72 见方、
 * 圆角 24 的浅色卡片，与这里不是同一个东西，故两边各留一份。 */
lv_obj_t *make_round_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_BACK_BTN_H, USER_BACK_BTN_H);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, user_light_text, 0);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);
    lv_obj_center(icon);
    return btn;
}

void on_powder_low_async(void *user);
void on_print_start_rejected_async(void *user);
void on_outer_door_confirm_req_async(void *user);
void on_plate_place_confirm_req_async(void *user);
void close_powder_dlg(void);
void close_outer_door_confirm_dlg(void);
void close_plate_place_confirm_dlg(void);
void stop_print_watchdogs(void);
void start_print_peer_watch(void);
void finish_printing(bool notify_complete);
void try_exit_print_preview(bool notify_complete);
void arm_drain_fallback_timer(void);
void cancel_drain_fallback_timer(void);
void arm_offline_exit_timer(void);
void on_print_peer_watch(lv_timer_t *timer);
void on_print_drain_fallback(lv_timer_t *timer);
void on_offline_exit(lv_timer_t *timer);
void on_terminate_timeout(lv_timer_t *timer);
void arm_door_open_retry_timer(void);
void stop_door_timers(void);
void pf_on_clean_stop_done(void *u);
void on_print_video_manifest(const middleware::PrintVideoManifestPayload &p);

void invoke_done(DoneCb &cb, void *&user)
{
    DoneCb fn = cb;
    void *u = user;
    cb = nullptr;
    user = nullptr;
    if(fn) {
        fn(u);
    }
}

const std::vector<control_file_t> &active_files()
{
    return g_files;
}

void copy_file_entry(control_file_t &dst, const middleware::FileEntryPayload &src)
{
    std::strncpy(dst.name, src.name, sizeof(dst.name) - 1);
    dst.name[sizeof(dst.name) - 1] = '\0';
    std::strncpy(dst.path, src.path, sizeof(dst.path) - 1);
    dst.path[sizeof(dst.path) - 1] = '\0';
    dst.thumb_small[0] = '\0';
    dst.thumb_large[0] = '\0';
    dst.thumb_print[0] = '\0';
    dst.thumb_small_src[0] = '\0';
    dst.thumb_large_src[0] = '\0';
    dst.thumb_print_src[0] = '\0';
    dst.index        = src.index;
    dst.duration_min = src.duration_min;
    dst.material_g   = src.material_g;
    dst.file_size    = src.file_size;
    dst.line_count   = src.line_count;
}

static bool mcu_cache_drained(uint16_t cmd_rate, uint16_t data_rate)
{
    return cmd_rate == 0 && data_rate == 0;
}

static bool print_ready_to_exit(void)
{
    return g_last_print_progress >= 99;
}

void cancel_drain_fallback_timer(void)
{
    if(g_print_stall_timer) {
        lv_timer_delete(g_print_stall_timer);
        g_print_stall_timer = nullptr;
    }
}

void arm_drain_fallback_timer(void)
{
    if(g_print_stall_timer) {
        return;
    }
    g_print_stall_timer = lv_timer_create(on_print_drain_fallback,
        PRINT_DRAIN_FALLBACK_MS, nullptr);
    lv_timer_set_repeat_count(g_print_stall_timer, 1);
}

void arm_offline_exit_timer(void)
{
    if(g_offline_exit_timer) {
        return;
    }
    g_offline_exit_timer = lv_timer_create(on_offline_exit,
        PRINT_OFFLINE_EXIT_MS, nullptr);
    lv_timer_set_repeat_count(g_offline_exit_timer, 1);
}

void try_exit_print_preview(bool notify_complete)
{
    if(g_state != CONTROL_STATE_PRINTING || g_terminate_pending) {
        return;
    }
    if(!print_ready_to_exit()) {
        return;
    }
    if(!mcu_cache_drained(g_last_cmd_cache_rate, g_last_data_cache_rate)) {
        arm_drain_fallback_timer();
        return;
    }
    cancel_drain_fallback_timer();
    finish_printing(notify_complete);
}

void stop_print_watchdogs(void)
{
    if(g_print_peer_watch_timer) {
        lv_timer_delete(g_print_peer_watch_timer);
        g_print_peer_watch_timer = nullptr;
    }
    if(g_print_stall_timer) {
        lv_timer_delete(g_print_stall_timer);
        g_print_stall_timer = nullptr;
    }
    if(g_terminate_timeout_timer) {
        lv_timer_delete(g_terminate_timeout_timer);
        g_terminate_timeout_timer = nullptr;
    }
    if(g_offline_exit_timer) {
        lv_timer_delete(g_offline_exit_timer);
        g_offline_exit_timer = nullptr;
    }
}

void finish_printing(bool notify_complete)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        stop_print_watchdogs();
        g_terminate_pending = false;
        return;
    }

    const bool user_terminate = g_terminate_pending;
    stop_print_watchdogs();
    g_terminate_pending = false;
    g_last_print_progress = 0;
    g_last_cmd_cache_rate = 0;
    g_last_data_cache_rate = 0;
    /* 缺粉窗可能还开着（用户既没补粉也没取消，作业已被终止/跑完） */
    close_powder_dlg();
    close_outer_door_confirm_dlg();
    close_plate_place_confirm_dlg();

    if(notify_complete && g_job_done_cb) {
        DoneCb done_cb = g_job_done_cb;
        void *done_u = g_job_u;
        g_job_prog_cb = nullptr;
        g_job_done_cb = nullptr;
        g_job_u = nullptr;
        done_cb(done_u);
        return;
    }

    g_job_prog_cb = nullptr;
    g_job_done_cb = nullptr;
    g_job_u = nullptr;
    /* 不回 IDLE：先进 PRINT_DONE 保活摄像头预览，等 Camera 出片后切结果页 */
    enter_state(CONTROL_STATE_PRINT_DONE);
    if(user_terminate) {
        show_terminated_result_without_manifest();
    }
}

void on_print_peer_watch(lv_timer_t * /*timer*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        stop_print_watchdogs();
        return;
    }
    if(middleware::MsgPort::instance().peer_connected(middleware::ModuleId::APP_GLAVO)) {
        return;
    }
    if(g_terminate_pending) {
        finish_printing(false);
        return;
    }
    if(print_ready_to_exit() &&
       !mcu_cache_drained(g_last_cmd_cache_rate, g_last_data_cache_rate)) {
        arm_offline_exit_timer();
        return;
    }
    finish_printing(false);
}

void on_offline_exit(lv_timer_t * /*timer*/)
{
    g_offline_exit_timer = nullptr;
    if(g_state == CONTROL_STATE_PRINTING) {
        finish_printing(g_last_print_progress >= 100);
    }
}

void on_print_drain_fallback(lv_timer_t * /*timer*/)
{
    g_print_stall_timer = nullptr;
    if(g_state == CONTROL_STATE_PRINTING && print_ready_to_exit()) {
        finish_printing(g_last_print_progress >= 100);
    }
}

void on_terminate_timeout(lv_timer_t * /*timer*/)
{
    g_terminate_timeout_timer = nullptr;
    if(g_state == CONTROL_STATE_PRINTING && g_terminate_pending) {
        finish_printing(false);
    }
}

void start_print_peer_watch(void)
{
    stop_print_watchdogs();
    g_last_print_progress = 0;
    g_last_cmd_cache_rate = 0;
    g_last_data_cache_rate = 0;
    g_print_peer_watch_timer = lv_timer_create(on_print_peer_watch, PRINT_PEER_WATCH_MS, nullptr);
}

/* ---------- WiFi 传输：PIN 配对码弹窗 ----------
 * 配对是网络事件，任何时刻都可能来，所以挂 lv_layer_top() 而不是 g_overlay
 * （后者在 IDLE 态是隐藏的），打印中也照弹。
 * 只能手动关：XfrPairPinPayload.expires_s 暂不使用，过期不会自动收窗。 */
ui_xfr_pin_dialog_t *g_xfr_pin_dlg = nullptr;
ui_xfr_recv_dialog_t *g_xfr_recv_dlg = nullptr;
char g_xfr_recv_name[128] = {0};

bool xfr_progress_state_active(uint8_t state)
{
    return state == static_cast<uint8_t>(middleware::XfrTransferState::Uploading) ||
           state == static_cast<uint8_t>(middleware::XfrTransferState::Slicing);
}

void close_xfr_pin_dlg(void)
{
    if(g_xfr_pin_dlg) {
        ui_xfr_pin_dialog_close(g_xfr_pin_dlg);
        g_xfr_pin_dlg = nullptr;
    }
}

void close_xfr_recv_dlg(void)
{
    if(g_xfr_recv_dlg) {
        ui_xfr_recv_dialog_close(g_xfr_recv_dlg);
        g_xfr_recv_dlg = nullptr;
    }
    g_xfr_recv_name[0] = '\0';
}

/* 只关 UI。取消配对目前没有契约，后端配对会话仍然存活，手机此时依然能配对成功。 */
void on_xfr_pin_cancel(void * /*user*/)
{
    close_xfr_pin_dlg();
}

/* 只关 UI。取消传输目前没有契约，后端上传/切片任务不受影响。 */
void on_xfr_recv_cancel(void * /*user*/)
{
    close_xfr_recv_dlg();
}

void on_transport_result(uint8_t result);

void wire_msg_proxy()
{
    if(g_proxy_wired) {
        return;
    }
    g_proxy_wired = true;
    auto &proxy = ui::UiMsgProxy::instance();

    proxy.register_pm25_result([](const middleware::Pm25ResultPayload &p) {
        g_pm25_high = p.is_high != 0;
        invoke_done(g_pm25_cb, g_pm25_u);
    });

    proxy.register_open_door_result([](const middleware::DoorResultPayload &p) {
        DoneCb cb = g_open_cb;
        void *u = g_open_u;
        g_open_cb = nullptr;
        g_open_u = nullptr;
        if(g_state != CONTROL_STATE_DOOR_OPENING) {
            return;
        }
        if(!p.ok) {
            ++g_door_open_fail_count;
            if(g_door_open_fail_count < DOOR_OPEN_MAX_ATTEMPTS) {
                arm_door_open_retry_timer();
                return;
            }
            /* 传感器无效或门一直未开：停转圈，回退入口态，避免无限轮询。 */
            g_door_open_fail_count = 0;
            stop_door_timers();
            ui_toast_show("Door did not open", user_color_status_err, 3000);
            if(g_door_origin == DoorFlowOrigin::PrintCollect) {
                enter_state(CONTROL_STATE_PRINT_DONE);
                page_user_monitor_reset_result_slide();
            } else if(g_door_origin == DoorFlowOrigin::PrintPrep) {
                goto_file_selection();
            } else {
                enter_state(CONTROL_STATE_IDLE);
            }
            return;
        }
        g_door_open_fail_count = 0;
        /* 开门成功：乐观记下外门已开，Manual 内门/吹粉不必再等下一帧 env */
        ui_device_state_set_outer_door_open(true);
        if(cb) {
            cb(u);
        }
    });

    proxy.register_purify_progress([](int progress) {
        g_purify_progress = progress;
    });

    proxy.register_purify_done([]() {
        g_purify_progress = 100;
        invoke_done(g_purify_cb, g_purify_u);
    });

    proxy.register_clean_start_result([]() {
        invoke_done(g_clean_cb, g_clean_u);
    });

    proxy.register_clean_stop_result([]() {
        invoke_done(g_clean_stop_cb, g_clean_stop_u);
    });

    /* 自动转运：result=0 → BoxShutter；非 0 停留 BoxMoving（无超时） */
    proxy.register_transport_result(on_transport_result);

    /* Prep：后台 Z→−100 → 关内门 → 开外门。成功才弹装盒页；Done 后再发转运。 */
    proxy.register_transfer_demo_prep([](const middleware::DoorResultPayload &p) {
        if(!g_transfer_demo_prep_pending ||
           g_door_origin != DoorFlowOrigin::TransferDemo) {
            return;
        }
        set_transfer_demo_pending(false);
        if(!p.ok) {
            g_door_origin = DoorFlowOrigin::PrintPrep;
            ui_toast_show("Transfer demo failed", user_color_status_err, 3000);
            return;
        }
        ui_device_state_set_outer_door_open(true);
        enter_state(CONTROL_STATE_TRANSFER_BOX);
    });

    /* USB 列表经 Fs_DirOpen IPC；Local 由 load_local_files_from_config 直读盘 */
    proxy.register_fs_dir([](const middleware::FileEntryPayload *files, uint32_t count) {
        if(g_dir_open_pending.empty()) {
            return;
        }
        g_dir_open_pending.pop_front();
        if(page_user_files_source() != FileTab::Usb) {
            return;
        }

        std::vector<control_file_t> next;
        if(files && count > 0) {
            next.reserve(count);
            for(uint32_t i = 0; i < count; i++) {
                control_file_t f = {};
                copy_file_entry(f, files[i]);
                next.push_back(f);
            }
        }
        apply_files_list(next);
    });

    proxy.register_fs_delete_rsp([](const middleware::FsDeleteRspPayload &p) {
        if(static_cast<middleware::PrintFileSource>(p.file_source) !=
           middleware::PrintFileSource::Local) {
            return;
        }
        if(p.ok) {
            if(page_user_files_source() == FileTab::Local) {
                request_fs_dir_open(FileTab::Local);
            }
            return;
        }
        const auto reason = static_cast<middleware::FsDeleteResult>(p.reason);
        if(reason == middleware::FsDeleteResult::BusyPrinting) {
            ui_toast_show("Cannot delete while printing", user_color_status_err, 2200);
        } else {
            ui_toast_show("Delete failed", user_color_status_err, 2200);
        }
    });

    proxy.register_supply_status([](const middleware::SupplyStatusPayload &p) {
        apply_supply(p);
    });

    proxy.register_print_prepare_done([](bool ok) {
        if(g_state != CONTROL_STATE_PREPARE_WAIT) {
            return;
        }
        close_prepare_dlg();
        if(!ok) {
            ui_toast_show("Prepare failed", user_color_status_err, 2500);
            enter_state(CONTROL_STATE_IDLE);
            return;
        }
        /* 打印版/舱门预确认已在文件选择之前完成，预处理成功即开始打印 */
        enter_state(CONTROL_STATE_PRINTING);
    });

    proxy.register_print_progress([](const middleware::PrintProgressPayload &p) {
        g_last_print_progress   = p.progress_percent;
        g_last_cmd_cache_rate   = p.cmd_cache_rate;
        g_last_data_cache_rate  = p.data_cache_rate;
        if(g_job_prog_cb) {
            control_progress_t pr = {};
            pr.progress_percent  = p.progress_percent;
            pr.remaining_minutes = p.remaining_minutes;
            pr.pm25_indoor       = p.pm25_indoor;
            pr.pm25_cabin        = p.pm25_cabin;
            pr.temperature_c     = p.temperature_c;
            pr.oxygen_percent    = p.oxygen_percent;
            pr.current_layer     = p.current_layer;
            pr.total_layers      = p.total_layers;
            g_job_prog_cb(&pr, g_job_u);
        }
        if(p.progress_percent < 99) {
            cancel_drain_fallback_timer();
            return;
        }
        try_exit_print_preview(p.progress_percent >= 100);
    });

    /* 缺粉：后端已自动暂停（另发 Pause_Rsp 让徽标翻到 PAUSE），这里只弹补粉窗 */
    proxy.register_print_powder_low([]() {
        lv_async_call(on_powder_low_async, nullptr);
    });

    proxy.register_print_start_rejected([](const middleware::PrintStartRejectedPayload&) {
        lv_async_call(on_print_start_rejected_async, nullptr);
    });

    proxy.register_outer_door_confirm_req([]() {
        lv_async_call(on_outer_door_confirm_req_async, nullptr);
    });

    proxy.register_plate_place_confirm_req([]() {
        lv_async_call(on_plate_place_confirm_req_async, nullptr);
    });

    proxy.register_print_terminate_done([]() {
        const bool user_terminate = g_terminate_pending;
        if(user_terminate) {
            /* 用户终止：立即退预览，不等双缓冲 */
            finish_printing(false);
            return;
        }
        if(g_state != CONTROL_STATE_PRINTING) {
            return;
        }
        /* 自然完成也会发 Terminate_Rsp：须与 Progress 一致，双缓冲为 0 才退 */
        try_exit_print_preview(true);
    });

    /* Camera 出片 + manifest 落盘 → 打印结果页（契约见 common/COMMON.md 消息总表） */
    ui::CameraMsgProxy::instance().register_print_video_manifest(
        [](const middleware::PrintVideoManifestPayload &p) {
            on_print_video_manifest(p);
        });

    proxy.register_xfr_pair_pin([](const middleware::XfrPairPinPayload& p) {
        /* 后端重发 PIN（续期或新会话）时先收掉旧窗，避免叠成两层 */
        close_xfr_pin_dlg();
        g_xfr_pin_dlg = ui_xfr_pin_dialog_open(lv_layer_top(), p.pin,
                                               on_xfr_pin_cancel, nullptr);
    });

    proxy.register_xfr_file_received([](const middleware::XfrFileReceivedPayload& p) {
        close_xfr_recv_dlg();
        char msg[180];
        std::snprintf(msg, sizeof(msg), "File received: %s", p.file_name);
        ui_toast_show(msg, user_color_status_ok, 2200);
        if(page_user_files_active() && page_user_files_source() == FileTab::Local) {
            request_fs_dir_open(FileTab::Local);
        }
    });

    proxy.register_xfr_transfer_progress([](const middleware::XfrTransferProgressPayload& p) {
        const bool active = xfr_progress_state_active(p.state);
        if(active) {
            if(!g_xfr_recv_dlg || std::strcmp(g_xfr_recv_name, p.file_name) != 0) {
                close_xfr_recv_dlg();
                std::snprintf(g_xfr_recv_name, sizeof(g_xfr_recv_name), "%s", p.file_name);
                g_xfr_recv_dlg = ui_xfr_recv_dialog_open(lv_layer_top(), g_xfr_recv_name,
                                                         on_xfr_recv_cancel, nullptr);
            }
            ui_xfr_recv_dialog_set_progress(g_xfr_recv_dlg,
                                            static_cast<int32_t>(p.progress_percent));
            return;
        }

        if(g_xfr_recv_dlg) {
            ui_xfr_recv_dialog_set_progress(g_xfr_recv_dlg,
                                            static_cast<int32_t>(p.progress_percent));
            close_xfr_recv_dlg();
        }
        if(p.state == static_cast<uint8_t>(middleware::XfrTransferState::Error)) {
            ui_toast_show("Transfer failed", user_color_status_err, 2200);
        }
    });

    proxy.add_usb_hotplug_listener([](const middleware::UsbHotplugPayload& p) {
        if(!page_user_files_active() || page_user_files_source() != FileTab::Usb) {
            return;
        }
        g_dir_open_pending.clear();
        if(p.mounted != 0) {
            request_fs_dir_open(FileTab::Usb);
            return;
        }
        apply_files_list({});
        ui_toast_show("USB removed", user_color_status_err, 1800);
    });

}

void backend_reset_session()
{
    g_pm25_high = false;
    g_purify_progress = 0;
    page_user_files_reset();
    close_xfr_recv_dlg();
    ui::UiMsgProxy::instance().send_supply_status();
}

void backend_cancel_pending()
{
    g_pm25_cb = nullptr;
    g_pm25_u = nullptr;
    g_open_cb = nullptr;
    g_open_u = nullptr;
    g_purify_cb = nullptr;
    g_purify_u = nullptr;
    g_clean_cb = nullptr;
    g_clean_u = nullptr;
    g_clean_stop_cb = nullptr;
    g_clean_stop_u = nullptr;
    g_job_prog_cb = nullptr;
    g_job_done_cb = nullptr;
    g_job_u = nullptr;
}

void backend_start_pm25(DoneCb cb, void *u)
{
    g_pm25_cb = cb;
    g_pm25_u = u;
    ui::UiMsgProxy::instance().send_pm25_check();
}

void backend_start_clean(DoneCb cb, void *u)
{
    g_clean_cb = cb;
    g_clean_u = u;
    ui::UiMsgProxy::instance().send_clean_start();
}

void backend_start_clean_stop(DoneCb cb, void *u)
{
    g_clean_stop_cb = cb;
    g_clean_stop_u = u;
    ui::UiMsgProxy::instance().send_clean_stop();
}

void backend_start_open_door(DoneCb cb, void *u)
{
    g_open_cb = cb;
    g_open_u = u;
    ui::UiMsgProxy::instance().send_open_door();
}

size_t backend_file_count()
{
    return active_files().size();
}

const control_file_t *backend_get_file(size_t index)
{
    const auto &files = active_files();
    return index < files.size() ? &files[index] : nullptr;
}

void backend_start_job(const control_file_t *file, ProgressCb prog_cb, DoneCb done_cb, void *u)
{
    g_job_prog_cb = prog_cb;
    g_job_done_cb = done_cb;
    g_job_u = u;

    const uint32_t file_index = file ? file->index : 0;
    ui::UiMsgProxy::instance().send_print_start(
        file_index, file_tab_to_source(page_user_files_source()));
}

void backend_terminate_job()
{
    g_terminate_pending = true;
    if(!middleware::MsgPort::instance().peer_connected(middleware::ModuleId::APP_GLAVO)) {
        finish_printing(false);
        return;
    }
    ui::UiMsgProxy::instance().send_print_terminate();
    if(g_terminate_timeout_timer) {
        lv_timer_delete(g_terminate_timeout_timer);
    }
    g_terminate_timeout_timer = lv_timer_create(on_terminate_timeout,
        TERMINATE_TIMEOUT_MS, nullptr);
    lv_timer_set_repeat_count(g_terminate_timeout_timer, 1);
}

/* ============================================================================
 *  overlay / 弹窗管理
 * ============================================================================ */

void overlay_show(void)
{
    if(!g_overlay) {
        return;
    }
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_overlay);
}

void overlay_hide(void)
{
    if(g_overlay) {
        lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void close_current_dialog(void)
{
    if(g_current_dlg) {
        ui_modal_dialog_close(g_current_dlg);
        g_current_dlg = nullptr;
    }
    close_confirm_dlg();
    close_prepare_dlg();
    /* 注意：选择文件页（见 page_user_files.cpp）现由 PageManager 托管、常驻不销毁，
     * 这里不再删除/清空其句柄。 */
}

/* ============================================================================
 *  打印前预确认弹窗（Figma 1.1.x）：
 *    - 宽入口弹窗（g_plate_*）：是否已放入打印版 → 已放入 / 未放入 子视图
 *    - 窄舱门弹窗（g_door_*） ：Opening / Cleaning（PM2.5 超标时）；开门成功即收
 *  公共小部件：Mask required 徽标、缩放插画。
 * ============================================================================ */

/* ---- 宽入口弹窗：持久句柄（header 常驻，仅切换 content 子视图） ---- */
lv_obj_t *g_plate_content  = nullptr;   /**< 子视图内容容器（choose/inserted/notinserted 重建） */
lv_obj_t *g_plate_back_btn = nullptr;   /**< 返回圆钮（choose 视图隐藏） */

/* ---- 舱门弹窗：持久句柄（对齐 Figma 6388:49816/49425/50202：顶部满宽大图 + 内容列） ---- */
lv_obj_t         *g_door_img_box = nullptr;  /**< 顶部满宽图容器（切 variant 时换图） */
lv_obj_t         *g_door_content = nullptr;  /**< 下方 padded 内容列（切 variant 时重建） */
lv_obj_t         *g_door_title   = nullptr;
lv_obj_t         *g_door_body    = nullptr;  /**< [D] 状态副行（PM2.5 净化提示） */
ui_arc_spinner_t *g_door_spinner = nullptr;
/* 门流程节奏：让 Opening / Cleaning 中间态"看得见"——避免后端即时应答导致一闪而过。
 *   - Opening / Cleaning：最短停留后再前进；开门成功即结束，不再等关门。 */
constexpr uint32_t DOOR_OPENING_MIN_MS = 300u;    /**< "正在准备开门"最短显示 */
constexpr uint32_t CLEAN_MIN_MS        = 300u;    /**< "正在清粉中"最短显示 */
constexpr uint32_t DOOR_OPEN_RETRY_MS  = 300u;    /**< 开门失败重试间隔 */
/* 转运盒：装盒到位仍用 BOX_READY_MS 占位；转移完成等 APP_Transport_Result_Ntf（result=0），
 * 失败(1/2)停留转圈页、无超时。闸门那步稿上本就没有到位检测，一直是用户自己点。 */
constexpr uint32_t BOX_READY_MS        = 1500u;   /**< 「转运盒未到位」翻可点的占位时长 */
/* 门弹窗顶图的设计尺寸：卡片宽定比例，图容器高按此比算 */
constexpr int32_t DOOR_IMG_W = 496;
constexpr int32_t DOOR_IMG_H = 321;
lv_timer_t *g_door_advance_timer    = nullptr;    /**< Opening/Cleaning 最短停留到时→前进 */
lv_timer_t *g_door_open_retry_timer = nullptr;    /**< Opening 开门失败重试定时器 */
uint32_t    g_door_opening_since     = 0;          /**< 进入 Opening 的 tick */
uint32_t    g_clean_since            = 0;          /**< 进入 Cleaning 的 tick */
bool        g_box_ready              = false;      /**< BoxInstall 的按钮是否已翻成可点 */

/* 口罩提示徽标（深色半透明胶囊 + 红色"圆圈 i"图标 + 红字）已随新稿删除。
 * 门弹窗三态、转运盒三步、放版弹窗「未放入」原先各挂一枚，新稿一律没有它——
 * 口罩要求改为写进标题下那行灰字说明。 */

/** 插画缩放到显示宽 target_w 后的显示高（与 make_illustration 同一套整数取整，
 *  供需要"跟插图等高/底边对齐"的兄弟控件复用，避免两处算出差 1px 的高度）。 */
int32_t illustration_height(const lv_image_dsc_t *dsc, int32_t target_w)
{
    const int32_t nat_w = (int32_t)dsc->header.w;
    const int32_t nat_h = (int32_t)dsc->header.h;
    const int32_t scale = nat_w > 0 ? (256 * target_w) / nat_w : 256;
    return nat_h * scale / 256;
}

/** 缩放插画：把 master 位图（dsc）等比缩放到显示宽 target_w，装进等大的盒子。 */
lv_obj_t *make_illustration(lv_obj_t *parent, const lv_image_dsc_t *dsc, int32_t target_w)
{
    const int32_t nat_w = (int32_t)dsc->header.w;
    const int32_t nat_h = (int32_t)dsc->header.h;
    const int32_t scale = nat_w > 0 ? (256 * target_w) / nat_w : 256;
    const int32_t w = nat_w * scale / 256;
    const int32_t h = nat_h * scale / 256;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_clip_corner(box, false, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    /* 插图不拦截点击：使卡片(按钮)整块可点，而不是只有文字区域。 */
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *im = lv_image_create(box);
    lv_image_set_src(im, dsc);
    lv_image_set_pivot(im, 0, 0);
    lv_image_set_scale(im, scale);
    lv_obj_set_pos(im, 0, 0);
    lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

/* -------------------------- 宽入口弹窗子视图 -------------------------- */

void plate_show_choose(void);          /* choose：两卡片 */
void plate_show_inserted(void);        /* 已放入：选择打印任务 */
void plate_show_notinserted(void);     /* 未放入：滑动开门 */

void on_plate_back(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_PLATE_PROMPT) {
        plate_show_choose();
    }
}

/* 右上角 "X"：取消预确认，回到首页（enter_state 统一关弹窗/隐藏遮罩） */
void on_plate_close(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_PLATE_PROMPT) {
        enter_state(CONTROL_STATE_IDLE);
    }
}

void on_plate_pick_inserted(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_PLATE_PROMPT) {
        plate_show_inserted();
    }
}

void on_plate_pick_notinserted(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_PLATE_PROMPT) {
        plate_show_notinserted();
    }
}

/* 已放入 → 选择打印任务 */
void on_plate_select_task(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_PLATE_PROMPT) {
        goto_file_selection();
    }
}

/* 未放入 → 滑动开门解锁 */
void on_plate_swipe_open(void * /*u*/)
{
    if(g_state == CONTROL_STATE_PLATE_PROMPT) {
        g_door_origin = DoorFlowOrigin::PrintPrep;
        g_door_plain_opening_title = false;
        enter_state(CONTROL_STATE_DOOR_OPENING);
    }
}

/* ---- 放版弹窗的稿几何（6382:48451 / 48694 与 6388:48910，原生 1280×800）----
 * 稿值（1280 档）：
 *   卡 808｜内卡 760（左右各留 24）｜插图 320×272（距内卡左上各 24）
 *   右列 360（插图右侧留 32，列右侧留 24）｜内卡圆角 16｜插图圆角 8
 *
 * **只有卡宽是查表的（USER_PLATE_DLG_W），插图与右列都按剩余空间现算。**
 * 不按 808 的比例折算，是因为内距（USER_PAD / USER_PAD_LG）三档并非等比——1024 档的
 * PAD_LG 反而比 1280 大，见 ui_theme.cpp 头部「间距梯度」。比例折算只在 1280 档对得上，
 * 另两档会和 flex 实际分配的宽度差二十来像素，插图顶出卡片、滑块顶出列外。 */
int32_t plate_dialog_width(void) { return USER_PLATE_DLG_W; }

/**
 * 卡内插图的显示宽（稿 320@808）。
 *
 * **按 choose 视图那张卡的可用内宽算，不按 808 的比例折算。** 两处的内距
 * （`USER_PAD`）三档并非等比，比例折算只在 1280 档对得上：1024 档算出 255 而卡内
 * 只有 246、1920 档算出 480 而卡内只有 466，插图会顶出卡片。choose 的卡宽由
 * `flex_grow` 平分，所以这里照它的公式反推：
 *   卡外宽 = (弹窗内宽 − 两卡间距) / 2，插图 = 卡外宽 − 卡内距×2
 * 1280 档代入正好回到稿值 320。
 */
int32_t plate_img_w(void)
{
    const int32_t body_w    = USER_PLATE_DLG_W - USER_PAD * 2;   /* 弹窗内宽 */
    const int32_t card_outer = (body_w - USER_PAD) / 2;          /* 两卡平分，间距 USER_PAD */
    return card_outer - USER_PAD * 2;                            /* 扣掉卡自身内距 */
}

/**
 * 单卡右侧文字/操作列的宽（稿 360@808）。
 *
 * 同样按剩余空间算：那一列在布局上是 `flex_grow(1)` 撑出来的，公式必须和 flex
 * 实际分配的一致，否则滑块按它建出来会顶出列外。
 */
int32_t plate_right_col_w(void)
{
    /* 卡宽 − 弹窗内距×2 − 内卡内距×2 − 插图 − 图↔列间距 */
    return USER_PLATE_DLG_W - USER_PAD * 4 - plate_img_w() - USER_PAD_LG;
}
/** 内卡圆角（稿 16@1280 = 浅色圆角的 2/3）；插图圆角是它的一半（稿 8）。 */
int32_t plate_card_radius(void) { return USER_LIGHT_RADIUS * 2 / 3; }

/** 内卡底：稿上三张分别是 10% / 5% / 4%，按多数派统一取 5%。 */
void plate_style_inner_card(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(card, 13, 0);          /* 0.05 × 255 ≈ 13 */
    lv_obj_set_style_radius(card, plate_card_radius(), 0);
    lv_obj_set_style_border_width(card, 0, 0);
}

/** 插图 + 稿上那圈 8px 圆角。位图是不透明矩形，圆角靠容器裁而不是靠 alpha。 */
lv_obj_t *plate_make_image(lv_obj_t *parent, const lv_image_dsc_t *img)
{
    lv_obj_t *box = make_illustration(parent, img, plate_img_w());
    lv_obj_set_style_radius(box, plate_card_radius() / 2, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    return box;
}

/* 单张大卡片（已放入/未放入）：左插画 + 右文字/操作列。稿上内卡 760×320，
 * 插图与右列都贴着 24 的内距，右列底边与插图底边齐（稿上按钮/滑块底边都落在 378）。 */
lv_obj_t *plate_make_single_card(const lv_image_dsc_t *img)
{
    lv_obj_t *card = lv_obj_create(g_plate_content);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    plate_style_inner_card(card);
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_set_style_pad_column(card, USER_PAD_LG, 0);   /* 稿：插图↔右列 32 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    plate_make_image(card, img);
    return card;
}

/* choose 子视图：两张可点卡片（我已放入 / 还未放入）。稿上每张 368×369，
 * 内含 320×272 的插图 + 下方一行标签，两张之间 24。 */
lv_obj_t *plate_make_choice_card(const lv_image_dsc_t *img,
                                 const char *label, lv_event_cb_t cb)
{
    lv_obj_t *card = lv_button_create(g_plate_content);
    lv_obj_remove_style_all(card);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(card, 1);
    plate_style_inner_card(card);
    lv_obj_set_style_bg_opa(card, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_set_style_pad_row(card, USER_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, nullptr);

    plate_make_image(card, img);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_dlg_btn(), 0);
    return card;
}

void plate_show_choose(void)
{
    if(!g_plate_content) {
        return;
    }
    if(g_plate_back_btn) {
        lv_obj_add_flag(g_plate_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(g_plate_content);
    lv_obj_set_flex_flow(g_plate_content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_plate_content, USER_PAD, 0);   /* 稿：两卡间距 24 */
    lv_obj_set_style_pad_row(g_plate_content, 0, 0);

    plate_make_choice_card(&img_plate_loaded, "Plate Loaded",
                           on_plate_pick_inserted);
    plate_make_choice_card(&img_plate_empty, "Not Loaded Yet",
                           on_plate_pick_notinserted);
}

/* 右文字/操作列（已放入 / 未放入 共用骨架） */
lv_obj_t *plate_make_text_col(lv_obj_t *card)
{
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, USER_PAD, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

void plate_show_inserted(void)
{
    if(!g_plate_content) {
        return;
    }
    if(g_plate_back_btn) {
        lv_obj_clear_flag(g_plate_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(g_plate_content);
    lv_obj_set_flex_flow(g_plate_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(g_plate_content, 0, 0);

    lv_obj_t *card = plate_make_single_card(&img_plate_loaded);
    lv_obj_t *col = plate_make_text_col(card);

    /* 稿 6382:48694：右列与插图等高，文字贴顶、按钮贴底（稿上按钮与插图底边都落在
     * y=378）。这里给显式像素高而不是 LV_PCT(100)——卡片高是 LV_SIZE_CONTENT、由插图
     * 撑出来的，子对象再按百分比取父高会形成循环依赖。
     * 宽度不用给：plate_make_text_col() 的 flex_grow(1) 会把剩余横向空间全吃掉。 */
    lv_obj_set_height(col, illustration_height(&img_plate_loaded, plate_img_w()));

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, "Plate Loaded");
    lv_obj_set_style_text_color(t, user_light_text, 0);
    lv_obj_set_style_text_font(t, user_font_dlg_title(), 0);

    lv_obj_t *s = lv_label_create(col);
    lv_label_set_text(s, "Tap the button below to choose a print file");
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_style_text_color(s, user_light_text, 0);
    lv_obj_set_style_text_opa(s, LV_OPA_50, 0);
    lv_obj_set_style_text_font(s, user_font_text_sm(), 0);

    /* 撑开，把按钮顶到列底。吃掉全部剩余空间后，plate_make_text_col 里的主轴 CENTER
     * 也就不再起作用。 */
    lv_obj_t *sp = lv_obj_create(col);
    lv_obj_remove_style_all(sp);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);

    /* 稿 6382:48694 的按钮是 360×72、圆角 12，即满列宽。走宽弹窗通用主按钮。 */
    make_dlg_wide_button(col, "Select Print Job", true, on_plate_select_task);
}

void plate_show_notinserted(void)
{
    if(!g_plate_content) {
        return;
    }
    if(g_plate_back_btn) {
        lv_obj_clear_flag(g_plate_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(g_plate_content);
    lv_obj_set_flex_flow(g_plate_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(g_plate_content, 0, 0);

    lv_obj_t *card = plate_make_single_card(&img_plate_empty);
    lv_obj_t *col = plate_make_text_col(card);

    lv_obj_set_height(col, illustration_height(&img_plate_empty, plate_img_w()));

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, "Not Loaded Yet");
    lv_obj_set_style_text_color(t, user_light_text, 0);
    lv_obj_set_style_text_font(t, user_font_dlg_title(), 0);

    /* 口罩要求并进这句灰字，稿上不再有单独的口罩徽标（同门弹窗那一族）。 */
    lv_obj_t *s = lv_label_create(col);
    lv_label_set_text(s, "Slide to open the door and load the plate. "
                         "Please wear a mask.");
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_style_text_color(s, user_light_text, 0);
    lv_obj_set_style_text_opa(s, LV_OPA_50, 0);
    lv_obj_set_style_text_font(s, user_font_text_sm(), 0);

    /* 同「已放入」：撑开把滑块顶到列底，稿上滑块底边与插图底边齐（都在 y=378）。 */
    lv_obj_t *sp = lv_obj_create(col);
    lv_obj_remove_style_all(sp);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);

    /* 稿 6388:48910 的滑块：轨道 360×94 贴满右列，旋钮 84 的圆角方块（rx20）上下内缩 5。
     * 这三个数正是浅色档的 USER_HOME_SLIDE_H / USER_ACTION_BTN_SZ ——控件的 light 档本身
     * 就是这套稿（深墨实底 user_light_nav_on、圆角 USER_LIGHT_RADIUS、无玻璃高光），
     * 所以这里只要选 UI_SLIDE_THEME_LIGHT，不需要再给 custom_accent。 */
    ui_slide_unlock_cfg_t slide_cfg = {};
    slide_cfg.hint_text    = "SLIDE TO UNLOCK";   /* 箭头由控件绘制 3 个 chevron */
    slide_cfg.on_unlocked  = on_plate_swipe_open;
    slide_cfg.user_data    = nullptr;
    slide_cfg.track_w      = plate_right_col_w();
    slide_cfg.track_h      = USER_HOME_SLIDE_H;
    slide_cfg.knob_sz      = USER_ACTION_BTN_SZ;
    slide_cfg.theme        = UI_SLIDE_THEME_LIGHT;
    slide_cfg.knob_icon    = ui_user_pick_icon(ic_door_panel_set,
                                               USER_ACTION_BTN_SZ * 8 / 21);
    ui_slide_unlock_create(col, &slide_cfg);
}

/* 宽入口弹窗：backdrop + 宽卡片 + 常驻 header（返回/标题/关闭）+ content。
 * 稿 6382:48451（choose）/ 48694（已放入）/ 6388:48910（未放入），三视图共用本外壳。 */
ui_modal_dialog_t *show_plate_prompt(lv_obj_t *parent)
{
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user(nullptr, plate_dialog_width(), false);
    cfg.light_theme = true;
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }
    /* 收紧标题上下间距：缩小卡片顶内边距与 header↔content 间距 */
    if(dlg->card) {
        lv_obj_set_style_pad_top(dlg->card, USER_PAD_SM, 0);
    }
    if(dlg->body) {
        lv_obj_set_style_pad_row(dlg->body, USER_PAD_SM, 0);
    }

    /* header 行：返回钮（choose 隐藏）+ 标题列（flex_grow）+ 关闭钮 */
    lv_obj_t *header = lv_obj_create(dlg->body);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, USER_PAD, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    g_plate_back_btn = make_round_icon_btn(header, LV_SYMBOL_LEFT, on_plate_back);
    lv_obj_add_flag(g_plate_back_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *tcol = lv_obj_create(header);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_flex_grow(tcol, 1);
    lv_obj_set_height(tcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tcol, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(tcol, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(tcol);
    lv_label_set_text(title, "Is Print Plate Inserted?");
    lv_obj_set_style_text_color(title, user_light_text, 0);
    lv_obj_set_style_text_font(title, user_font_dlg_title(), 0);

    lv_obj_t *close = make_round_icon_btn(header, LV_SYMBOL_CLOSE, on_plate_close);
    (void)close;

    /* content 容器：内容自适应高度，由子视图重建 */
    g_plate_content = lv_obj_create(dlg->body);
    lv_obj_remove_style_all(g_plate_content);
    lv_obj_set_width(g_plate_content, LV_PCT(100));
    lv_obj_set_height(g_plate_content, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(g_plate_content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_plate_content, LV_OBJ_FLAG_SCROLLABLE);

    plate_show_choose();
    return dlg;
}

/* -------------------------- 舱门弹窗（Figma 6388:49816/49425/50202） -------------------------- */

/* Opening/Cleaning 是开门两态（Cleaning 无稿，与 Opening 同形只换标题）；
 * Box* 是取件分支插入的转运盒三步（稿 6357:39444 / 39504 / 39470），共用同一个门弹窗骨架，
 * 只换顶图与内容列。开门成功即收 overlay，不再有 DoorOpened/关门等待。 */
enum class DoorVariant { Opening, Cleaning,
                         BoxInstall, BoxMoving, BoxShutter };

void finish_after_door_open(void);
void on_door_opening_cancel(lv_event_t *e);
void on_box_install_confirm(lv_event_t *e);
void on_box_shutter_done(lv_event_t *e);
void door_apply_variant(DoorVariant v);
void on_transport_result(uint8_t result);
void pf_start_open_door(void);

/* 满宽圆角矩形主按钮（宽弹窗底部：门弹窗的确认钮、放版弹窗"已放入"的选择任务钮、
 * 三张粉料弹窗的主按钮）。满列宽、高 USER_DLG_FOOTER_BTN_H。
 *
 * 圆角取 `USER_LIGHT_RADIUS / 2`（三档 18/12/9）而不是 USER_BTN_RADIUS（三档 12/8/6）：
 * 新稿上这批按钮是 `rx=12`@1280，而 USER_BTN_RADIUS 在该档是 8。半个浅色圆角正好是
 * 稿值，也是工程里既有的「1280 档的 12」写法（同设置页面板条目）。
 *
 * **配色走新稿的深墨实色，不再是 accent 蓝 + 外发光**：新弹窗稿（6357/6382/6388 一族）
 * 里所有主按钮都是 #333145 平涂、无光晕，压在白卡上。三张粉料弹窗共用本函数，会跟着
 * 一起变——它们本来就是白卡，蓝按钮反倒是那一族里唯一的深色档残留。
 *
 * enabled=false → 整枚（含文字）opa 30 且不可点。稿上置灰态是给整组加 `opacity 0.3`
 * （见 image15 的 `<g opacity="0.3">`），文字跟着一起淡；先前只淡背景、白字仍是满
 * 不透明，压在淡底上比可点态还扎眼。 */
lv_obj_t *make_dlg_wide_button(lv_obj_t *parent, const char *text, bool enabled,
                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(btn, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(!enabled) {
        lv_obj_set_style_opa(btn, LV_OPA_30, 0);
    }
    if(enabled && cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, text);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, user_font_dlg_btn(), 0);
    lv_obj_center(bl);
    return btn;
}

/* 顶图：把源图等比铺满 g_door_img_box（宽 = 卡片宽）。
 * 取两轴放大倍数的较大者并向上取整：五张顶图都是 DOOR_IMG_W×DOOR_IMG_H（生成器统一
 * 铺成这个尺寸），两轴同倍数，取较大者是为了容下将来尺寸不齐的图——按宽定倍会在矮
 * 一两像素的图底边漏出一条容器底色，按高补齐则只是横向多出零点几像素、由父容器裁掉。 */
void door_set_image(const lv_image_dsc_t *img)
{
    if(!g_door_img_box) {
        return;
    }
    lv_obj_clean(g_door_img_box);
    const int32_t bw = USER_DLG_W;
    const int32_t bh = bw * DOOR_IMG_H / DOOR_IMG_W;
    const int32_t nat_w = (int32_t)img->header.w;
    const int32_t nat_h = (int32_t)img->header.h;
    const int32_t sx = nat_w > 0 ? (256 * bw + nat_w - 1) / nat_w : 256;
    const int32_t sy = nat_h > 0 ? (256 * bh + nat_h - 1) / nat_h : 256;
    const int32_t scale = sx > sy ? sx : sy;
    lv_obj_t *im = lv_image_create(g_door_img_box);
    lv_image_set_src(im, img);
    lv_image_set_pivot(im, 0, 0);
    lv_image_set_scale(im, scale);
    lv_obj_set_pos(im, 0, 0);
    lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
}

/* 舱门弹窗：自建 backdrop + 卡片（顶部满宽大图 + 下方 padded 内容列）。
 * 不用 ui_modal_dialog（其四周内边距做不出满宽顶图），仅复用 ui_modal_dialog_t
 * 承载 root，以沿用现有关闭逻辑。 */
ui_modal_dialog_t *show_door_dialog(lv_obj_t *parent, DoorVariant variant)
{
    auto *dlg = static_cast<ui_modal_dialog_t *>(std::calloc(1, sizeof(ui_modal_dialog_t)));
    if(!dlg) {
        return nullptr;
    }
    g_door_img_box = nullptr;
    g_door_content = nullptr;
    g_door_title   = nullptr;
    g_door_body    = nullptr;
    g_door_spinner = nullptr;

    /* 背景遮罩（吞点击，防穿透底层首页）。浅色档同 ui_modal_dialog：深墨 28% 而非黑 70%。 */
    lv_obj_t *backdrop = lv_obj_create(parent);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, user_light_text, 0);
    lv_obj_set_style_bg_opa(backdrop, USER_LIGHT_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    dlg->root = backdrop;

    /* 卡片：白底、无内边距（顶图满宽出血），overflow 裁剪保证上圆角。
     * 圆角走浅色档的 24（稿值），不再是深色档的 USER_DLG_RADIUS。 */
    const int32_t card_w = USER_DLG_W;
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    /* 顶部满宽图容器（高 = 宽 × 321/496，齐卡片左右上边缘，圆角随卡片裁剪）。
     * 底色给白：五张顶图都是不透明满幅照片，这层只在换图的一帧里露出来。 */
    g_door_img_box = lv_obj_create(card);
    lv_obj_remove_style_all(g_door_img_box);
    lv_obj_set_size(g_door_img_box, card_w, card_w * DOOR_IMG_H / DOOR_IMG_W);
    lv_obj_set_style_bg_color(g_door_img_box, user_light_card, 0);
    lv_obj_set_style_bg_opa(g_door_img_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_door_img_box, LV_OBJ_FLAG_SCROLLABLE);

    /* 下方 padded 内容列（标题/副标题/按钮由 variant 重建） */
    g_door_content = lv_obj_create(card);
    lv_obj_remove_style_all(g_door_content);
    lv_obj_set_width(g_door_content, LV_PCT(100));
    lv_obj_set_height(g_door_content, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(g_door_content, USER_PAD, 0);
    lv_obj_set_style_pad_row(g_door_content, USER_PAD, 0);
    lv_obj_set_style_bg_opa(g_door_content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_door_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_door_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_door_content, LV_OBJ_FLAG_SCROLLABLE);

    /* 右上角那枚 X 没了：新稿把开门态的退出做成了内容列末行的满宽 Cancel；
     * 转运盒三步本来就不给退出口。 */

    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(backdrop, 180, 0);
    lv_obj_move_foreground(backdrop);

    door_apply_variant(variant);
    return dlg;
}

/* 弹窗标题（门三态 / 转运盒三步共用）。返回 title 句柄，文案由调用方 set_text。
 *
 * **新稿把口罩徽标整个去掉了**（6388:49816/49425/50202 与 6357 一族都没有它），
 * 口罩要求并进了灰字说明行——「门已开启」那张写的是 "Wear a mask and load the print
 * plate"。原先这里是「标题 + 徽标」成组、徽标文案分三种由调用方传入，现在退化成
 * 一个标题标签，`make_mask_badge()` 在门这一族里已无调用方。 */
lv_obj_t *door_make_title(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_color(title, user_light_text, 0);
    lv_obj_set_style_text_font(title, user_font_dlg_title(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    return title;
}

/* 内容列末行的转圈：Opening/Cleaning 与转运盒「正在转移工件」共用。 */
void door_make_spinner(lv_obj_t *parent)
{
    lv_obj_t *sp_box = lv_obj_create(parent);
    lv_obj_remove_style_all(sp_box);
    lv_obj_set_size(sp_box, USER_SPINNER_SZ, USER_SPINNER_SZ);
    lv_obj_set_style_bg_opa(sp_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sp_box, LV_OBJ_FLAG_SCROLLABLE);
    g_door_spinner = ui_arc_spinner_create(sp_box, USER_SPINNER_SZ);
    if(g_door_spinner && g_door_spinner->root) {
        lv_obj_center(g_door_spinner->root);
    }
}

/* 标题下方的灰字说明行（居中换行）。稿上是深墨压到 50%，不是另给一个灰色——
 * 与三张粉料弹窗的副标题同一档。 */
lv_obj_t *door_make_sub_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_color(sub, user_light_text, 0);
    lv_obj_set_style_text_opa(sub, LV_OPA_50, 0);
    lv_obj_set_style_text_font(sub, user_font_text_sm(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sub, text);
    return sub;
}

/* 切换舱门弹窗形态（Opening/Cleaning + 转运盒三步）：
 * 换顶图 + 重建内容列。骨架共用：满宽顶图 + 标题 +（灰字说明）+ 末行按钮或转圈。 */
void door_apply_variant(DoorVariant v)
{
    if(!g_door_img_box || !g_door_content) {
        return;
    }
    if(g_door_spinner) {
        ui_arc_spinner_destroy(g_door_spinner);
        g_door_spinner = nullptr;
    }
    lv_obj_clean(g_door_content);
    g_door_title = nullptr;
    g_door_body  = nullptr;

    if(v == DoorVariant::Opening || v == DoorVariant::Cleaning) {
        door_set_image(&img_door_opening);

        g_door_title = door_make_title(g_door_content);
        /* 开门等待和 PM2.5 检测共用这一态。首页 / 取件 / 放版标题写成空气检测，
         * 避免把停留看成门开得慢。外门关着去开内门时改回 Opening the door...。
         * 超标清粉仍走 Cleaning；清完回到本态时沿用进入时的那句。 */
        const char *opening_title = g_door_plain_opening_title
                                        ? "Opening the door..."
                                        : "Checking PM2.5...";
        lv_label_set_text(g_door_title,
                          v == DoorVariant::Cleaning ? "Cleaning powder..."
                                                     : opening_title);

        door_make_spinner(g_door_content);

        /* 状态副行（PM2.5 净化提示；默认空）。稿上这张只有标题+转圈+取消钮，没画这一行，
         * 但它是取消净化时唯一的「正在停风扇」反馈（见 on_door_opening_cancel），留着；
         * 空串时 label 不占高，无稿态下看不出差别。 */
        g_door_body = door_make_sub_label(g_door_content, "");

        /* 稿把右上角那枚 X 换成了满宽取消钮。语义不变，仍是 on_door_opening_cancel：
         * 净化中先停风扇，取件分支退回结果页，其余回 IDLE。 */
        make_dlg_wide_button(g_door_content, "Cancel", true, on_door_opening_cancel);
        return;
    }

    /* 转运盒三步（稿 6357:39444 / 39504 / 39470）。
     * 稿把原先的「装盒」与「抽隔断板」合并成了一步，两件事在同一张图上用四枚外向箭头
     * 一起表达，所以这里只有三档而不是四档。 */
    if(v == DoorVariant::BoxInstall) {
        door_set_image(&img_box_install);
        g_door_title = door_make_title(g_door_content);
        lv_label_set_text(g_door_title, "Install the box and pull out the divider");
        door_make_sub_label(g_door_content,
                            "Tap the button below once the box is in place");
        /* 到位与否只改按钮：未到位置灰不可点。g_box_ready 由 BOX_READY_MS 占位定时器
         * 翻起（后端出到位信号后改由轮询翻）。 */
        make_dlg_wide_button(g_door_content,
                             g_box_ready ? "Done" : "Not Ready",
                             g_box_ready, on_box_install_confirm);
    } else if(v == DoorVariant::BoxMoving) {
        door_set_image(&img_box_moving);
        g_door_title = door_make_title(g_door_content);
        lv_label_set_text(g_door_title, "Transferring the part to the box");
        door_make_sub_label(g_door_content,
                            "Do not detach the device during transfer");
        /* 稿上这步无按钮也无关闭钮：等 MCU 转运结果 result=0 才前进 */
        door_make_spinner(g_door_content);
    } else {
        door_set_image(&img_box_shutter);
        g_door_title = door_make_title(g_door_content);
        lv_label_set_text(g_door_title, "Push the shutter in by hand");
        door_make_sub_label(g_door_content,
                            "Tap done once the shutter is pushed in");
        /* 闸门到位无检测（稿上两版插图按钮都是可点的），用户自己点 */
        make_dlg_wide_button(g_door_content, "Unlock and Remove the Box",
                             true, on_box_shutter_done);
    }
}

/* -------------------- 缺粉弹窗（Figma 4273:5892） -------------------- */

/* 稿上是「顶部满宽示意图 + 标题 + 副标题 + 主按钮 + 副按钮」，几何与门弹窗同一套
 * （496 宽卡片 + 496×321 顶图），所以复用 show_door_dialog 的骨架，只换内容列。
 * 顶图在稿里也还是「粉舱安装示意」占位，等设计出图后按 door_set_image 那样接。 */
ui_modal_dialog_t *g_powder_dlg = nullptr;
lv_obj_t *g_powder_resume_btn = nullptr;
/** 当前开着的是三档里的哪一张，决定「取消」的语义与主按钮是否受粉量门控。 */
control_supply_dlg_t g_supply_kind = CONTROL_SUPPLY_DLG_POWDER_EMPTY;

void on_powder_refilled(lv_event_t *e);
void on_powder_cancel(lv_event_t *e);

bool powder_resume_ready(uint8_t powder_bits)
{
    return (powder_bits & 0x03u) >= 0x01u;
}

void set_powder_resume_btn_enabled(bool enabled)
{
    if(!g_powder_resume_btn) {
        return;
    }
    if(enabled) {
        lv_obj_clear_state(g_powder_resume_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(g_powder_resume_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(g_powder_resume_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(g_powder_resume_btn, LV_STATE_DISABLED);
        lv_obj_remove_flag(g_powder_resume_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(g_powder_resume_btn, LV_OPA_30, 0);
    }
}

void refresh_powder_resume_btn_state(void)
{
    const ui_device_snapshot_t *snap = ui_device_state_get();
    const bool ready = snap ? powder_resume_ready(snap->powder_bits) : false;
    set_powder_resume_btn_enabled(ready);
}

void on_powder_state_update(const ui_device_snapshot_t * /*snap*/)
{
    /* 只有缺粉那档的主按钮跟着粉量位走；两个满盒弹窗不受它影响，别把按钮灰掉。 */
    if(g_powder_dlg && g_supply_kind == CONTROL_SUPPLY_DLG_POWDER_EMPTY) {
        refresh_powder_resume_btn_state();
    }
}

/* 满宽圆角副按钮：与 make_dlg_wide_button 同尺寸同圆角，深墨 10% 底 + 深墨字。
 *
 * 唯一调用方是三张粉料弹窗的「Cancel」，而那三张是**白卡**。先前这里用的是全工程
 * 深色档次按钮那套「4% 白底 + 白字」，压在白卡上底和字都看不见——整枚按钮是隐形的。
 * 新稿的次按钮（6423:51893 的 Cancel）正是 `#333145 @10%` 底配同色字，改用它。 */
lv_obj_t *make_dlg_wide_ghost_button(lv_obj_t *parent, const char *text,
                                     lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(btn, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, 0);   /* 稿 #333145 @10% */
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, text);
    lv_obj_set_style_text_color(bl, user_light_text, 0);
    lv_obj_set_style_text_font(bl, user_font_dlg_btn(), 0);
    lv_obj_center(bl);
    return btn;
}

void close_powder_dlg(void)
{
    if(g_powder_dlg) {
        ui_modal_dialog_close(g_powder_dlg);
        g_powder_dlg = nullptr;
        g_powder_resume_btn = nullptr;
        g_supply_kind = CONTROL_SUPPLY_DLG_POWDER_EMPTY;
    }
}

/* 三个粉料弹窗共用一张表。三态只差顶图与三句文案，骨架完全相同（稿
 * 6345:31129 / 6343:30738 / 6343:30854 逐像素同构：496×632 白卡 + 496×321 满宽顶图
 * + 标题 + 副标题 + 满宽主按钮 + 满宽副按钮）。 */
struct SupplyDlgSpec {
    const lv_image_dsc_t *img;
    const char *title;
    const char *body;
    const char *primary;
};
const SupplyDlgSpec kSupplyDlgs[] = {
    { &img_powder_empty,   "Powder bin is empty",
      "Refill the powder bin to continue.",       "Refilled, resume printing" },
    { &img_powder_recycle, "Recycled powder box is full",
      "Empty the recycled powder box to continue.", "Replaced, resume printing" },
    { &img_powder_waste,   "Waste powder box is full",
      "Empty the waste powder box to continue.",  "Emptied, resume printing" },
};

/* 弹窗仅挂在打印预览区域内，不占用全局顶层：这样切到其它页面仍可互动。
 *
 * **配色走浅色档**（白卡 + 深色标题），与门弹窗那一族的深色骨架不同——这三张有新稿
 * （白卡），门弹窗还没有。等门弹窗也改稿了再统一。 */
void show_supply_dialog(control_supply_dlg_t kind)
{
    if(g_powder_dlg) {
        return;
    }
    lv_obj_t *parent = page_user_monitor_print_preview_parent();
    if(!parent) {
        return;
    }
    auto *dlg = static_cast<ui_modal_dialog_t *>(std::calloc(1, sizeof(ui_modal_dialog_t)));
    if(!dlg) {
        return;
    }
    const SupplyDlgSpec &spec = kSupplyDlgs[kind];

    /* 遮罩同其余弹窗的浅色档：深墨 28%，不是黑 70%。卡片本来就是白的，遮罩先前还留着
     * 深色档那档浓度，两者对不上。 */
    lv_obj_t *backdrop = lv_obj_create(parent);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, user_light_text, 0);
    lv_obj_set_style_bg_opa(backdrop, USER_LIGHT_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    dlg->root = backdrop;

    const int32_t card_w = USER_DLG_W;
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_LIGHT_RADIUS, 0);   /* 稿 24 */
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_shadow_width(card, 60, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    /* 顶部满宽示意图（496×321，与门弹窗同框）。缩放沿用 door_set_image 那套
     * 「取两轴倍数的较大者并向上取整」，源图就是稿的原生尺寸，1280 档正好 1:1。 */
    lv_obj_t *img_box = lv_obj_create(card);
    lv_obj_remove_style_all(img_box);
    lv_obj_set_size(img_box, card_w, card_w * DOOR_IMG_H / DOOR_IMG_W);
    lv_obj_set_style_bg_color(img_box, user_light_card, 0);
    lv_obj_set_style_bg_opa(img_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(img_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *im = lv_image_create(img_box);
    lv_image_set_src(im, spec.img);
    lv_image_set_pivot(im, 0, 0);
    {
        const int32_t bh = card_w * DOOR_IMG_H / DOOR_IMG_W;
        const int32_t sx = (256 * card_w + DOOR_IMG_W - 1) / DOOR_IMG_W;
        const int32_t sy = (256 * bh + DOOR_IMG_H - 1) / DOOR_IMG_H;
        lv_image_set_scale(im, static_cast<uint32_t>(sx > sy ? sx : sy));
    }
    lv_obj_set_pos(im, 0, 0);
    lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content, USER_DLG_PAD, 0);
    lv_obj_set_style_pad_row(content, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, spec.title);
    lv_obj_set_style_text_color(title, user_light_text, 0);
    lv_obj_set_style_text_font(title, user_font_dlg_title(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *sub = lv_label_create(content);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_text(sub, spec.body);
    lv_obj_set_style_text_color(sub, user_light_text, 0);
    lv_obj_set_style_text_opa(sub, LV_OPA_50, 0);
    lv_obj_set_style_text_font(sub, user_font_text_sm(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    g_powder_resume_btn = make_dlg_wide_button(content, spec.primary, true,
                                               on_powder_refilled);
    make_dlg_wide_ghost_button(content, "Cancel", on_powder_cancel);
    g_supply_kind = kind;
    /* 只有缺粉那档按实时粉量门控主按钮；两个满盒没有对应的传感器位，一律可点。 */
    if(kind == CONTROL_SUPPLY_DLG_POWDER_EMPTY) {
        refresh_powder_resume_btn_state();
    }

    g_powder_dlg = dlg;
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(backdrop, 180, 0);
    lv_obj_move_foreground(backdrop);
}

/* 缺粉时后端已自行暂停，这里只负责提示与「补完了，继续」的入口。 */
void show_powder_dialog(void)
{
    show_supply_dialog(CONTROL_SUPPLY_DLG_POWDER_EMPTY);
}

/* 「我已补粉/替换/倾倒，继续打印」：缺粉那档要求实时粉量达到阈值（powder_bits>=1）
 * 才放行；两个满盒没有对应的传感器位，只能信用户。 */
void on_powder_refilled(lv_event_t * /*e*/)
{
    if(g_supply_kind == CONTROL_SUPPLY_DLG_POWDER_EMPTY) {
        refresh_powder_resume_btn_state();
        const ui_device_snapshot_t *snap = ui_device_state_get();
        if(!snap || !powder_resume_ready(snap->powder_bits)) {
            return;
        }
    }
    close_powder_dlg();
    /* 恢复请求发不出去时明确告知：作业仍停在后端的缺粉暂停态，需要重试，
     * 不能静默关窗让用户以为已经继续打印。 */
    if(!ui::UiMsgProxy::instance().send_print_resume()) {
        ui_toast_show("Resume failed, please retry", user_color_status_err, 2200);
    }
}

/* 「取消」只关窗、保持当前暂停态。缺粉那档另外通知后端「本次打印忽略后续缺粉检测」，
 * 两个满盒没有对应的忽略指令（也还没有上报指令），所以只关窗。 */
void on_powder_cancel(lv_event_t * /*e*/)
{
    const bool was_powder = (g_supply_kind == CONTROL_SUPPLY_DLG_POWDER_EMPTY);
    close_powder_dlg();
    if(was_powder) {
        ui::UiMsgProxy::instance().send_print_powder_low_ignore();
    }
}

/* Powder_Low 从总线线程来，建窗必须回到 LVGL 线程 */
void on_powder_low_async(void * /*user*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    show_powder_dialog();
}

/** 光路检测失败等起印拒绝：走用户终止同一条路径（无 Camera manifest 的终止页）。 */
void on_print_start_rejected_async(void * /*user*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    g_terminate_pending = true;
    finish_printing(false);
}

/* ---- 起印前外门确认（无超时；Confirm 再检门，Cancel 跳过外门检测） ---- */
ui_modal_dialog_t *g_outer_door_confirm_dlg = nullptr;

void close_outer_door_confirm_dlg(void)
{
    if(g_outer_door_confirm_dlg) {
        ui_modal_dialog_close(g_outer_door_confirm_dlg);
        g_outer_door_confirm_dlg = nullptr;
    }
}

void on_outer_door_confirm_ok(lv_event_t * /*e*/)
{
    close_outer_door_confirm_dlg();
    ui::UiMsgProxy::instance().send_outer_door_confirm(false);
}

void on_outer_door_confirm_cancel(lv_event_t * /*e*/)
{
    close_outer_door_confirm_dlg();
    ui::UiMsgProxy::instance().send_outer_door_confirm(true);
}

void show_outer_door_confirm_dlg(void)
{
    if(g_outer_door_confirm_dlg) {
        return;
    }
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Close the outer door", USER_DLG_W, true);
    /* 与弹窗新稿一族一致：白卡 + 深墨主按钮（不用 accent 蓝）。 */
    cfg.light_theme = true;
    cfg.with_close_btn = false;
    g_outer_door_confirm_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_outer_door_confirm_dlg) {
        return;
    }
    lv_obj_t *sub = ui_user_make_body_label(
        g_outer_door_confirm_dlg->body,
        "Close the outer door fully, then confirm to continue print prep.");
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    /* Confirm 在上、Cancel 在下；列布局时关掉 flex_grow，避免按钮被拉高。 */
    lv_obj_set_flex_flow(g_outer_door_confirm_dlg->footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_outer_door_confirm_dlg->footer, USER_PAD, 0);
    lv_obj_set_flex_align(g_outer_door_confirm_dlg->footer, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ok = ui_user_make_confirm_footer_btn(
        g_outer_door_confirm_dlg->footer, "Confirm",
        true, user_light_nav_on, true,
        on_outer_door_confirm_ok, nullptr);
    lv_obj_set_flex_grow(ok, 0);
    lv_obj_set_width(ok, LV_PCT(100));
    lv_obj_t *cancel = ui_user_make_confirm_footer_btn(
        g_outer_door_confirm_dlg->footer, "Cancel",
        false, user_light_nav_on, true,
        on_outer_door_confirm_cancel, nullptr);
    lv_obj_set_flex_grow(cancel, 0);
    lv_obj_set_width(cancel, LV_PCT(100));
}

void on_outer_door_confirm_req_async(void * /*user*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    show_outer_door_confirm_dlg();
}

/* ---- 无基板：放入基板并关外门（Cancel=阻断，勿复用外门 skip 继续） ---- */
ui_modal_dialog_t *g_plate_place_confirm_dlg = nullptr;

void close_plate_place_confirm_dlg(void)
{
    if(g_plate_place_confirm_dlg) {
        ui_modal_dialog_close(g_plate_place_confirm_dlg);
        g_plate_place_confirm_dlg = nullptr;
    }
}

void on_plate_place_confirm_ok(lv_event_t * /*e*/)
{
    close_plate_place_confirm_dlg();
    ui::UiMsgProxy::instance().send_plate_place_confirm(false);
}

void on_plate_place_confirm_cancel(lv_event_t * /*e*/)
{
    close_plate_place_confirm_dlg();
    ui::UiMsgProxy::instance().send_plate_place_confirm(true);
}

void show_plate_place_confirm_dlg(void)
{
    if(g_plate_place_confirm_dlg) {
        return;
    }
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Place the build plate", USER_DLG_W, true);
    cfg.light_theme = true;
    cfg.with_close_btn = false;
    g_plate_place_confirm_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_plate_place_confirm_dlg) {
        return;
    }
    lv_obj_t *sub = ui_user_make_body_label(
        g_plate_place_confirm_dlg->body,
        "Doors are opening. Place the build plate, close the outer door, then confirm.");
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_flow(g_plate_place_confirm_dlg->footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_plate_place_confirm_dlg->footer, USER_PAD, 0);
    lv_obj_set_flex_align(g_plate_place_confirm_dlg->footer, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ok = ui_user_make_confirm_footer_btn(
        g_plate_place_confirm_dlg->footer, "Confirm",
        true, user_light_nav_on, true,
        on_plate_place_confirm_ok, nullptr);
    lv_obj_set_flex_grow(ok, 0);
    lv_obj_set_width(ok, LV_PCT(100));
    lv_obj_t *cancel = ui_user_make_confirm_footer_btn(
        g_plate_place_confirm_dlg->footer, "Cancel",
        false, user_light_nav_on, true,
        on_plate_place_confirm_cancel, nullptr);
    lv_obj_set_flex_grow(cancel, 0);
    lv_obj_set_width(cancel, LV_PCT(100));
}

void on_plate_place_confirm_req_async(void * /*user*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    show_plate_place_confirm_dlg();
}





/* ---- 打印入口拦截：停机类报警未复位时 MCU 会拒绝新打印请求
 *      （名单见 ui_alarm_print_blocker()） ---- */




void on_print_confirm_cancel(lv_event_t * /*e*/)
{
    close_confirm_dlg();
}

void on_print_confirm_ok(lv_event_t * /*e*/)
{
    close_confirm_dlg();
    if(!page_user_files_active() || page_user_files_sel_index() < 0) {
        return;
    }
    if(ui::DebugMsgProxy::instance().test_image_active() ||
       ui::DebugMsgProxy::instance().galvo_point_test_active()) {
        ui_toast_show("Test mode active", user_color_status_err, 1800);
        return;
    }
    enter_state(CONTROL_STATE_PREPARE_WAIT);
    ui::UiMsgProxy::instance().send_print_prepare(
        static_cast<uint32_t>(page_user_files_sel_index()),
        file_tab_to_source(page_user_files_source()));
}

void close_confirm_dlg(void)
{
    if(g_confirm_dlg) {
        ui_modal_dialog_close(g_confirm_dlg);
        g_confirm_dlg = nullptr;
    }
}



/* 通用二次确认弹窗（用户层 ui_modal_dialog 风格）：取消(4% 白底) + 确认(填充色)。
 * 删除文件 / 终止打印共用，保证两处风格一致；页脚两枚按钮的样式已提到
 * ui_user_make_confirm_footer_btn()，与打印历史页的批量删除确认同源。 */
void open_user_confirm_dialog(lv_obj_t *parent, const char *title, const char *body,
                              const char *cancel_text, const char *confirm_text,
                              lv_color_t confirm_color, bool light,
                              lv_event_cb_t cancel_cb, lv_event_cb_t confirm_cb)
{
    if(g_confirm_dlg || !parent) {
        return;
    }
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(title, USER_DLG_W, true);
    /* 弹窗新稿落地后一律浅色；light 形参留给少数调用点显式选深色（目前已无）。 */
    cfg.light_theme = light;
    g_confirm_dlg = ui_modal_dialog_open(parent, &cfg);
    if(!g_confirm_dlg) {
        return;
    }

    lv_obj_t *body_lbl = ui_user_make_body_label(g_confirm_dlg->body, body);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(body_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* **两枚钮的档位要跟着卡片走**：卡片翻白了而按钮还按深色档建的话，取消钮走的是
     * 「白色压到 4%」那一支，压在白卡上完全看不见（实机上就只剩一枚确认钮）。 */
    ui_user_make_confirm_footer_btn(g_confirm_dlg->footer, cancel_text, false,
                                    user_color_panel_alt, light, cancel_cb, nullptr);
    ui_user_make_confirm_footer_btn(g_confirm_dlg->footer, confirm_text, true,
                                    confirm_color, light, confirm_cb, nullptr);
}


/* 开始打印确认弹窗的页脚按钮。走共享工厂的**浅色档**：Cancel 是 `#333145 @10%` 底配
 * 深墨字，Start Print 是 `#333145` 实心配白字。
 *
 * 主按钮不用 accent 蓝也不发光：新弹窗稿一族（6357/6382/6388/6423）的主按钮一律是深墨
 * 平涂，这里跟上。蓝色在本工程里留给 OTA 那几个确认弹窗，红色留给删除/终止。 */
lv_obj_t *make_print_confirm_btn(lv_obj_t *parent, const char *text,
                                 bool primary, lv_event_cb_t cb)
{
    return ui_user_make_confirm_footer_btn(parent, text, primary,
                                           user_light_nav_on, true,
                                           cb, nullptr);
}

/* 「开始打印」二次确认弹窗：标题 + 副标题（耗时/耗材）+ 取消/开始打印。 */
void show_print_confirm_dialog(void)
{
    if(g_confirm_dlg || page_user_files_sel_index() < 0) {
        return;
    }
    auto *dlg = static_cast<ui_modal_dialog_t *>(std::calloc(1, sizeof(ui_modal_dialog_t)));
    if(!dlg) {
        return;
    }
    const control_file_t *file = &g_active_file;

    lv_obj_t *backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, user_light_text, 0);
    lv_obj_set_style_bg_opa(backdrop, USER_LIGHT_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    dlg->root = backdrop;

    /* 白卡 + 浅色圆角，与 ui_modal_dialog_open() 的 light 档同一套外壳。本弹窗自建
     * 骨架而不走那个控件，是因为顶部那块满宽预览图做不出来（控件四周有内边距）。 */
    const int32_t card_w = USER_DLG_W;
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    /* 顶部满宽预览（16:9）。有缩略图就铺图，没有才出占位图标——先前这里只建了占位、
     * 从不去加载 thumb，于是无论如何都是一个破图。取 thumb_large 而不是 thumb_print，
     * 与选文件页详情面板同源：二次确认紧接在详情面板之后，同一个文件该还是同一张图。
     *
     * 只判路径存不存在，不额外看当前页签：USB 条目的 thumb_* 在 copy_file_entry() 里
     * 本来就是空串，判存自然为假；而页签是瞬态的页面状态，弹窗只该认它手上这份
     * g_active_file。 */
    lv_obj_t *preview = lv_obj_create(card);
    lv_obj_remove_style_all(preview);
    lv_obj_set_size(preview, card_w, card_w * 9 / 16);
    lv_obj_set_style_bg_color(preview, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(preview, 13, 0);          /* #333145 @5% */
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

    if(ui_user_thumb_path_exists(file->thumb_large)) {
        lv_obj_t *pimg = lv_image_create(preview);
        lv_image_set_src(pimg, file->thumb_large_src);
        lv_obj_set_size(pimg, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(pimg, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(pimg);
        lv_obj_remove_flag(pimg, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t *picon = lv_label_create(preview);
        lv_label_set_text(picon, LV_SYMBOL_IMAGE);
        lv_obj_set_style_text_color(picon, user_light_text, 0);
        lv_obj_set_style_text_opa(picon, LV_OPA_40, 0);
        lv_obj_set_style_text_font(picon, user_font_value(), 0);
        lv_obj_center(picon);
    }

    lv_obj_t *content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content, USER_PAD, 0);
    lv_obj_set_style_pad_row(content, USER_PAD, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Start printing this file?");
    lv_obj_set_style_text_color(title, user_light_text, 0);
    lv_obj_set_style_text_font(title, user_font_dlg_title(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    char sub[96];
    char dur[32];
    ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
    std::snprintf(sub, sizeof(sub), "Est. time %s   Material %.1f g",
                  dur, (double)file->material_g);
    lv_obj_t *sublbl = lv_label_create(content);
    lv_label_set_long_mode(sublbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sublbl, LV_PCT(100));
    lv_label_set_text(sublbl, sub);
    lv_obj_set_style_text_color(sublbl, user_light_text, 0);
    lv_obj_set_style_text_opa(sublbl, LV_OPA_50, 0);
    lv_obj_set_style_text_font(sublbl, user_font_text_sm(), 0);
    lv_obj_set_style_text_align(sublbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *footer = lv_obj_create(content);
    lv_obj_remove_style_all(footer);
    lv_obj_set_width(footer, LV_PCT(100));
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, USER_PAD, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    make_print_confirm_btn(footer, "Cancel", false, on_print_confirm_cancel);
    make_print_confirm_btn(footer, "Start Print", true, on_print_confirm_ok);

    g_confirm_dlg = dlg;
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(backdrop, 180, 0);
    lv_obj_move_foreground(backdrop);
}

void close_prepare_dlg(void)
{
    if(g_prepare_dlg) {
        ui_modal_dialog_close(g_prepare_dlg);
        g_prepare_dlg = nullptr;
    }
}

void show_prepare_dlg(void)
{
    if(g_prepare_dlg || !g_overlay) {
        return;
    }
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Preparing Print File", USER_DLG_W, false);
    cfg.light_theme = true;
    g_prepare_dlg = ui_modal_dialog_open(g_overlay, &cfg);
    if(!g_prepare_dlg) {
        return;
    }

    ui_user_make_body_label(g_prepare_dlg->body,
                            "Copying and scanning file, please wait.\n"
                            "Optical path check starts after this, on the print page.");

    lv_obj_t *vc = ui_user_make_spinner_container(g_prepare_dlg->body, USER_SPINNER_SZ);
    ui_arc_spinner_t *sp = ui_arc_spinner_create(vc, USER_SPINNER_SZ);
    if(sp && sp->root) {
        lv_obj_center(sp->root);
    }
}


/**
 * 落一次供应度：内部值 + 对外快照 + 本页那两根 bar + 观察者扇出，四件事只在这里做。
 * 分成两份值是因为 control.hpp 不该把 upc_contract.hpp 拖进每个 include 它的页面。
 */
void apply_supply(const middleware::SupplyStatusPayload &p)
{
    g_supply = p;
    g_supply_pub.material_pct = p.material_pct;
    g_supply_pub.filter_pct   = p.filter_pct;
    g_supply_pub.material_ok  = (p.material_ok != 0);
    g_supply_pub.filter_ok    = (p.filter_ok != 0);
    refresh_supply_bars();
    for(int i = 0; i < kMaxSupplyObservers; i++) {
        if(g_supply_observers[i]) {
            g_supply_observers[i](&g_supply_pub);
        }
    }
}

void refresh_supply_bars(void)
{
    if(g_supply_mat_bar) {
        lv_bar_set_value(g_supply_mat_bar, (int)g_supply.material_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(g_supply_mat_bar,
            g_supply.material_ok ? user_color_status_ok : user_color_status_err,
            LV_PART_INDICATOR);
    }
    if(g_supply_mat_pct) {
        lv_label_set_text_fmt(g_supply_mat_pct, "%d%%", (int)g_supply.material_pct);
    }
    if(g_supply_mat_status) {
        lv_label_set_text(g_supply_mat_status, g_supply.material_ok ? "Adequate" : "Low");
        lv_obj_set_style_text_color(g_supply_mat_status,
            g_supply.material_ok ? user_color_status_ok : user_color_status_err, 0);
    }
    if(g_supply_filt_bar) {
        lv_bar_set_value(g_supply_filt_bar, (int)g_supply.filter_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(g_supply_filt_bar,
            g_supply.filter_ok ? user_color_status_ok : user_color_status_err,
            LV_PART_INDICATOR);
    }
    if(g_supply_filt_pct) {
        lv_label_set_text_fmt(g_supply_filt_pct, "%d%%", (int)g_supply.filter_pct);
    }
    if(g_supply_filt_status) {
        lv_label_set_text(g_supply_filt_status, g_supply.filter_ok ? "Adequate" : "Low");
        lv_obj_set_style_text_color(g_supply_filt_status,
            g_supply.filter_ok ? user_color_status_ok : user_color_status_err, 0);
    }
}














/* ============================================================================
 *  后端回调 → 状态推进
 * ============================================================================ */

/* 清理门流程节奏计时器（跨态时统一调用，防止回调在错误状态触发） */
void stop_door_timers(void)
{
    if(g_door_advance_timer) {
        lv_timer_delete(g_door_advance_timer);
        g_door_advance_timer = nullptr;
    }
    if(g_door_open_retry_timer) {
        lv_timer_delete(g_door_open_retry_timer);
        g_door_open_retry_timer = nullptr;
    }
}

void on_door_opening_cancel(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_DOOR_OPENING) {
        if(g_cleaning_active) {
            if(g_clean_stop_pending) {
                return;
            }
            g_clean_stop_pending = true;
            if(g_door_body) {
                lv_label_set_text(g_door_body, "Stopping fan...");
            }
            backend_start_clean_stop(pf_on_clean_stop_done, nullptr);
            return;
        }
        if(g_door_origin == DoorFlowOrigin::PrintCollect) {
            /* 取件取消：退回结果页（浮层一直没收起），并把滑块推回起点 */
            enter_state(CONTROL_STATE_PRINT_DONE);
            page_user_monitor_reset_result_slide();
            return;
        }
        enter_state(CONTROL_STATE_IDLE);
    }
}

void pf_on_clean_stop_done(void * /*u*/)
{
    g_clean_stop_pending = false;
    g_cleaning_active = false;
    if(g_state != CONTROL_STATE_DOOR_OPENING) {
        return;
    }
    door_apply_variant(DoorVariant::Opening);
    g_door_opening_since = lv_tick_get();
    pf_start_open_door();
}

void door_open_retry_cb(lv_timer_t * /*t*/)
{
    g_door_open_retry_timer = nullptr;
    if(g_state == CONTROL_STATE_DOOR_OPENING) {
        pf_start_open_door();
    }
}

void arm_door_open_retry_timer(void)
{
    if(g_door_open_retry_timer || g_state != CONTROL_STATE_DOOR_OPENING) {
        return;
    }
    g_door_open_retry_timer = lv_timer_create(door_open_retry_cb,
                                              DOOR_OPEN_RETRY_MS, nullptr);
    lv_timer_set_repeat_count(g_door_open_retry_timer, 1);
}

/* ---- 转运盒三步（取件分支，稿 3171:7305 / 3225:10476 / 3225:13586）----
 * 装盒 →（转运盒到位）→ 转移工件 →（转移完成）→ 推入闸门 → 解锁取下 → 结束开门流程。
 * 后端还没有这三处信号，两处等待用 BOX_* 定时器占位，闸门那步稿上本就靠用户点。 */

/** 开门成功后收 overlay 并按入口走终态；不再做关门检测。 */
void finish_after_door_open(void)
{
    if(g_state != CONTROL_STATE_DOOR_OPENING &&
       g_state != CONTROL_STATE_TRANSFER_BOX) {
        return;
    }
    if(g_door_origin == DoorFlowOrigin::HomeStandalone ||
       g_door_origin == DoorFlowOrigin::PrintCollect ||
       g_door_origin == DoorFlowOrigin::TransferDemo) {
        /* 回 IDLE 由 exit_printing_mode 关掉还在跑的吹粉。 */
        enter_state(CONTROL_STATE_IDLE);
    } else {
        goto_file_selection();
    }
}

/** 门开之后往哪走：取件且开了转运盒流程先走三步，否则直接结束。 */
void proceed_after_door_open(void)
{
    if(transfer_box_flow_active()) {
        enter_state(CONTROL_STATE_TRANSFER_BOX);
    } else {
        finish_after_door_open();
    }
}

/* 占位「转运盒已到位」：把置灰按钮翻成可点（重建同一形态即可，见 g_box_ready） */
void box_ready_cb(lv_timer_t * /*t*/)
{
    g_door_advance_timer = nullptr;
    if(g_state != CONTROL_STATE_TRANSFER_BOX) {
        return;
    }
    g_box_ready = true;
    door_apply_variant(DoorVariant::BoxInstall);
}

/* 复用 g_door_advance_timer：装盒到位占位与门流程的最短停留不会同时在跑，
 * 且 enter_state → stop_door_timers 会统一清掉。 */
void arm_box_timer(lv_timer_cb_t cb, uint32_t period_ms)
{
    if(g_door_advance_timer) {
        lv_timer_delete(g_door_advance_timer);
    }
    g_door_advance_timer = lv_timer_create(cb, period_ms, nullptr);
    lv_timer_set_repeat_count(g_door_advance_timer, 1);
}

void on_box_install_confirm(lv_event_t * /*e*/)
{
    if(g_state != CONTROL_STATE_TRANSFER_BOX) {
        return;
    }
    /* 取件与演示：Done 后才发转运串口。 */
    ui::UiMsgProxy::instance().send_transport_request();
    door_apply_variant(DoorVariant::BoxMoving);
}

void on_transport_result(uint8_t result)
{
    if(g_state != CONTROL_STATE_TRANSFER_BOX) {
        return;
    }
    if(result == 0) {
        door_apply_variant(DoorVariant::BoxShutter);
    }
}

void on_box_shutter_done(lv_event_t * /*e*/)
{
    if(g_state != CONTROL_STATE_TRANSFER_BOX) {
        return;
    }
    ui_toast_show_icon("Transfer complete. Remove the box",
                       user_color_status_ok, 3000, &ic_badge_check);
    finish_after_door_open();
}

/* Opening 最短停留到时 → 结束开门 / 进转运盒 */
void door_advance_after_opening_cb(lv_timer_t * /*t*/)
{
    g_door_advance_timer = nullptr;   /* one-shot：LVGL 回调后自删，仅清空句柄 */
    if(g_state == CONTROL_STATE_DOOR_OPENING) {
        proceed_after_door_open();
    }
}

/* 开门应答：满足最短停留才前进（不足则延时补足），保证"正在准备开门"可见 */
void pf_on_open_door_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_DOOR_OPENING) {
        return;
    }
    const uint32_t elapsed = lv_tick_elaps(g_door_opening_since);
    if(elapsed >= DOOR_OPENING_MIN_MS) {
        proceed_after_door_open();
        return;
    }
    if(g_door_advance_timer) {
        lv_timer_delete(g_door_advance_timer);
    }
    g_door_advance_timer = lv_timer_create(door_advance_after_opening_cb,
                                           DOOR_OPENING_MIN_MS - elapsed, nullptr);
    lv_timer_set_repeat_count(g_door_advance_timer, 1);
}

void pf_start_open_door(void)
{
    if(g_door_open_retry_timer) {
        lv_timer_delete(g_door_open_retry_timer);
        g_door_open_retry_timer = nullptr;
    }
    backend_start_open_door(pf_on_open_door_done, nullptr);
}

/* 清粉最短停留到时 → 发开门请求 */
void clean_advance_to_open_cb(lv_timer_t * /*t*/)
{
    g_door_advance_timer = nullptr;
    if(g_state == CONTROL_STATE_DOOR_OPENING) {
        door_apply_variant(DoorVariant::Opening);
        g_door_opening_since = lv_tick_get();
        pf_start_open_door();
    }
}

/* 清粉应答：满足最短停留才开门（不足则延时补足），保证"正在清粉中"可见 */
void pf_on_clean_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_DOOR_OPENING) {
        return;
    }
    g_cleaning_active = false;
    g_clean_stop_pending = false;
    const uint32_t elapsed = lv_tick_elaps(g_clean_since);
    if(elapsed >= CLEAN_MIN_MS) {
        door_apply_variant(DoorVariant::Opening);
        g_door_opening_since = lv_tick_get();
        pf_start_open_door();
        return;
    }
    if(g_door_advance_timer) {
        lv_timer_delete(g_door_advance_timer);
    }
    g_door_advance_timer = lv_timer_create(clean_advance_to_open_cb,
                                           CLEAN_MIN_MS - elapsed, nullptr);
    lv_timer_set_repeat_count(g_door_advance_timer, 1);
}

void pf_on_pm25_check_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_DOOR_OPENING) {
        return;
    }
    if(g_pm25_high) {
        door_apply_variant(DoorVariant::Cleaning);
        g_clean_since = lv_tick_get();
        g_cleaning_active = true;
        g_clean_stop_pending = false;
        backend_start_clean(pf_on_clean_done, nullptr);
        return;
    }
    g_cleaning_active = false;
    g_clean_stop_pending = false;
    pf_start_open_door();
}

void pf_on_print_progress(const control_progress_t *p, void * /*u*/)
{
    if(g_state == CONTROL_STATE_PRINTING) {
        page_user_monitor_printing_update(p);
    }
}

void pf_on_print_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    ui_toast_show("Print complete", user_color_status_ok, 2000);
    /* 同 finish_printing：留在 PRINT_DONE 等 Camera 出片，不直接回 IDLE */
    enter_state(CONTROL_STATE_PRINT_DONE);
}

void show_terminated_result_without_manifest(void)
{
    if(g_state != CONTROL_STATE_PRINT_DONE || g_print_result_shown) {
        return;
    }
    control_print_result_t r = {};
    r.terminated = true;
    r.has_stats = false;
    if(g_active_file.name[0] != '\0') {
        std::snprintf(r.job_name, sizeof(r.job_name), "%s", g_active_file.name);
    }
    g_print_result = r;
    g_print_result_pending = false;
    page_user_monitor_enter_print_result(&g_print_result);
    g_print_result_shown = true;
}

/**
 * Media 写完 /userdata/media/video/config.json → 切打印结果页。
 * has_entry=1 时以 manifest 条目为权威（status/耗时/耗材）；
 * has_entry=0（0 帧未落盘）时退回用 payload.aborted 判定，统计值显示 "--"。
 */
void on_print_video_manifest(const middleware::PrintVideoManifestPayload &p)
{
    if(g_state != CONTROL_STATE_PRINTING && g_state != CONTROL_STATE_PRINT_DONE) {
        return;
    }
    /* 结果页已显示后，仍要接收后续 manifest 更新：
     * - 不能重复套结果页 UI；
     * - 但要刷新 g_print_result.video_dir（finalize 主键），避免退出后用错目录键。 */
    if(g_state == CONTROL_STATE_PRINT_DONE && g_print_result_shown) {
        if(p.has_entry && p.frames_dir[0]) {
            std::snprintf(g_print_result.video_dir, sizeof(g_print_result.video_dir),
                          "%s", p.frames_dir);

            std::vector<upc::PrintHistoryEntry> entries;
            (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);
            for(const auto &e : entries) {
                if(e.frames_dir != p.frames_dir) {
                    continue;
                }
                g_print_result.terminated = (e.status == upc::PrintHistoryStatus::Terminated);
                g_print_result.duration_sec = e.duration_sec;
                g_print_result.material_g = e.material_g;
                g_print_result.has_stats = true;
                if(!e.job_name.empty()) {
                    std::snprintf(g_print_result.job_name, sizeof(g_print_result.job_name),
                                  "%s", e.job_name.c_str());
                }
                break;
            }
        }
        return;
    }

    control_print_result_t r = {};
    r.terminated = p.aborted != 0;
    r.has_stats  = false;
    r.material_g = -1.0f;
    std::snprintf(r.job_name, sizeof(r.job_name), "%s", g_active_file.name);
    std::snprintf(r.video_dir, sizeof(r.video_dir), "%s", g_active_file.name);

    if(p.has_entry && p.frames_dir[0]) {
        std::snprintf(r.video_dir, sizeof(r.video_dir), "%s", p.frames_dir);
        std::vector<upc::PrintHistoryEntry> entries;
        (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);
        for(const auto &e : entries) {
            if(e.frames_dir != p.frames_dir) {
                continue;
            }
            r.terminated   = (e.status == upc::PrintHistoryStatus::Terminated);
            r.duration_sec = e.duration_sec;
            r.material_g   = e.material_g;
            r.has_stats    = true;
            if(!e.job_name.empty()) {
                std::snprintf(r.job_name, sizeof(r.job_name), "%s", e.job_name.c_str());
            }
            break;
        }
    }

    /* 无论通知到达时机如何，都先缓存当前结果，供结果页退出后的后台提交链路使用。 */
    g_print_result = r;

    /* Camera 可能抢在 UI 走完收尾之前就发通知（0 帧时 finalize 几乎立即返回）。
     * 这种情况下先攒着，等 enter_state(PRINT_DONE) 再套用，否则结果页会丢。 */
    if(g_state != CONTROL_STATE_PRINT_DONE) {
        g_print_result_pending = true;
        return;
    }
    page_user_monitor_enter_print_result(&r);
    g_print_result_shown = true;
}

void on_terminate_cancel(lv_event_t * /*e*/)
{
    close_confirm_dlg();
}

void on_terminate_confirm(lv_event_t * /*e*/)
{
    close_confirm_dlg();
    backend_terminate_job();
}

void on_backend_action_fail(const char *message)
{
    if(message && message[0]) {
        ui_toast_show(message, user_color_status_err, 2500);
    }
    if(g_state != CONTROL_STATE_IDLE && g_state != CONTROL_STATE_PRINTING &&
       g_state != CONTROL_STATE_PRINT_DONE) {
        enter_state(CONTROL_STATE_IDLE);
    }
}

/* ============================================================================
 *  状态机
 * ============================================================================ */

bool cloud_print_occupied(control_state_t st)
{
    if(st == CONTROL_STATE_PRINTING || st == CONTROL_STATE_PRINT_DONE) {
        return true;
    }
    return g_door_origin == DoorFlowOrigin::PrintCollect &&
           (st == CONTROL_STATE_DOOR_OPENING || st == CONTROL_STATE_TRANSFER_BOX);
}

void enter_state(control_state_t s)
{
    close_current_dialog();
    stop_door_timers();
    g_plate_content    = nullptr;
    g_plate_back_btn   = nullptr;
    g_door_img_box     = nullptr;
    g_door_content     = nullptr;
    g_door_title       = nullptr;
    g_door_body        = nullptr;
    g_door_spinner     = nullptr;
    g_cleaning_active  = false;
    g_clean_stop_pending = false;
    g_box_ready        = false;
    if(s != CONTROL_STATE_TRANSFER_BOX) {
        set_transfer_demo_pending(false);
    }
    if(s != CONTROL_STATE_DOOR_OPENING) {
        g_door_plain_opening_title = false;
    }

    const control_state_t prev = g_state;
    g_state = s;
    /* 云端 finished/canceled/aborted 要挂到打印占用结束。取件开门不算结束。 */
    if(s == CONTROL_STATE_IDLE && cloud_print_occupied(prev)) {
        ui::UiMsgProxy::instance().send_cloud_result_dismiss();
    }

    switch(s) {
    case CONTROL_STATE_IDLE:
        stop_print_watchdogs();
        g_print_result_shown = false;
        backend_cancel_pending();
        page_user_monitor_exit_printing_mode();
        overlay_hide();
        break;

    case CONTROL_STATE_PLATE_PROMPT:
        overlay_show();
        g_current_dlg = show_plate_prompt(g_overlay);
        break;

    case CONTROL_STATE_DOOR_OPENING:
        overlay_show();
        g_current_dlg = show_door_dialog(g_overlay, DoorVariant::Opening);
        g_door_opening_since = lv_tick_get();   /* 计"正在准备开门"最短停留 */
        g_door_open_fail_count = 0;
        backend_start_pm25(pf_on_pm25_check_done, nullptr);
        break;

    case CONTROL_STATE_TRANSFER_BOX:
        overlay_show();
        g_current_dlg = show_door_dialog(g_overlay, DoorVariant::BoxInstall);
        /* 「转运盒到位」占位：到时把按钮翻成可点。后端出信号后换成轮询检测。 */
        arm_box_timer(box_ready_cb, BOX_READY_MS);
        break;

    case CONTROL_STATE_PREPARE_WAIT:
        overlay_show();
        show_prepare_dlg();
        break;

    case CONTROL_STATE_PRINTING:
        overlay_hide();
        g_print_result_pending = false;      /* 丢掉上一单可能残留的结果 */
        g_print_result_shown = false;
        /* 首页图标回仪表盘：开始打印时用户正停在选文件页，那是首页区的一个视图，
         * 不复位的话打完回首页会又落回选文件页。 */
        g_home_section_view = "user_home";
        /* **打印界面在监控页**（稿 6338:27240 一族），不是首页。改版前这里切的是
         * "user_home"，浮层挂在首页上；现在整套搬到了监控页，所以直接切过去。 */
        ui::PageManager::instance().switch_to("user_monitor");
        backend_start_job(&g_active_file,
                                     pf_on_print_progress,
                                     pf_on_print_done,
                                     nullptr);
        page_user_monitor_enter_printing_mode(&g_active_file);
        start_print_peer_watch();
        break;

    case CONTROL_STATE_PRINT_DONE:
        /* 刻意不走 IDLE：exit_printing_mode() 会把 Job 卡从结果态收成待机空态。
         * 界面维持打印中原样，等 Camera 的 manifest 通知到达，
         * on_print_video_manifest() 再切结果页浮层。 */
        stop_print_watchdogs();
        overlay_hide();
        if(g_print_result_pending) {
            g_print_result_pending = false;
            page_user_monitor_enter_print_result(&g_print_result);
            g_print_result_shown = true;
        }
        break;
    }
}

/* 预确认完成 → 进入文件选择页（PageManager 隐藏页 "user_files"，状态保留）。
 * 关闭当前 overlay 弹窗并回到 IDLE，让文件页在空闲态下正常工作。 */
void goto_file_selection(void)
{
    close_current_dialog();
    backend_cancel_pending();
    g_state = CONTROL_STATE_IDLE;
    overlay_hide();
    g_home_section_view = "user_files";
    ui::PageManager::instance().switch_to("user_files");
}

} /* namespace */

/* ============================================================================
 *  对外 API
 * ============================================================================ */

/* ---- 选择文件页（page_user_files.cpp）要的数据与流程入口 ---- */

size_t control_files_count(void)
{
    return backend_file_count();
}

const control_file_t *control_files_get(size_t index)
{
    return backend_get_file(index);
}

void control_files_request_dir_open(FileTab tab)
{
    request_fs_dir_open(tab);
}

void control_files_reset_dir_pending(void)
{
    g_dir_open_pending.clear();
}

void control_files_set_active(const control_file_t *file)
{
    g_active_file = file ? *file : control_file_t{};
}

void control_files_clear(void)
{
    g_files.clear();
}

void control_files_close(void)
{
    g_home_section_view = "user_home";
    ui::PageManager::instance().switch_to("user_home");
}

void control_files_show_print_confirm(void)
{
    show_print_confirm_dialog();
}

bool control_files_delete_allowed(void)
{
    return g_state == CONTROL_STATE_IDLE;
}

void control_files_open_confirm(const char *title, const char *body,
                                const char *cancel_text, const char *confirm_text,
                                lv_color_t confirm_color, bool light,
                                lv_event_cb_t cancel_cb, lv_event_cb_t confirm_cb)
{
    /* 文件页 IDLE 时 g_overlay 隐藏，须挂 top layer（同 control_terminate_request） */
    open_user_confirm_dialog(lv_layer_top(), title, body, cancel_text, confirm_text,
                             confirm_color, light, cancel_cb, confirm_cb);
}

void control_files_close_dialog(void)
{
    close_confirm_dlg();
}

void control_init(lv_obj_t *parent)
{
    if(g_overlay) {
        return;
    }
    if(!parent) {
        parent = lv_screen_active();
    }
    wire_msg_proxy();
    ui_device_state_add_observer(on_powder_state_update);
    g_on_action_fail = on_backend_action_fail;
    backend_reset_session();

    g_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);
}

void control_start(void)
{
    if(g_state != CONTROL_STATE_IDLE) {
        return;
    }
    /* 调试开关打开：等同在预确认里选「已放入 → 选择打印任务」，不经未放入的开门分支 */
    if(g_skip_plate_prompt) {
        goto_file_selection();
        return;
    }
    /* Figma 入口：先弹「是否已放入打印版」两卡片预确认，再进入文件选择 */
    g_door_origin = DoorFlowOrigin::PrintPrep;
    enter_state(CONTROL_STATE_PLATE_PROMPT);
}

void control_show_supply_dialog(control_supply_dlg_t kind)
{
    show_supply_dialog(kind);
}

void control_open_door_request(void)
{
    if(g_state != CONTROL_STATE_IDLE) {
        return;
    }
    g_door_origin = DoorFlowOrigin::HomeStandalone;
    g_door_plain_opening_title = false;
    enter_state(CONTROL_STATE_DOOR_OPENING);
}

void control_open_door_for_inner(void)
{
    if(g_state != CONTROL_STATE_IDLE) {
        return;
    }
    /* 终态与首页开门相同（回待机），只换转圈标题。 */
    g_door_origin = DoorFlowOrigin::HomeStandalone;
    g_door_plain_opening_title = true;
    enter_state(CONTROL_STATE_DOOR_OPENING);
}

bool control_transfer_demo_start(void)
{
    if(g_state != CONTROL_STATE_IDLE || g_transfer_demo_prep_pending) {
        return false;
    }
    /* 不进开门 loading：后台降 Z / 关内门 / 开外门，Rsp 到再弹正式装盒页。 */
    g_door_origin = DoorFlowOrigin::TransferDemo;
    set_transfer_demo_pending(true);
    ui::UiMsgProxy::instance().send_transfer_demo_prep();
    return true;
}

bool control_transfer_demo_pending(void)
{
    return g_transfer_demo_prep_pending;
}

const char *control_home_section_target(void)
{
    return g_home_section_view;
}




void control_cancel(void)
{
    enter_state(CONTROL_STATE_IDLE);
}

bool control_is_printing(void)
{
    /* 含 PRINT_DONE：结果页还没取件前不允许进文件页开新任务。
     * 取件的开门流程同样算打印中——此时结果页浮层还在弹窗背后没收起。 */
    return g_state == CONTROL_STATE_PRINTING ||
           g_state == CONTROL_STATE_PRINT_DONE ||
           g_transfer_demo_prep_pending ||
           ((g_door_origin == DoorFlowOrigin::PrintCollect ||
             g_door_origin == DoorFlowOrigin::TransferDemo) &&
            (g_state == CONTROL_STATE_DOOR_OPENING ||
             g_state == CONTROL_STATE_TRANSFER_BOX));
}

void control_print_result_collect(void)
{
    if(g_state != CONTROL_STATE_PRINT_DONE) {
        return;
    }
    /* 改为滑动开门成功后即触发 finalize 链路，不再等门关闭。 */
    schedule_commit_print_result_history();
    /* 取件 = 开门，但不能借道 IDLE：IDLE 分支会 page_user_monitor_exit_printing_mode()，
     * 收起结果页浮层，弹窗就浮在待机 Job 空态上了。直接进开门态，
     * 结果页与预览原样留在弹窗背后；开门成功后由 PrintCollect 分支回 IDLE。 */
    g_door_origin = DoorFlowOrigin::PrintCollect;
    g_door_plain_opening_title = false;
    enter_state(CONTROL_STATE_DOOR_OPENING);
}

void control_set_transfer_box_flow(bool on)
{
    g_transfer_box_flow = on;
}

bool control_transfer_box_flow_enabled(void)
{
    return g_transfer_box_flow;
}

void control_set_skip_plate_prompt(bool on)
{
    g_skip_plate_prompt = on;
}

bool control_skip_plate_prompt_enabled(void)
{
    return g_skip_plate_prompt;
}

const control_supply_t *control_supply_get(void)
{
    return &g_supply_pub;
}

void control_supply_request(void)
{
    ui::UiMsgProxy::instance().send_supply_status();
}

void control_supply_add_observer(control_supply_observer_t cb)
{
    if(!cb) {
        return;
    }
    for(int i = 0; i < kMaxSupplyObservers; i++) {
        if(g_supply_observers[i] == cb) {
            return;   /* 幂等：页面重建时重复注册不会叠加 */
        }
        if(!g_supply_observers[i]) {
            g_supply_observers[i] = cb;
            return;
        }
    }
}

void control_terminate_request(void)
{
    if(!control_is_printing()) {
        return;
    }
    /* 防止残留确认窗句柄阻断本次终止确认弹窗。 */
    if(g_confirm_dlg) {
        close_confirm_dlg();
    }
    /* 与删除确认同款风格（用户层 ui_modal_dialog）。打印中 overlay 可能隐藏，
     * 故挂到 top layer 确保始终可见。
     *
     * 新稿 6345:31291（「是否确认终止任务？」）的结构与这一版**完全一致**：标题问句 +
     * 一句不可恢复说明 + 等分两栏（灰 Cancel / 红 Terminate），所以照稿对下来无需改
     * 版式，只是把节点号记在这里备查。 */
    open_user_confirm_dialog(lv_layer_top(), "Terminate this job?",
                             "The job cannot be resumed once terminated.",
                             "Cancel", "Terminate", ui_color_danger, true,
                             on_terminate_cancel, on_terminate_confirm);
}
