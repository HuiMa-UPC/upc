#include "control.hpp"
#include "ui_msg_proxy.hpp"
#include "msg_port.hpp"
#include "gcode_manifest.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_user_widgets.hpp"
#include "ui_slide_unlock.hpp"
#include "ui_arc_spinner.hpp"
#include "print_flow_img.h"
#include "home_metric_icons.h"

#include "page_user_home.hpp"
#include "page_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace {

enum class FileTab : uint8_t {
    Local = 0,
    Usb   = 1,
};

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

/* 门流程来源：区分同一套门弹窗（DOOR_OPENING→DOOR_CONFIRM）的入口与终态。
 *   PrintPrep      —— 打印前置流程，确认关门后进入选文件页；
 *   HomeStandalone —— 首页独立开门，确认关门后返回首页。 */
enum class DoorFlowOrigin { PrintPrep, HomeStandalone };
DoorFlowOrigin g_door_origin = DoorFlowOrigin::PrintPrep;

/* ---- 选择文件屏（设计稿 1.1.5）：底板 + 本机/USB 源 + 右侧详情面板 ---- */
lv_obj_t *g_file_panel   = nullptr;   /**< 选择文件页根（PageManager 托管，常驻不销毁） */
bool      g_files_page_active = false; /**< 选择文件页当前是否可见（PageManager on_enter/on_exit 维护） */
const char *g_home_section_view = "user_home"; /**< 首页区当前视图："user_home" 或 "user_files" */
lv_obj_t *g_file_grid    = nullptr;   /**< 文件行列表容器（源切换/响应时重建） */
int       g_sel_index    = -1;
FileTab   g_file_tab     = FileTab::Local;

/* 本机/USB 滑动胶囊分段控件：轨道 + 滑动高亮块 + 两个文字 */
lv_obj_t *g_seg_pill     = nullptr;   /**< 滑动高亮块（随选中左右移动） */
lv_obj_t *g_seg_lbl_local = nullptr;
lv_obj_t *g_seg_lbl_usb   = nullptr;

/* 右侧详情面板：选中文件后填充 */
lv_obj_t *g_detail_hint     = nullptr;  /**< 未选文件时的占位提示 */
lv_obj_t *g_detail_body     = nullptr;  /**< 详情内容容器（选中后显示） */
lv_obj_t *g_detail_name_lbl = nullptr;
lv_obj_t *g_detail_time_lbl = nullptr;       /**< 本机：预估耗时 */
lv_obj_t *g_detail_mat_lbl  = nullptr;       /**< 本机：预估耗材 */
lv_obj_t *g_detail_size_lbl = nullptr;       /**< USB：文件大小 */
lv_obj_t *g_detail_meta_local = nullptr;     /**< 本机详情 meta 行（耗时/耗材） */
lv_obj_t *g_detail_meta_usb   = nullptr;     /**< USB 详情 meta 行（文件大小） */
lv_obj_t *g_detail_secondary_btn = nullptr;  /**< 底部次按钮（本机=删除；USB 隐藏） */
lv_obj_t *g_detail_secondary_lbl = nullptr;  /**< 次按钮文字 */
ui_modal_dialog_t *g_confirm_dlg = nullptr;
ui_modal_dialog_t *g_prepare_dlg = nullptr;

/* 顶部供应度（耗材/滤芯）。默认值=后端未就绪时的占位（设计稿示例 48% 充足）。 */
middleware::SupplyStatusPayload g_supply = {48.0f, 48.0f, 1, 1};
lv_obj_t *g_supply_mat_bar = nullptr, *g_supply_mat_pct = nullptr, *g_supply_mat_status = nullptr;
lv_obj_t *g_supply_filt_bar = nullptr, *g_supply_filt_pct = nullptr, *g_supply_filt_status = nullptr;

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
DoneCb g_close_cb = nullptr;
void  *g_close_u = nullptr;
DoneCb g_purify_cb = nullptr;
void  *g_purify_u = nullptr;
DoneCb g_clean_cb = nullptr;
void  *g_clean_u = nullptr;

using ProgressCb = void (*)(const control_progress_t *p, void *user_data);
ProgressCb g_job_prog_cb = nullptr;
DoneCb     g_job_done_cb = nullptr;
void      *g_job_u = nullptr;

void (*g_on_action_fail)(const char *message) = nullptr;

std::vector<control_file_t> g_files;

/** USB Fs_DirOpen 请求队列：Glavo 按 FIFO 应答，弹出项须与当前 Tab 一致才刷新 UI */
std::deque<FileTab> g_dir_open_pending;

void rebuild_file_grid(void);
void update_detail_panel(const control_file_t *file);

void apply_files_list(const std::vector<control_file_t>& files)
{
    char prev_name[64] = {0};
    const bool had_sel = (g_sel_index >= 0 &&
                          g_sel_index < static_cast<int>(g_files.size()));
    if(had_sel) {
        std::snprintf(prev_name, sizeof(prev_name), "%s", g_files[g_sel_index].name);
    }

    g_files = files;

    g_sel_index = -1;
    if(had_sel) {
        for(size_t i = 0; i < g_files.size(); i++) {
            if(std::strcmp(g_files[i].name, prev_name) == 0) {
                g_sel_index = static_cast<int>(i);
                g_active_file = g_files[i];
                break;
            }
        }
    }

    if(g_files_page_active) {
        rebuild_file_grid();
        update_detail_panel(g_sel_index >= 0 ? &g_active_file : nullptr);
    }
}

/** Local：UI 直接读 /home/Gcode/config.json（不经 Fs_DirOpen IPC） */
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
void highlight_file_rows(void);
void close_confirm_dlg(void);
void show_print_confirm_dialog(void);
void close_prepare_dlg(void);
void show_prepare_dlg(void);
void goto_file_selection(void);
lv_obj_t *make_round_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb);
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

    stop_print_watchdogs();
    g_terminate_pending = false;
    g_last_print_progress = 0;
    g_last_cmd_cache_rate = 0;
    g_last_data_cache_rate = 0;

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
    enter_state(CONTROL_STATE_IDLE);
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
        if(!p.ok) {
            if(g_on_action_fail) {
                g_on_action_fail("Open door failed");
            }
            return;
        }
        if(cb) {
            cb(u);
        }
    });

    proxy.register_close_door_result([](const middleware::DoorResultPayload &p) {
        DoneCb cb = g_close_cb;
        void *u = g_close_u;
        g_close_cb = nullptr;
        g_close_u = nullptr;
        if(!p.ok) {
            /* 门未关：静默（由 DoorOpened 弹窗文案提示 + 轮询重试），不逐次弹 toast */
            return;
        }
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

    /* USB 列表经 Fs_DirOpen IPC；Local 由 load_local_files_from_config 直读盘 */
    proxy.register_fs_dir([](const middleware::FileEntryPayload *files, uint32_t count) {
        if(g_dir_open_pending.empty()) {
            return;
        }
        g_dir_open_pending.pop_front();
        if(g_file_tab != FileTab::Usb) {
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

    proxy.register_supply_status([](const middleware::SupplyStatusPayload &p) {
        g_supply = p;
        refresh_supply_bars();
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
            g_job_prog_cb(&pr, g_job_u);
        }
        if(p.progress_percent < 99) {
            cancel_drain_fallback_timer();
            return;
        }
        try_exit_print_preview(p.progress_percent >= 100);
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
        /* 自然完成也会发 Terminate_Done：须与 Progress 一致，双缓冲为 0 才退 */
        try_exit_print_preview(true);
    });

    proxy.register_xfr_pair_pin([](const middleware::XfrPairPinPayload& p) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "Pair PIN: %s (expires %us)",
                      p.pin, static_cast<unsigned>(p.expires_s));
        ui_toast_show(msg, user_color_status_ok, 2800);
    });

    proxy.register_xfr_file_received([](const middleware::XfrFileReceivedPayload& p) {
        char msg[180];
        std::snprintf(msg, sizeof(msg), "File received: %s", p.file_name);
        ui_toast_show(msg, user_color_status_ok, 2200);
        if(g_files_page_active && g_file_tab == FileTab::Local) {
            request_fs_dir_open(FileTab::Local);
        }
    });

    proxy.register_usb_hotplug([](const middleware::UsbHotplugPayload& p) {
        if(!g_files_page_active || g_file_tab != FileTab::Usb) {
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
    g_file_tab = FileTab::Local;
    g_sel_index = -1;
    ui::UiMsgProxy::instance().send_supply_status();
}

void backend_cancel_pending()
{
    g_pm25_cb = nullptr;
    g_pm25_u = nullptr;
    g_open_cb = nullptr;
    g_open_u = nullptr;
    g_close_cb = nullptr;
    g_close_u = nullptr;
    g_purify_cb = nullptr;
    g_purify_u = nullptr;
    g_clean_cb = nullptr;
    g_clean_u = nullptr;
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

void backend_start_open_door(DoneCb cb, void *u)
{
    g_open_cb = cb;
    g_open_u = u;
    ui::UiMsgProxy::instance().send_open_door();
}

void backend_start_close_door(DoneCb cb, void *u)
{
    g_close_cb = cb;
    g_close_u = u;
    ui::UiMsgProxy::instance().send_close_door_check();
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
    ui::UiMsgProxy::instance().send_print_start(file_index, file_tab_to_source(g_file_tab));
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
    /* 注意：选择文件页（g_file_panel 及其内部控件）现由 PageManager 托管、常驻不销毁，
     * 这里不再删除/清空其句柄。 */
}

/* ============================================================================
 *  打印前预确认弹窗（Figma 1.1.x）：
 *    - 宽入口弹窗（g_plate_*）：是否已放入打印版 → 已放入 / 未放入 子视图
 *    - 窄舱门弹窗（g_door_*） ：Opening → DoorOpened（监测）→ DoorClosed（用户确认）
 *  公共小部件：Mask required 徽标、缩放插画。
 * ============================================================================ */

/* ---- 宽入口弹窗：持久句柄（header 常驻，仅切换 content 子视图） ---- */
lv_obj_t *g_plate_content  = nullptr;   /**< 子视图内容容器（choose/inserted/notinserted 重建） */
lv_obj_t *g_plate_back_btn = nullptr;   /**< 返回圆钮（choose 视图隐藏） */

/* ---- 舱门弹窗：持久句柄（对齐 Figma 2566:5240/5306/5430：顶部满宽大图 + 内容列） ---- */
lv_obj_t         *g_door_img_box = nullptr;  /**< 顶部满宽图容器（切 variant 时换图） */
lv_obj_t         *g_door_content = nullptr;  /**< 下方 padded 内容列（切 variant 时重建） */
lv_obj_t         *g_door_title   = nullptr;
lv_obj_t         *g_door_body    = nullptr;  /**< [D] 状态副行（PM2.5 净化提示） */
ui_arc_spinner_t *g_door_spinner = nullptr;
/* 门流程节奏：让 Opening / DoorOpened 两个中间态"看得见"——避免后端即时应答导致一闪而过。
 *   - Opening：最短停留后再前进；
 *   - DoorOpened：最短停留后开始"关门检测"轮询，检测到门已关才进 DoorClosed。 */
constexpr uint32_t DOOR_OPENING_MIN_MS = 1500u;   /**< "正在准备开门"最短显示 */
constexpr uint32_t DOOR_OPENED_MIN_MS  = 1500u;   /**< "门已开启(置灰)"最短显示后才开始检测 */
constexpr uint32_t DOOR_CLOSE_POLL_MS  = 800u;    /**< 关门检测轮询间隔 */
constexpr uint32_t CLEAN_MIN_MS        = 1500u;   /**< "正在清粉中"最短显示 */
lv_timer_t *g_door_advance_timer    = nullptr;    /**< Opening/Cleaning 最短停留到时→前进 */
lv_timer_t *g_door_close_poll_timer = nullptr;    /**< DoorOpened 关门检测轮询 */
uint32_t    g_door_opening_since     = 0;          /**< 进入 Opening 的 tick */
uint32_t    g_clean_since            = 0;          /**< 进入 Cleaning 的 tick */

/** 口罩提示徽标（Figma：深色半透明胶囊 + 红色"圆圈 i"图标 + 红字）。 */
lv_obj_t *make_mask_badge(lv_obj_t *parent, const char *text)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_50, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(badge, USER_PAD_SM, 0);
    lv_obj_set_style_pad_right(badge, USER_PAD, 0);
    lv_obj_set_style_pad_ver(badge, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(badge, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    /* 红色"圆圈 i"图标（A8 源 40×40，缩到 20，recolor 红） */
    lv_obj_t *ibox = lv_obj_create(badge);
    lv_obj_remove_style_all(ibox);
    lv_obj_set_size(ibox, 20, 20);
    lv_obj_set_style_bg_opa(ibox, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ibox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icon = lv_image_create(ibox);
    lv_image_set_src(icon, &ic_info);
    lv_obj_set_style_image_recolor(icon, user_color_status_err, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_image_set_pivot(icon, 20, 20);
    lv_image_set_scale(icon, 256 * 20 / 40);
    lv_obj_center(icon);

    lv_obj_t *lbl = lv_label_create(badge);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, user_color_status_err, 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
    return badge;
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
        enter_state(CONTROL_STATE_DOOR_OPENING);
    }
}

/* 单张大卡片（已放入/未放入）：左插画 + 右文字/操作列 */
lv_obj_t *plate_make_single_card(const lv_image_dsc_t *img, int32_t img_w)
{
    lv_obj_t *card = lv_obj_create(g_plate_content);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_radius(card, USER_DLG_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_pad_all(card, USER_DLG_PAD, 0);
    lv_obj_set_style_pad_column(card, USER_PAD, 0);   /* 图↔文字间距收小，右列更宽 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    make_illustration(card, img, img_w);
    return card;
}

/* choose 子视图：两张可点卡片（我已放入 / 还未放入） */
lv_obj_t *plate_make_choice_card(const lv_image_dsc_t *img, int32_t img_w,
                                 const char *label, lv_event_cb_t cb)
{
    lv_obj_t *card = lv_button_create(g_plate_content);
    lv_obj_remove_style_all(card);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, USER_DLG_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_border_color(card, user_color_accent, LV_STATE_PRESSED);
    /* 压缩图片与边框空隙：内边距收到 SM，图↔文字用普通间距 */
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(card, USER_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, nullptr);

    make_illustration(card, img, img_w);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    return card;
}

/* 宽入口弹窗宽度：占屏 78%（需容下未放入分支右侧的滑动开门条）。
 * show_plate_prompt 与 plate_show_choose 共用，保证卡片铺图计算一致。 */
int32_t plate_dialog_width(void)
{
    const int32_t disp_w = lv_display_get_horizontal_resolution(nullptr);
    return disp_w > 0 ? disp_w * 78 / 100 : USER_DLG_W * 2;
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
    lv_obj_set_style_pad_column(g_plate_content, USER_DLG_PAD, 0);
    lv_obj_set_style_pad_row(g_plate_content, 0, 0);

    /* 让插图铺满卡片内宽（仅留极小边距）：
     * 每卡外宽 = (对话内宽 - 卡间距) / 2；插图宽 = 外宽 - 卡内边距×2 - 少量安全余量。 */
    const int32_t wide_w = plate_dialog_width();
    const int32_t card_outer = (wide_w - USER_DLG_PAD * 2 - USER_DLG_PAD) / 2;
    int32_t img_w = card_outer - USER_PAD_SM * 2 - 4;
    if(img_w < 120) {
        img_w = 120;
    }
    plate_make_choice_card(&img_plate_single, img_w, "Plate Loaded",
                           on_plate_pick_inserted);
    plate_make_choice_card(&img_plate_double, img_w, "Not Loaded Yet",
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

    lv_obj_t *card = plate_make_single_card(&img_plate_single, USER_HOME_DEVICE_W);
    lv_obj_t *col = plate_make_text_col(card);

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, "Plate Loaded");
    lv_obj_set_style_text_color(t, user_color_text, 0);
    lv_obj_set_style_text_font(t, user_font_value(), 0);

    lv_obj_t *s = lv_label_create(col);
    lv_label_set_text(s, "Tap the button below to choose a print file");
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_style_text_color(s, user_color_text_dim, 0);
    lv_obj_set_style_text_font(s, user_font_caption(), 0);

    lv_obj_t *btn = lv_button_create(col);
    lv_obj_add_style(btn, user_style_btn_primary(), 0);
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_margin_top(btn, USER_PAD, 0);
    lv_obj_add_event_cb(btn, on_plate_select_task, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Select Print Job");
    lv_obj_set_style_text_color(bl, user_color_text, 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);
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

    lv_obj_t *card = plate_make_single_card(&img_plate_double, USER_HOME_DEVICE_W);
    lv_obj_t *col = plate_make_text_col(card);

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, "Not Loaded Yet");
    lv_obj_set_style_text_color(t, user_color_text, 0);
    lv_obj_set_style_text_font(t, user_font_value(), 0);

    lv_obj_t *s = lv_label_create(col);
    lv_label_set_text(s, "Slide to open the door and load the plate. "
                         "Please wear a mask.");
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_style_text_color(s, user_color_text_dim, 0);
    lv_obj_set_style_text_font(s, user_font_caption(), 0);

    make_mask_badge(col, "Wear a mask before opening");

    /* 滑块加长到贴合右侧文字列右边缘：
     * 列宽 = 对话内宽 - 插图宽 - 卡内边距×4 - 图文间距（-2 防溢出）。 */
    int32_t slide_w = plate_dialog_width()
                      - USER_DLG_PAD * 4 - USER_HOME_DEVICE_W - USER_PAD - 2;
    if(slide_w < USER_SLIDE_W) {
        slide_w = USER_SLIDE_W;
    }

    ui_slide_unlock_cfg_t slide_cfg = {};
    slide_cfg.hint_text     = "SWIPE TO OPEN";   /* 箭头由控件绘制 3 个 chevron */
    slide_cfg.on_unlocked   = on_plate_swipe_open;
    slide_cfg.user_data     = nullptr;
    slide_cfg.track_w       = slide_w;                         /* 贴合容器右边缘 */
    slide_cfg.track_h       = USER_BTN_DOOR_ICON_SZ * 4 / 3;   /* 加高，避免过扁（与首页开门滑块一致） */
    slide_cfg.custom_accent = true;
    slide_cfg.accent_color  = user_color_status_err;
    ui_slide_unlock_t *slide = ui_slide_unlock_create(col, &slide_cfg);
    lv_obj_t *sr = ui_slide_unlock_get_root(slide);
    if(sr) {
        lv_obj_set_style_margin_top(sr, USER_PAD, 0);
    }
}

/* 宽入口弹窗：backdrop + 宽卡片 + 常驻 header（返回/标题/副标题/关闭）+ content */
ui_modal_dialog_t *show_plate_prompt(lv_obj_t *parent)
{
    const int32_t wide_w = plate_dialog_width();

    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(nullptr, wide_w, false);
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

    /* header 行：返回钮（choose 隐藏）+ 标题/副标题列（flex_grow）+ 关闭钮 */
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
    lv_obj_set_style_text_color(title, user_color_text, 0);
    lv_obj_set_style_text_font(title, user_font_value(), 0);

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

/* -------------------------- 舱门弹窗（Figma 2566:5240/5306/5430） -------------------------- */

enum class DoorVariant { Opening, Cleaning, DoorOpened, DoorClosed };

void on_door_closed_confirm_btn(lv_event_t *e);
void door_apply_variant(DoorVariant v);

/* 满宽圆角矩形主按钮（门弹窗底部）。enabled=false → 透明度 30 且不可点。 */
lv_obj_t *make_door_button(lv_obj_t *parent, const char *text, bool enabled,
                           lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_DLG_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_color_accent, 0);
    lv_obj_set_style_bg_opa(btn, enabled ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(enabled && cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, text);
    lv_obj_set_style_text_color(bl, user_color_text, 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);
    return btn;
}

/* 顶图：把 496×321 源图等比铺满 g_door_img_box（宽 = 卡片宽）。 */
void door_set_image(const lv_image_dsc_t *img)
{
    if(!g_door_img_box) {
        return;
    }
    lv_obj_clean(g_door_img_box);
    const int32_t bw = USER_DLG_W;
    const int32_t nat_w = (int32_t)img->header.w;
    const int32_t scale = nat_w > 0 ? 256 * bw / nat_w : 256;
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

    /* 背景遮罩（吞点击，防穿透底层首页） */
    lv_obj_t *backdrop = lv_obj_create(parent);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, USER_DLG_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    dlg->root = backdrop;

    /* 卡片：无内边距（顶图满宽），overflow 裁剪保证上圆角 */
    const int32_t card_w = USER_DLG_W;
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_DLG_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_shadow_width(card, 60, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    /* 顶部满宽图容器（高 = 宽 × 321/496，齐卡片左右上边缘，圆角随卡片裁剪） */
    g_door_img_box = lv_obj_create(card);
    lv_obj_remove_style_all(g_door_img_box);
    lv_obj_set_size(g_door_img_box, card_w, card_w * 321 / 496);
    lv_obj_set_style_bg_color(g_door_img_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_door_img_box, LV_OPA_20, 0);
    lv_obj_clear_flag(g_door_img_box, LV_OBJ_FLAG_SCROLLABLE);

    /* 下方 padded 内容列（标题/徽标/副标题/按钮由 variant 重建） */
    g_door_content = lv_obj_create(card);
    lv_obj_remove_style_all(g_door_content);
    lv_obj_set_width(g_door_content, LV_PCT(100));
    lv_obj_set_height(g_door_content, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(g_door_content, USER_DLG_PAD, 0);
    lv_obj_set_style_pad_row(g_door_content, USER_DLG_PAD, 0);
    lv_obj_set_style_bg_opa(g_door_content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_door_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_door_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_door_content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(backdrop, 180, 0);
    lv_obj_move_foreground(backdrop);

    door_apply_variant(variant);
    return dlg;
}

/* 标题 + 徽标同组（gap SM），用于 DoorOpened / DoorClosed。返回 title 句柄。 */
lv_obj_t *door_make_title_group(lv_obj_t *parent)
{
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_width(grp, LV_PCT(100));
    lv_obj_set_height(grp, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grp, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(grp);
    lv_obj_set_style_text_color(title, user_color_text, 0);
    lv_obj_set_style_text_font(title, user_font_value(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    make_mask_badge(grp, "Please wear a mask");
    return title;
}

/* 切换舱门弹窗形态（Figma 三态）：换顶图 + 重建内容列（标题/徽标/副标题/操作）。 */
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
        door_set_image(&img_device_unlock);

        g_door_title = lv_label_create(g_door_content);
        lv_label_set_text(g_door_title,
                          v == DoorVariant::Cleaning ? "Cleaning powder..."
                                                     : "Opening the door...");
        lv_obj_set_style_text_color(g_door_title, user_color_text, 0);
        lv_obj_set_style_text_font(g_door_title, user_font_value(), 0);
        lv_obj_set_style_text_align(g_door_title, LV_TEXT_ALIGN_CENTER, 0);

        make_mask_badge(g_door_content, "Please wear a mask");

        lv_obj_t *sp_box = lv_obj_create(g_door_content);
        lv_obj_remove_style_all(sp_box);
        lv_obj_set_size(sp_box, USER_SPINNER_SZ, USER_SPINNER_SZ);
        lv_obj_set_style_bg_opa(sp_box, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(sp_box, LV_OBJ_FLAG_SCROLLABLE);
        g_door_spinner = ui_arc_spinner_create(sp_box, USER_SPINNER_SZ);
        if(g_door_spinner && g_door_spinner->root) {
            lv_obj_center(g_door_spinner->root);
        }

        /* 状态副行（PM2.5 净化提示；默认空） */
        g_door_body = lv_label_create(g_door_content);
        lv_label_set_text(g_door_body, "");
        lv_obj_set_style_text_color(g_door_body, user_color_text_dim, 0);
        lv_obj_set_style_text_font(g_door_body, user_font_caption(), 0);
        lv_obj_set_style_text_align(g_door_body, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    /* DoorOpened / DoorClosed：标题+徽标成组 + 灰字副标题 + 满宽按钮 */
    door_set_image(v == DoorVariant::DoorOpened ? &img_door_opened : &img_device_lock);

    g_door_title = door_make_title_group(g_door_content);

    lv_obj_t *sub = lv_label_create(g_door_content);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_color(sub, user_color_text_dim, 0);
    lv_obj_set_style_text_font(sub, user_font_caption(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    if(v == DoorVariant::DoorOpened) {
        lv_label_set_text(g_door_title, "Door Opened");
        lv_label_set_text(sub, "Wear a mask and load the print plate");
        make_door_button(g_door_content, "Door Not Closed", false, nullptr);
    } else {
        lv_label_set_text(g_door_title, "Confirm the door is closed");
        lv_label_set_text(sub, "Confirm to start selecting a job");
        make_door_button(g_door_content, "Door Closed", true, on_door_closed_confirm_btn);
    }
}

/* 文件大小（字节）→ 人类可读字符串 */
void format_file_size(char *buf, size_t n, uint32_t bytes)
{
    if(bytes >= 1024u * 1024u) {
        std::snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if(bytes >= 1024u) {
        std::snprintf(buf, n, "%.1f KB", (double)bytes / 1024.0);
    } else {
        std::snprintf(buf, n, "%u B", bytes);
    }
}

/* 选中文件 → 填充右侧详情面板。
 * 本机：预估耗时/耗材 + 删除按钮（原样）；USB：仅文件大小，无次按钮。 */
void update_detail_panel(const control_file_t *file)
{
    const bool has = (file != nullptr);
    if(g_detail_hint) {
        if(has) lv_obj_add_flag(g_detail_hint, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(g_detail_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_detail_body) {
        if(has) lv_obj_clear_flag(g_detail_body, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(g_detail_body, LV_OBJ_FLAG_HIDDEN);
    }
    if(!has) {
        return;
    }

    const bool is_usb = (g_file_tab == FileTab::Usb);

    if(g_detail_name_lbl) {
        lv_label_set_text(g_detail_name_lbl, file->name);
    }

    /* meta 行：本机显示耗时/耗材，USB 显示文件大小（互斥） */
    if(g_detail_meta_local) {
        if(is_usb) lv_obj_add_flag(g_detail_meta_local, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(g_detail_meta_local, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_detail_meta_usb) {
        if(is_usb) lv_obj_clear_flag(g_detail_meta_usb, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(g_detail_meta_usb, LV_OBJ_FLAG_HIDDEN);
    }
    if(!is_usb) {
        if(g_detail_time_lbl) {
            char dur[32];
            ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
            lv_label_set_text(g_detail_time_lbl, dur);
        }
        if(g_detail_mat_lbl) {
            lv_label_set_text_fmt(g_detail_mat_lbl, "%.1f g", (double)file->material_g);
        }
    } else if(g_detail_size_lbl) {
        char sz[32];
        format_file_size(sz, sizeof(sz), file->file_size);
        lv_label_set_text(g_detail_size_lbl, sz);
    }

    /* 次按钮：仅本机显示“删除该文件”，USB 无次按钮 */
    if(g_detail_secondary_btn) {
        if(is_usb) lv_obj_add_flag(g_detail_secondary_btn, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(g_detail_secondary_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_detail_secondary_lbl) {
        lv_label_set_text(g_detail_secondary_lbl, "Delete File");
    }
}

void on_file_card_click(lv_event_t *e)
{
    const size_t index =
        static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    const control_file_t *file = backend_get_file(index);
    if(!file || !g_files_page_active) {
        return;
    }
    /* 再次点击已选中的文件 → 取消选中，回到空态 */
    if(g_sel_index == static_cast<int>(index)) {
        g_sel_index = -1;
        update_detail_panel(nullptr);
        highlight_file_rows();
        return;
    }
    g_sel_index = static_cast<int>(index);
    g_active_file = *file;
    update_detail_panel(file);
    highlight_file_rows();
    /* 不再在选择时请求 file_info：详情用目录项数据即可，避免后端为预览而全量解析阻塞 */
}

void on_file_select_cancel(lv_event_t * /*e*/)
{
    /* 选择文件页的返回键：首页区视图切回仪表盘 */
    g_home_section_view = "user_home";
    ui::PageManager::instance().switch_to("user_home");
}

/* 详情面板「Start Print」→ Prepare → D1… */
void on_detail_start_print(lv_event_t * /*e*/)
{
    if(!g_files_page_active || g_sel_index < 0) {
        return;
    }
    show_print_confirm_dialog();
}

void on_print_confirm_cancel(lv_event_t * /*e*/)
{
    close_confirm_dlg();
}

void on_print_confirm_ok(lv_event_t * /*e*/)
{
    close_confirm_dlg();
    if(!g_files_page_active || g_sel_index < 0) {
        return;
    }
    enter_state(CONTROL_STATE_PREPARE_WAIT);
    ui::UiMsgProxy::instance().send_print_prepare(
        static_cast<uint32_t>(g_sel_index), file_tab_to_source(g_file_tab));
}

void close_confirm_dlg(void)
{
    if(g_confirm_dlg) {
        ui_modal_dialog_close(g_confirm_dlg);
        g_confirm_dlg = nullptr;
    }
}

void on_delete_cancel(lv_event_t * /*e*/)
{
    close_confirm_dlg();
}

void on_delete_confirm(lv_event_t * /*e*/)
{
    close_confirm_dlg();
    if(g_state != CONTROL_STATE_IDLE || g_sel_index < 0 || g_file_tab != FileTab::Local) {
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(g_sel_index);
    ui::UiMsgProxy::instance().send_fs_delete_ntf(idx, middleware::PrintFileSource::Local);
    g_sel_index = -1;
    g_active_file = {};
    update_detail_panel(nullptr);
    request_fs_dir_open(FileTab::Local);
}

/* 通用二次确认弹窗页脚按钮：cancel=描边；confirm=填充色（危险/强调） */
lv_obj_t *make_confirm_footer_btn(lv_obj_t *footer, const char *text,
                                  bool filled, lv_color_t fill_color,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if(filled) {
        lv_obj_set_style_bg_color(btn, fill_color, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(btn, user_color_panel_alt, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, user_color_divider, 0);
    }
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_center(lbl);
    return btn;
}

/* 通用二次确认弹窗（用户层 ui_modal_dialog 风格）：取消(描边) + 确认(填充色)。
 * 删除文件 / 终止打印共用，保证两处风格一致。 */
void open_user_confirm_dialog(lv_obj_t *parent, const char *title, const char *body,
                              const char *cancel_text, const char *confirm_text,
                              lv_color_t confirm_color,
                              lv_event_cb_t cancel_cb, lv_event_cb_t confirm_cb)
{
    if(g_confirm_dlg || !parent) {
        return;
    }
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(title, USER_DLG_W, true);
    g_confirm_dlg = ui_modal_dialog_open(parent, &cfg);
    if(!g_confirm_dlg) {
        return;
    }

    lv_obj_t *body_lbl = ui_user_make_body_label(g_confirm_dlg->body, body);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(body_lbl, LV_TEXT_ALIGN_CENTER, 0);

    make_confirm_footer_btn(g_confirm_dlg->footer, cancel_text, false,
                            user_color_panel_alt, cancel_cb);
    make_confirm_footer_btn(g_confirm_dlg->footer, confirm_text, true,
                            confirm_color, confirm_cb);
}

/* 删除二次确认弹窗：取消 / 删除（红色危险按钮） */
void show_delete_confirm(void)
{
    char msg[96];
    std::snprintf(msg, sizeof(msg), "\"%s\" will be permanently deleted.", g_active_file.name);
    /* 文件页 IDLE 时 g_overlay 隐藏，须挂 top layer（同 control_terminate_request） */
    open_user_confirm_dialog(lv_layer_top(), "Delete this file?", msg,
                             "Cancel", "Delete", ui_color_danger,
                             on_delete_cancel, on_delete_confirm);
}

/* 确认弹窗页脚按钮：两枚等宽圆角，主按钮填充色、次按钮浅灰底。 */
lv_obj_t *make_print_confirm_btn(lv_obj_t *parent, const char *text,
                                 bool primary, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    if(primary) {
        lv_obj_set_style_bg_color(btn, user_color_accent, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_color(btn, user_color_panel_alt, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    }
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, text);
    lv_obj_set_style_text_color(bl, user_color_text, 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);
    return btn;
}

/* 「开始打印」二次确认弹窗：标题 + 副标题（耗时/耗材）+ 取消/开始打印。 */
void show_print_confirm_dialog(void)
{
    if(g_confirm_dlg || g_sel_index < 0) {
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
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, USER_DLG_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    dlg->root = backdrop;

    const int32_t card_w = USER_DLG_W;
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_DLG_RADIUS, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_shadow_width(card, 60, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    lv_obj_t *preview = lv_obj_create(card);
    lv_obj_remove_style_all(preview);
    lv_obj_set_size(preview, card_w, card_w * 9 / 16);
    lv_obj_set_style_bg_color(preview, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *picon = lv_label_create(preview);
    lv_label_set_text(picon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(picon, user_color_text_dim, 0);
    lv_obj_set_style_text_font(picon, user_font_value(), 0);
    lv_obj_center(picon);

    lv_obj_t *content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(content, USER_DLG_PAD, 0);
    lv_obj_set_style_pad_row(content, USER_DLG_PAD, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "Start printing this file?");
    lv_obj_set_style_text_color(title, user_color_text, 0);
    lv_obj_set_style_text_font(title, user_font_value(), 0);
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
    lv_obj_set_style_text_color(sublbl, user_color_text_dim, 0);
    lv_obj_set_style_text_font(sublbl, user_font_caption(), 0);
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
    g_prepare_dlg = ui_modal_dialog_open(g_overlay, &cfg);
    if(!g_prepare_dlg) {
        return;
    }

    ui_user_make_body_label(g_prepare_dlg->body,
                            "Copying and scanning file, please wait");

    lv_obj_t *vc = ui_user_make_spinner_container(g_prepare_dlg->body, USER_SPINNER_SZ);
    ui_arc_spinner_t *sp = ui_arc_spinner_create(vc, USER_SPINNER_SZ);
    if(sp && sp->root) {
        lv_obj_center(sp->root);
    }
}

/* 详情面板底部次按钮：仅本机显示 → 删除该文件（二次确认） */
void on_detail_secondary(lv_event_t * /*e*/)
{
    if(g_state != CONTROL_STATE_IDLE || g_sel_index < 0 || g_file_tab != FileTab::Local) {
        return;
    }
    show_delete_confirm();
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

/* 移动滑动胶囊高亮块到当前源，并刷新两个文字的明暗 */
void seg_set_active(FileTab tab, bool animate)
{
    if(g_seg_pill) {
        const int32_t pill_w = lv_obj_get_width(g_seg_pill);
        const int32_t target = (tab == FileTab::Local) ? 0 : pill_w;
        if(animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, g_seg_pill);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
            lv_anim_set_values(&a, lv_obj_get_x(g_seg_pill), target);
            lv_anim_set_duration(&a, 180);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        } else {
            lv_obj_set_x(g_seg_pill, target);
        }
    }
    if(g_seg_lbl_local) {
        lv_obj_set_style_text_color(g_seg_lbl_local,
            tab == FileTab::Local ? user_color_text : user_color_text_dim, 0);
    }
    if(g_seg_lbl_usb) {
        lv_obj_set_style_text_color(g_seg_lbl_usb,
            tab == FileTab::Usb ? user_color_text : user_color_text_dim, 0);
    }
}

void switch_source(FileTab tab)
{
    if(g_file_tab == tab) {
        return;
    }
    g_file_tab = tab;
    g_sel_index = -1;
    seg_set_active(tab, true);
    update_detail_panel(nullptr);
    g_files.clear();
    rebuild_file_grid();
    request_fs_dir_open(tab);
}

void on_tab_local(lv_event_t * /*e*/) { switch_source(FileTab::Local); }
void on_tab_usb(lv_event_t * /*e*/)   { switch_source(FileTab::Usb); }

/* 设计稿为横向列表行：缩略图 + 名称 + （耗时 / 耗材）meta */
lv_obj_t *make_file_card(lv_obj_t *parent, const control_file_t *file, size_t index)
{
    const int32_t thumb_sz = USER_PAD * 3;   /* 方形缩略图，随 profile 缩放 */

    lv_obj_t *row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, user_color_panel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, user_color_panel_alt, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, user_color_divider, 0);
    lv_obj_set_style_radius(row, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(row, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(row, USER_PAD, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, on_file_card_click, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));

    /* 缩略图（中性灰圆角块 + 文件图标，无设计稿渲染图时的占位） */
    lv_obj_t *thumb = lv_obj_create(row);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, thumb_sz, thumb_sz);
    lv_obj_set_style_bg_color(thumb, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, USER_RADIUS, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ticon = lv_label_create(thumb);
    lv_label_set_text(ticon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(ticon, user_color_text_dim, 0);
    lv_obj_set_style_text_font(ticon, user_font_text(), 0);
    lv_obj_center(ticon);

    /* 文字列：名称 + meta */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, file->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);

    /* USB 列表行不显示预估耗时/耗材（仅本机显示） */
    if(g_file_tab != FileTab::Usb) {
        char info[64];
        ui_user_format_file_meta(info, sizeof(info),
                                 file->duration_min, file->material_g);
        lv_obj_t *meta = lv_label_create(col);
        lv_label_set_text(meta, info);
        lv_obj_set_style_text_color(meta, user_color_text_dim, 0);
        lv_obj_set_style_text_font(meta, user_font_caption(), 0);
    }

    return row;
}

void highlight_file_rows(void)
{
    if(!g_file_grid) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(g_file_grid);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(g_file_grid, i);
        const bool sel = (static_cast<int>(i) == g_sel_index);
        /* 选中态：橙描边 + 橙色半透明底（设计稿 #FF7843@0.1 + 橙边） */
        lv_obj_set_style_border_color(row,
            sel ? user_color_accent : user_color_divider, 0);
        lv_obj_set_style_border_width(row, sel ? 2 : 1, 0);
        lv_obj_set_style_bg_color(row, sel ? user_color_accent : user_color_panel, 0);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_10 : LV_OPA_COVER, 0);
    }
}

void rebuild_file_grid(void)
{
    if(!g_file_grid) {
        return;
    }
    /* 保存重建前的滚动位置，重建后恢复（切回页面刷新目录时不跳回顶端） */
    const int32_t prev_scroll_y = lv_obj_get_scroll_y(g_file_grid);

    lv_obj_clean(g_file_grid);
    const size_t n = backend_file_count();
    if(n == 0) {
        lv_obj_t *empty = ui_user_make_body_label(g_file_grid, "No print files available");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        return;
    }
    for(size_t i = 0; i < n; i++) {
        const control_file_t *file = backend_get_file(i);
        if(file) {
            make_file_card(g_file_grid, file, i);
        }
    }
    highlight_file_rows();

    /* 先让布局/滚动范围生效，再恢复滚动位置（超出范围会被自动钳制） */
    lv_obj_update_layout(g_file_grid);
    lv_obj_scroll_to_y(g_file_grid, prev_scroll_y, LV_ANIM_OFF);
}

/* 本机/USB 滑动胶囊分段控件：胶囊轨道 + 浮动滑块(g_seg_pill) + 两个透明点击按钮 */
lv_obj_t *make_source_segment(lv_obj_t *parent)
{
    const int32_t seg_h   = USER_BACK_BTN_H * 3 / 2;      /* 比返回钮更高，避免偏扁 */
    const int32_t seg_w   = USER_BACK_BTN_H * 9 / 2;      /* ≈3:1，贴近设计稿胶囊比例 */
    const int32_t inner_w = seg_w - USER_PAD_SM;          /* 去掉左右内边距 */
    const int32_t pill_w  = inner_w / 2;

    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, seg_w, seg_h);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(track, USER_PAD_SM / 2, 0);
    lv_obj_set_flex_flow(track, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    /* 浮动滑块（不参与 flex，绝对定位、可动画） */
    g_seg_pill = lv_obj_create(track);
    lv_obj_remove_style_all(g_seg_pill);
    lv_obj_add_flag(g_seg_pill, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(g_seg_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_seg_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_seg_pill, pill_w, seg_h - USER_PAD_SM);
    lv_obj_set_pos(g_seg_pill, 0, 0);
    lv_obj_set_style_radius(g_seg_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_seg_pill, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(g_seg_pill, LV_OPA_COVER, 0);

    /* 两个等宽透明点击按钮 */
    lv_obj_t *btn_local = lv_button_create(track);
    lv_obj_remove_style_all(btn_local);
    lv_obj_set_size(btn_local, pill_w, LV_PCT(100));
    lv_obj_set_style_bg_opa(btn_local, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_local, on_tab_local, LV_EVENT_CLICKED, nullptr);
    g_seg_lbl_local = lv_label_create(btn_local);
    lv_label_set_text(g_seg_lbl_local, "Local");
    lv_obj_set_style_text_font(g_seg_lbl_local, user_font_text(), 0);
    lv_obj_center(g_seg_lbl_local);

    lv_obj_t *btn_usb = lv_button_create(track);
    lv_obj_remove_style_all(btn_usb);
    lv_obj_set_size(btn_usb, pill_w, LV_PCT(100));
    lv_obj_set_style_bg_opa(btn_usb, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_usb, on_tab_usb, LV_EVENT_CLICKED, nullptr);
    g_seg_lbl_usb = lv_label_create(btn_usb);
    lv_label_set_text(g_seg_lbl_usb, "USB");
    lv_obj_set_style_text_font(g_seg_lbl_usb, user_font_text(), 0);
    lv_obj_center(g_seg_lbl_usb);

    seg_set_active(g_file_tab, false);
    return track;
}

/* 详情面板内的「耗时 / 耗材」小项：caption + value（value 句柄回传以便刷新） */
void make_detail_inline_kv(lv_obj_t *parent, const char *caption,
                           lv_color_t value_color, lv_obj_t **out_val)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(col);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *val = lv_label_create(col);
    lv_label_set_text(val, "—");
    lv_obj_set_style_text_color(val, value_color, 0);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    if(out_val) {
        *out_val = val;
    }
}

void build_detail_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, USER_PAD, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 未选文件时的占位提示（垂直居中） */
    g_detail_hint = ui_user_make_body_label(panel, "Select a file to continue");
    lv_obj_set_width(g_detail_hint, LV_PCT(100));
    lv_obj_set_style_text_align(g_detail_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_grow(g_detail_hint, 1);

    /* 选中后显示的详情内容 */
    g_detail_body = lv_obj_create(panel);
    lv_obj_remove_style_all(g_detail_body);
    lv_obj_set_width(g_detail_body, LV_PCT(100));
    lv_obj_set_flex_grow(g_detail_body, 1);
    lv_obj_set_flex_flow(g_detail_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_detail_body, USER_PAD, 0);
    lv_obj_set_style_bg_opa(g_detail_body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_body, LV_OBJ_FLAG_SCROLLABLE);

    /* 大缩略图（中性灰占位） */
    lv_obj_t *preview = lv_obj_create(g_detail_body);
    lv_obj_remove_style_all(preview);
    lv_obj_set_size(preview, LV_PCT(100), USER_FILE_DLG_PREVIEW_H);
    lv_obj_set_style_bg_color(preview, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(preview, USER_RADIUS, 0);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *picon = lv_label_create(preview);
    lv_label_set_text(picon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(picon, user_color_text_dim, 0);
    lv_obj_set_style_text_font(picon, user_font_value(), 0);
    lv_obj_center(picon);

    /* 文件名 */
    g_detail_name_lbl = lv_label_create(g_detail_body);
    lv_label_set_text(g_detail_name_lbl, "—");
    lv_label_set_long_mode(g_detail_name_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_detail_name_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(g_detail_name_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(g_detail_name_lbl, user_font_text(), 0);

    /* 本机 meta：耗时 / 耗材 一行 */
    g_detail_meta_local = lv_obj_create(g_detail_body);
    lv_obj_remove_style_all(g_detail_meta_local);
    lv_obj_set_width(g_detail_meta_local, LV_PCT(100));
    lv_obj_set_height(g_detail_meta_local, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_detail_meta_local, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_detail_meta_local, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(g_detail_meta_local, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_meta_local, LV_OBJ_FLAG_SCROLLABLE);
    make_detail_inline_kv(g_detail_meta_local, "Est. Time", user_color_text, &g_detail_time_lbl);
    make_detail_inline_kv(g_detail_meta_local, "Material", user_color_accent, &g_detail_mat_lbl);

    /* USB meta：文件大小（与本机 meta 互斥显示） */
    g_detail_meta_usb = lv_obj_create(g_detail_body);
    lv_obj_remove_style_all(g_detail_meta_usb);
    lv_obj_set_width(g_detail_meta_usb, LV_PCT(100));
    lv_obj_set_height(g_detail_meta_usb, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_detail_meta_usb, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(g_detail_meta_usb, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_meta_usb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_detail_meta_usb, LV_OBJ_FLAG_HIDDEN);
    make_detail_inline_kv(g_detail_meta_usb, "File Size", user_color_text, &g_detail_size_lbl);

    /* 撑开，把按钮顶到底部 */
    lv_obj_t *sp = lv_obj_create(g_detail_body);
    lv_obj_remove_style_all(sp);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);

    lv_obj_t *btn_start = lv_button_create(g_detail_body);
    lv_obj_add_style(btn_start, user_style_btn_primary(), 0);
    lv_obj_set_width(btn_start, LV_PCT(100));
    lv_obj_set_height(btn_start, USER_DLG_FOOTER_BTN_H);
    lv_obj_add_event_cb(btn_start, on_detail_start_print, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn_start);
    lv_label_set_text(bl, "Start Print");
    lv_obj_set_style_text_color(bl, user_color_text, 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);

    /* 次按钮：仅本机显示“删除该文件”（USB 无次按钮，在 update_detail_panel 中隐藏） */
    g_detail_secondary_btn = lv_button_create(g_detail_body);
    lv_obj_t *sec = g_detail_secondary_btn;
    lv_obj_remove_style_all(sec);
    lv_obj_set_width(sec, LV_PCT(100));
    lv_obj_set_height(sec, USER_DLG_BTN_GHOST_H);
    lv_obj_set_style_bg_opa(sec, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(sec, user_color_panel_alt, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sec, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_radius(sec, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(sec, 1, 0);
    lv_obj_set_style_border_color(sec, user_color_divider, 0);
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sec, on_detail_secondary, LV_EVENT_CLICKED, nullptr);

    g_detail_secondary_lbl = lv_label_create(sec);
    lv_label_set_text(g_detail_secondary_lbl, "Delete File");
    lv_obj_set_style_text_color(g_detail_secondary_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(g_detail_secondary_lbl, user_font_text(), 0);
    lv_obj_center(g_detail_secondary_lbl);
}

/* 列表上/下滚动（设计稿右侧圆形箭头按钮）。
 * LVGL：正 dy 使内容下移（看上面的项）；负 dy 看下面的项。 */
void on_list_scroll_up(lv_event_t * /*e*/)
{
    if(g_file_grid) {
        /* bounded 版按内容范围钳制，到顶端即停、不过冲卡住 */
        lv_obj_scroll_by_bounded(g_file_grid, 0, lv_obj_get_height(g_file_grid) * 3 / 4, LV_ANIM_ON);
    }
}

void on_list_scroll_down(lv_event_t * /*e*/)
{
    if(g_file_grid) {
        lv_obj_scroll_by_bounded(g_file_grid, 0, -lv_obj_get_height(g_file_grid) * 3 / 4, LV_ANIM_ON);
    }
}

/* 圆形图标按钮（返回箭头等） */
lv_obj_t *make_round_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_BACK_BTN_H, USER_BACK_BTN_H);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, user_color_panel, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, user_color_panel_alt, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, user_color_divider, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, user_color_text, 0);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);
    lv_obj_center(icon);
    return btn;
}

/* 选择文件页内容：建入 PageManager 内容区（parent 已在侧栏右侧），填满整块。
 * 返回页根，供 page_user_files_create 交给 PageManager 托管。 */
lv_obj_t *show_file_select(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, user_color_bg, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(page, USER_PAD_LG, 0);
    lv_obj_set_style_pad_ver(page, USER_PAD, 0);
    lv_obj_set_style_pad_column(page, USER_PAD, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_file_panel = page;

    /* ============ 左半（略宽）：顶部 返回+分段胶囊；下方 文件列表 ============ */
    lv_obj_t *left = lv_obj_create(page);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 11);          /* 左:右 ≈ 11:9，左半略宽 */
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(left, USER_PAD, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* 左半顶部：返回圆钮 + 本机/USB 滑动胶囊 */
    lv_obj_t *lhead = lv_obj_create(left);
    lv_obj_remove_style_all(lhead);
    lv_obj_set_width(lhead, LV_PCT(100));
    lv_obj_set_height(lhead, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lhead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lhead, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(lhead, USER_PAD, 0);
    lv_obj_set_style_bg_opa(lhead, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lhead, LV_OBJ_FLAG_SCROLLABLE);
    make_round_icon_btn(lhead, LV_SYMBOL_LEFT, on_file_select_cancel);
    make_source_segment(lhead);

    /* 文件列表（纵向行，可滚动） */
    g_file_grid = lv_obj_create(left);
    lv_obj_remove_style_all(g_file_grid);
    lv_obj_set_width(g_file_grid, LV_PCT(100));
    lv_obj_set_flex_grow(g_file_grid, 1);
    lv_obj_set_flex_flow(g_file_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_file_grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g_file_grid, USER_PAD_SM, 0);
    lv_obj_set_style_pad_right(g_file_grid, USER_PAD_SM, 0);  /* 给滚动条留白 */
    lv_obj_set_style_bg_opa(g_file_grid, LV_OPA_TRANSP, 0);
    rebuild_file_grid();

    /* ============ 中间：上/下滚动箭头（圆形按钮，纵向居中） ============ */
    lv_obj_t *arrows = lv_obj_create(page);
    lv_obj_remove_style_all(arrows);
    lv_obj_set_width(arrows, LV_SIZE_CONTENT);
    lv_obj_set_height(arrows, LV_PCT(100));
    lv_obj_set_flex_flow(arrows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(arrows, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(arrows, USER_PAD, 0);
    lv_obj_set_style_bg_opa(arrows, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(arrows, LV_OBJ_FLAG_SCROLLABLE);
    make_round_icon_btn(arrows, LV_SYMBOL_UP, on_list_scroll_up);
    make_round_icon_btn(arrows, LV_SYMBOL_DOWN, on_list_scroll_down);

    /* ============ 右半：顶部 常驻供应度状态栏；下方 详情/空态 ============ */
    lv_obj_t *right = lv_obj_create(page);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 9);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, USER_PAD, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* 右半顶部：耗材/滤芯供应度（常驻，未选文件时也显示） */
    lv_obj_t *supply = lv_obj_create(right);
    lv_obj_remove_style_all(supply);
    lv_obj_set_width(supply, LV_PCT(100));
    lv_obj_set_height(supply, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(supply, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(supply, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(supply, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(supply, LV_OBJ_FLAG_SCROLLABLE);
    ui_user_make_supply_bar(supply, "Material", g_supply.material_ok != 0,
                            (int)g_supply.material_pct,
                            &g_supply_mat_bar, &g_supply_mat_pct, &g_supply_mat_status);
    ui_user_make_supply_bar(supply, "Filter", g_supply.filter_ok != 0,
                            (int)g_supply.filter_pct,
                            &g_supply_filt_bar, &g_supply_filt_pct, &g_supply_filt_status);

    /* 右半下方：详情面板（含未选时的空态提示） */
    build_detail_panel(right);
    update_detail_panel(nullptr);

    return page;
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
    if(g_door_close_poll_timer) {
        lv_timer_delete(g_door_close_poll_timer);
        g_door_close_poll_timer = nullptr;
    }
}

/* 未放入分支：开门 → 门已开启等待页 → 关门监测 Rsp → 关门确认页 */
void pf_on_close_door_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_DOOR_CONFIRM) {
        return;
    }
    /* 门已关：停轮询，切到"门已关好"可点确认态 */
    if(g_door_close_poll_timer) {
        lv_timer_delete(g_door_close_poll_timer);
        g_door_close_poll_timer = nullptr;
    }
    door_apply_variant(DoorVariant::DoorClosed);
}

/* DoorOpened 轮询：首次在最短停留后触发，之后每 DOOR_CLOSE_POLL_MS 发一次关门检测，
 * 直到后端回"门已关"（pf_on_close_door_done 停轮询并前进）。 */
void door_close_poll_cb(lv_timer_t *t)
{
    if(g_state != CONTROL_STATE_DOOR_CONFIRM) {
        return;   /* 状态已变，计时器由 stop_door_timers 清理 */
    }
    lv_timer_set_period(t, DOOR_CLOSE_POLL_MS);   /* 首次后转为轮询节奏 */
    backend_start_close_door(pf_on_close_door_done, nullptr);
}

void on_door_closed_confirm_btn(lv_event_t * /*e*/)
{
    if(g_state != CONTROL_STATE_DOOR_CONFIRM) {
        return;
    }
    if(g_door_origin == DoorFlowOrigin::HomeStandalone) {
        enter_state(CONTROL_STATE_IDLE);
    } else {
        goto_file_selection();
    }
}

/* Opening 最短停留到时 → 进入 DoorOpened 确认态 */
void door_advance_to_confirm_cb(lv_timer_t * /*t*/)
{
    g_door_advance_timer = nullptr;   /* one-shot：LVGL 回调后自删，仅清空句柄 */
    if(g_state == CONTROL_STATE_DOOR_OPENING) {
        enter_state(CONTROL_STATE_DOOR_CONFIRM);
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
        enter_state(CONTROL_STATE_DOOR_CONFIRM);
        return;
    }
    if(g_door_advance_timer) {
        lv_timer_delete(g_door_advance_timer);
    }
    g_door_advance_timer = lv_timer_create(door_advance_to_confirm_cb,
                                           DOOR_OPENING_MIN_MS - elapsed, nullptr);
    lv_timer_set_repeat_count(g_door_advance_timer, 1);
}

void pf_start_open_door(void)
{
    backend_start_open_door(pf_on_open_door_done, nullptr);
}

/* 清粉最短停留到时 → 发开门请求 */
void clean_advance_to_open_cb(lv_timer_t * /*t*/)
{
    g_door_advance_timer = nullptr;
    if(g_state == CONTROL_STATE_DOOR_OPENING) {
        pf_start_open_door();
    }
}

/* 清粉应答：满足最短停留才开门（不足则延时补足），保证"正在清粉中"可见 */
void pf_on_clean_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_DOOR_OPENING) {
        return;
    }
    const uint32_t elapsed = lv_tick_elaps(g_clean_since);
    if(elapsed >= CLEAN_MIN_MS) {
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
        backend_start_clean(pf_on_clean_done, nullptr);
        return;
    }
    pf_start_open_door();
}

void pf_on_print_progress(const control_progress_t *p, void * /*u*/)
{
    if(g_state == CONTROL_STATE_PRINTING) {
        page_user_home_printing_update(p);
    }
}

void pf_on_print_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    ui_toast_show("Print complete", user_color_status_ok, 2000);
    enter_state(CONTROL_STATE_IDLE);
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
    if(g_state != CONTROL_STATE_IDLE && g_state != CONTROL_STATE_PRINTING) {
        enter_state(CONTROL_STATE_IDLE);
    }
}

/* ============================================================================
 *  状态机
 * ============================================================================ */

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

    g_state = s;

    switch(s) {
    case CONTROL_STATE_IDLE:
        stop_print_watchdogs();
        backend_cancel_pending();
        page_user_home_exit_printing_mode();
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
        backend_start_pm25(pf_on_pm25_check_done, nullptr);
        break;

    case CONTROL_STATE_DOOR_CONFIRM:
        overlay_show();
        g_current_dlg = show_door_dialog(g_overlay, DoorVariant::DoorOpened);
        /* 最短停留后开始轮询关门检测（门真正关上才前进到"门已关好"） */
        g_door_close_poll_timer = lv_timer_create(door_close_poll_cb,
                                                  DOOR_OPENED_MIN_MS, nullptr);
        break;

    case CONTROL_STATE_PREPARE_WAIT:
        overlay_show();
        show_prepare_dlg();
        break;

    case CONTROL_STATE_PRINTING:
        overlay_hide();
        g_home_section_view = "user_home";   /* 打印中首页图标回仪表盘 */
        ui::PageManager::instance().switch_to("user_home");
        backend_start_job(&g_active_file,
                                     pf_on_print_progress,
                                     pf_on_print_done,
                                     nullptr);
        page_user_home_enter_printing_mode(&g_active_file);
        start_print_peer_watch();
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

void control_init(lv_obj_t *parent)
{
    if(g_overlay) {
        return;
    }
    if(!parent) {
        parent = lv_screen_active();
    }
    wire_msg_proxy();
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
    /* Figma 入口：先弹「是否已放入打印版」两卡片预确认，再进入文件选择 */
    g_door_origin = DoorFlowOrigin::PrintPrep;
    enter_state(CONTROL_STATE_PLATE_PROMPT);
}

void control_open_door_request(void)
{
    if(g_state != CONTROL_STATE_IDLE) {
        return;
    }
    g_door_origin = DoorFlowOrigin::HomeStandalone;
    enter_state(CONTROL_STATE_DOOR_OPENING);
}

const char *control_home_section_target(void)
{
    return g_home_section_view;
}

/* ---- 选择文件页（PageManager 隐藏页 "user_files"）生命周期 ---- */
lv_obj_t *page_user_files_create(lv_obj_t *parent)
{
    /* 仅首次创建时复位到本机/未选；之后切走再回来由 PageManager 隐藏/显示保留状态 */
    g_file_tab = FileTab::Local;
    g_sel_index = -1;
    return show_file_select(parent);
}

void page_user_files_refresh(bool /*force*/)
{
    /* on_enter：标记可见；Local 读 config.json，USB 走 Fs_DirOpen；选中项按名恢复 */
    g_files_page_active = true;
    g_dir_open_pending.clear();
    request_fs_dir_open(g_file_tab);
    ui::UiMsgProxy::instance().send_supply_status();
}

void page_user_files_exit(void)
{
    g_files_page_active = false;
    g_dir_open_pending.clear();
    close_confirm_dlg();
}

void control_cancel(void)
{
    enter_state(CONTROL_STATE_IDLE);
}

bool control_is_printing(void)
{
    return g_state == CONTROL_STATE_PRINTING;
}

void control_terminate_request(void)
{
    if(g_state != CONTROL_STATE_PRINTING) {
        return;
    }
    /* 与删除确认同款风格（用户层 ui_modal_dialog）。打印中 overlay 可能隐藏，
     * 故挂到 top layer 确保始终可见。 */
    open_user_confirm_dialog(lv_layer_top(), "Terminate Print?",
                             "Are you sure you want to terminate the current print job? "
                             "Progress cannot be recovered.",
                             "Cancel", "Terminate", ui_color_danger,
                             on_terminate_cancel, on_terminate_confirm);
}
