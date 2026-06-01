#include "control.hpp"
#include "ui_msg_proxy.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_user_widgets.hpp"
#include "ui_slide_unlock.hpp"
#include "ui_board_anim.hpp"
#include "ui_arc_spinner.hpp"

#include "page_user_home.hpp"
#include "page_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

/* ============================================================================
 *  状态机全局
 * ============================================================================ */

using DoneCb = void (*)(void *user_data);

lv_obj_t *g_overlay = nullptr;
control_state_t g_state = CONTROL_STATE_IDLE;
ui_modal_dialog_t *g_current_dlg = nullptr;
control_file_t g_active_file = {};
lv_timer_t *g_purify_ui_tick = nullptr;

/* ---------- 后端异步状态（UiMsgProxy 适配，仅本文件使用） ---------- */
bool  g_proxy_wired = false;
bool  g_pm25_high = false;
int   g_purify_progress = 0;
bool  g_job_running = false;

DoneCb g_pm25_cb = nullptr;
void  *g_pm25_u = nullptr;
DoneCb g_open_cb = nullptr;
void  *g_open_u = nullptr;
DoneCb g_close_cb = nullptr;
void  *g_close_u = nullptr;
DoneCb g_purify_cb = nullptr;
void  *g_purify_u = nullptr;

using ProgressCb = void (*)(const control_progress_t *p, void *user_data);
ProgressCb g_job_prog_cb = nullptr;
DoneCb     g_job_done_cb = nullptr;
void      *g_job_u = nullptr;

void (*g_on_action_fail)(const char *message) = nullptr;

std::vector<control_file_t> g_files;
std::vector<control_file_t> g_files_snapshot;
bool g_files_frozen = false;

void enter_state(control_state_t s);
void start_purify_ui_tick(void);
void stop_purify_ui_tick(void);

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
    return g_files_frozen ? g_files_snapshot : g_files;
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
            ui_toast_show("Door check failed, please close the door",
                          user_color_status_err, 2500);
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

    proxy.register_fs_dir([](const middleware::FileEntryPayload *files, uint32_t count) {
        if(g_files_frozen) {
            return;
        }
        g_files.clear();
        if(!files || count == 0) {
            return;
        }
        for(uint32_t i = 0; i < count; i++) {
            control_file_t f = {};
            copy_file_entry(f, files[i]);
            g_files.push_back(f);
        }
    });

    proxy.register_print_progress([](const middleware::PrintProgressPayload &p) {
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
        if(p.progress_percent >= 100 && g_job_running) {
            g_job_running = false;
            invoke_done(g_job_done_cb, g_job_u);
        }
    });
}

void backend_reset_session()
{
    g_pm25_high = false;
    g_purify_progress = 0;
    g_job_running = false;
    ui::UiMsgProxy::instance().send_fs_dir_open();
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
    g_job_running = false;
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

void backend_start_purify(DoneCb cb, void *u)
{
    g_purify_progress = 0;
    g_purify_cb = cb;
    g_purify_u = u;
    ui::UiMsgProxy::instance().send_purify();
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

void backend_freeze_files()
{
    g_files_snapshot = g_files;
    g_files_frozen = true;
}

void backend_unfreeze_files()
{
    g_files_frozen = false;
    g_files_snapshot.clear();
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

void backend_get_supply_status(bool *material_ready, bool *hepa_ready)
{
    if(material_ready) {
        *material_ready = true;
    }
    if(hepa_ready) {
        *hepa_ready = true;
    }
}

void backend_start_job(const control_file_t *file, ProgressCb prog_cb, DoneCb done_cb, void *u)
{
    g_job_prog_cb = prog_cb;
    g_job_done_cb = done_cb;
    g_job_u = u;
    g_job_running = true;

    const uint32_t file_index = file ? file->index : 0;
    ui::UiMsgProxy::instance().send_print_start(file_index);
}

void backend_terminate_job()
{
    g_job_running = false;
    g_job_prog_cb = nullptr;
    g_job_done_cb = nullptr;
    g_job_u = nullptr;
    ui::UiMsgProxy::instance().send_print_terminate();
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
}

/* ============================================================================
 *  D2：PM2.5 检测 / 净化弹窗 UI
 * ============================================================================ */

enum class DoorStepVariant { Check, Purify };

struct door_step_state_t {
    lv_obj_t         *body_lbl;
    lv_obj_t         *visual_container;
    ui_arc_spinner_t *spinner;
    lv_obj_t         *bar;
    DoorStepVariant   variant;
};

void door_step_teardown_visuals(door_step_state_t *st)
{
    if(!st) {
        return;
    }
    if(st->spinner) {
        ui_arc_spinner_destroy(st->spinner);
        st->spinner = nullptr;
    }
    if(st->bar) {
        lv_obj_delete(st->bar);
        st->bar = nullptr;
    }
}

void door_step_build_check_visual(door_step_state_t *st)
{
    door_step_teardown_visuals(st);
    st->spinner = ui_arc_spinner_create(st->visual_container, USER_SPINNER_SZ);
    if(st->spinner) {
        lv_obj_center(st->spinner->root);
    }
}

void door_step_build_purify_visual(door_step_state_t *st)
{
    door_step_teardown_visuals(st);
    lv_obj_t *bar = ui_user_make_progress_bar(st->visual_container,
                                              USER_PROGRESS_W, USER_PROGRESS_H);
    lv_obj_center(bar);
    st->bar = bar;
}

void door_step_apply_variant(ui_modal_dialog_t *dlg,
                             door_step_state_t *st,
                             DoorStepVariant v)
{
    st->variant = v;
    if(v == DoorStepVariant::Check) {
        ui_modal_dialog_set_title(dlg, "Preparing to Open");
        if(st->body_lbl) {
            lv_label_set_text(st->body_lbl,
                              "Checking PM2.5, please do not operate the device");
        }
        door_step_build_check_visual(st);
    } else {
        ui_modal_dialog_set_title(dlg, "PM2.5 Level Too High");
        if(st->body_lbl) {
            lv_label_set_text(st->body_lbl, "Purifying air automatically");
        }
        door_step_build_purify_visual(st);
    }
}

void door_step_state_free(lv_event_t *e)
{
    auto *st = static_cast<door_step_state_t *>(lv_event_get_user_data(e));
    if(st) {
        door_step_teardown_visuals(st);
        delete st;
    }
}

ui_modal_dialog_t *show_door_step(lv_obj_t *parent, DoorStepVariant variant)
{
    const char *title = (variant == DoorStepVariant::Check)
                        ? "Preparing to Open" : "PM2.5 Level Too High";
    const ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(title, USER_DLG_W, false);
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }

    auto *st = new door_step_state_t{};
    st->body_lbl = ui_user_make_body_label(dlg->body,
        (variant == DoorStepVariant::Check)
        ? "Checking PM2.5, please do not operate the device"
        : "Purifying air automatically");

    lv_obj_t *vc = lv_obj_create(dlg->body);
    lv_obj_remove_style_all(vc);
    lv_obj_set_size(vc, USER_DLG_W - USER_DLG_PAD * 2, USER_SPINNER_SZ + USER_PAD);
    lv_obj_set_style_bg_opa(vc, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vc, LV_OBJ_FLAG_SCROLLABLE);
    st->visual_container = vc;

    door_step_apply_variant(dlg, st,
        variant == DoorStepVariant::Check ? DoorStepVariant::Check
                                          : DoorStepVariant::Purify);

    lv_obj_set_user_data(dlg->root, st);
    lv_obj_add_event_cb(dlg->root, door_step_state_free, LV_EVENT_DELETE, st);
    return dlg;
}

void door_step_set_progress(ui_modal_dialog_t *dlg, int percent)
{
    if(!dlg || !dlg->root) {
        return;
    }
    auto *st = static_cast<door_step_state_t *>(lv_obj_get_user_data(dlg->root));
    if(!st || !st->bar) {
        return;
    }
    if(percent < 0)   percent = 0;
    if(percent > 100) percent = 100;
    lv_bar_set_value(st->bar, percent, LV_ANIM_ON);
}

void door_step_switch_to_purify(ui_modal_dialog_t *dlg)
{
    if(!dlg || !dlg->root) {
        return;
    }
    auto *st = static_cast<door_step_state_t *>(lv_obj_get_user_data(dlg->root));
    if(!st) {
        return;
    }
    door_step_apply_variant(dlg, st, DoorStepVariant::Purify);
}

/* ============================================================================
 *  D1 / D4 / 文件选择 / 文件确认 弹窗 UI
 * ============================================================================ */

void on_insert_unlocked(void * /*u*/)
{
    if(g_state == CONTROL_STATE_D1_INSERT_PROMPT) {
        enter_state(CONTROL_STATE_D2_DOOR_OPENING);
    }
}

void on_insert_shortcut(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_D1_INSERT_PROMPT) {
        enter_state(CONTROL_STATE_D4_DOOR_CLOSE_CHECK);
    }
}

ui_modal_dialog_t *show_insert_prompt(lv_obj_t *parent)
{
    const ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Insert Print Board?", USER_DLG_W, false);
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }

    ui_board_anim_create(dlg->body,
                         USER_DLG_W - USER_DLG_PAD * 2,
                         USER_DLG_BODY_MIN_H);

    ui_slide_unlock_cfg_t slide_cfg = {};
    slide_cfg.hint_text = "Slide to Unlock  " LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT;
    slide_cfg.on_unlocked = on_insert_unlocked;
    slide_cfg.user_data = dlg;
    ui_slide_unlock_create(dlg->body, &slide_cfg);

    ui_user_make_ghost_link_btn(dlg->body,
                                "Board inserted, start printing " LV_SYMBOL_RIGHT,
                                on_insert_shortcut);
    return dlg;
}

ui_modal_dialog_t *show_door_close_check(lv_obj_t *parent)
{
    const ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Checking Door", USER_DLG_W, false);
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }

    ui_user_make_body_label(dlg->body,
                            "Verifying the door is closed and locked");

    lv_obj_t *vc = ui_user_make_spinner_container(dlg->body, USER_SPINNER_SZ);
    ui_arc_spinner_t *sp = ui_arc_spinner_create(vc, USER_SPINNER_SZ);
    if(sp && sp->root) {
        lv_obj_center(sp->root);
    }
    return dlg;
}

void on_file_card_click(lv_event_t *e)
{
    const size_t index =
        static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    const control_file_t *file = backend_get_file(index);
    if(!file || g_state != CONTROL_STATE_FILE_SELECT) {
        return;
    }
    g_active_file = *file;
    enter_state(CONTROL_STATE_FILE_CONFIRM);
}

void on_file_select_cancel(lv_event_t * /*e*/)
{
    enter_state(CONTROL_STATE_IDLE);
}

lv_obj_t *make_file_card(lv_obj_t *parent, const control_file_t *file, size_t index)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, USER_FILE_CARD_W, USER_FILE_CARD_H);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, user_color_panel_alt, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_border_color(card, user_color_accent, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(card, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, on_file_card_click, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));

    lv_obj_t *thumb = ui_user_make_accent_preview(card, LV_PCT(100), USER_FILE_THUMB_H);

    lv_obj_t *type_lbl = lv_label_create(thumb);
    lv_label_set_text(type_lbl, ".gcode");
    lv_obj_set_style_text_color(type_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(type_lbl, user_font_text(), 0);
    lv_obj_set_style_text_opa(type_lbl, LV_OPA_80, 0);
    lv_obj_align(type_lbl, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_SM, -USER_PAD_SM);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, file->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);

    char info[64];
    ui_user_format_file_meta(info, sizeof(info),
                             file->duration_min, file->material_g);
    lv_obj_t *meta = lv_label_create(card);
    lv_label_set_text(meta, info);
    lv_obj_set_style_text_color(meta, user_color_text_dim, 0);
    lv_obj_set_style_text_font(meta, user_font_caption(), 0);

    return card;
}

ui_modal_dialog_t *show_file_select(lv_obj_t *parent)
{
    const ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Select Print File", USER_FILE_SEL_W, true);
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }

    lv_obj_t *grid = lv_obj_create(dlg->body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, USER_FILE_SEL_H);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, USER_PAD, 0);
    lv_obj_set_style_pad_row(grid, USER_PAD, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);

    const size_t n = backend_file_count();
    if(n == 0) {
        lv_obj_t *empty = ui_user_make_body_label(grid, "No print files available");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
    } else {
        for(size_t i = 0; i < n; i++) {
            const control_file_t *file = backend_get_file(i);
            if(file) {
                make_file_card(grid, file, i);
            }
        }
    }

    ui_user_make_footer_ghost_btn(dlg->footer, "Cancel", on_file_select_cancel);
    return dlg;
}

void on_file_confirm_cancel(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_FILE_CONFIRM) {
        enter_state(CONTROL_STATE_FILE_SELECT);
    }
}

void on_file_confirm_ok(lv_event_t * /*e*/)
{
    if(g_state == CONTROL_STATE_FILE_CONFIRM) {
        enter_state(CONTROL_STATE_PRINTING);
    }
}

ui_modal_dialog_t *show_file_confirm(lv_obj_t *parent, const control_file_t *file)
{
    if(!file) {
        return nullptr;
    }

    const ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Confirm Print File", USER_FILE_DLG_W, true);
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }

    lv_obj_t *meta_row = lv_obj_create(dlg->body);
    lv_obj_remove_style_all(meta_row);
    lv_obj_set_width(meta_row, LV_PCT(100));
    lv_obj_set_height(meta_row, USER_FILE_DLG_PREVIEW_H);
    lv_obj_set_flex_flow(meta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(meta_row, USER_PAD, 0);
    lv_obj_set_style_bg_opa(meta_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(meta_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *preview = ui_user_make_accent_preview(meta_row,
        USER_FILE_DLG_PREVIEW_W, USER_FILE_DLG_PREVIEW_H);

    lv_obj_t *preview_icon = lv_label_create(preview);
    lv_label_set_text(preview_icon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(preview_icon, user_color_text, 0);
    lv_obj_set_style_text_font(preview_icon, user_font_value(), 0);
    lv_obj_center(preview_icon);

    lv_obj_t *info_col = lv_obj_create(meta_row);
    lv_obj_remove_style_all(info_col);
    lv_obj_set_size(info_col, LV_PCT(100), USER_FILE_DLG_PREVIEW_H);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_col, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(info_col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(info_col, LV_OBJ_FLAG_SCROLLABLE);

    auto make_info_pair = [&](const char *caption, const char *value, lv_color_t color) {
        lv_obj_t *cap = lv_label_create(info_col);
        lv_label_set_text(cap, caption);
        lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
        lv_obj_set_style_text_font(cap, user_font_caption(), 0);

        lv_obj_t *val = lv_label_create(info_col);
        lv_label_set_text(val, value);
        lv_obj_set_style_text_color(val, color, 0);
        lv_obj_set_style_text_font(val, user_font_text(), 0);
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(val, LV_PCT(100));
    };

    make_info_pair("File", file->name, user_color_text);
    char dur[32];
    ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
    make_info_pair("Est. Print Time", dur, user_color_text);
    char mat[32];
    std::snprintf(mat, sizeof(mat), "%.1f g", (double)file->material_g);
    make_info_pair("Material Usage", mat, user_color_accent);

    lv_obj_t *supply_row = lv_obj_create(dlg->body);
    lv_obj_remove_style_all(supply_row);
    lv_obj_set_width(supply_row, LV_PCT(100));
    lv_obj_set_height(supply_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(supply_row, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(supply_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(supply_row, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(supply_row, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(supply_row, USER_PAD_LG, 0);
    lv_obj_set_flex_flow(supply_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(supply_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(supply_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(supply_row);
    lv_label_set_text(label, "Supply Check");
    lv_obj_set_style_text_color(label, user_color_text_dim, 0);
    lv_obj_set_style_text_font(label, user_font_caption(), 0);

    bool material_ready = false;
    bool hepa_ready = false;
    backend_get_supply_status(&material_ready, &hepa_ready);
    ui_user_make_supply_indicator(supply_row, "Material", material_ready);
    ui_user_make_supply_indicator(supply_row, "HEPA", hepa_ready);

    ui_user_make_footer_ghost_btn(dlg->footer, "Cancel", on_file_confirm_cancel);
    lv_obj_t *btn_ok = lv_button_create(dlg->footer);
    lv_obj_add_style(btn_ok, user_style_btn_primary(), 0);
    lv_obj_set_size(btn_ok, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    const bool all_ready = material_ready && hepa_ready;
    if(!all_ready) {
        lv_obj_add_state(btn_ok, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(btn_ok, on_file_confirm_ok, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "Confirm Print");
    lv_obj_set_style_text_color(lbl_ok, user_color_text, 0);
    lv_obj_set_style_text_font(lbl_ok, user_font_text(), 0);
    lv_obj_center(lbl_ok);

    return dlg;
}

/* ============================================================================
 *  后端回调 → 状态推进
 * ============================================================================ */

bool is_door_prep_state(void)
{
    return g_state == CONTROL_STATE_D2_DOOR_OPENING ||
           g_state == CONTROL_STATE_D2_PURIFYING;
}

void pf_start_open_door_then_close_check(void)
{
    backend_start_open_door([](void * /*u*/) {
        if(is_door_prep_state()) {
            enter_state(CONTROL_STATE_D4_DOOR_CLOSE_CHECK);
        }
    }, nullptr);
}

void pf_on_pm25_check_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_D2_DOOR_OPENING) {
        return;
    }
    if(g_pm25_high) {
        g_state = CONTROL_STATE_D2_PURIFYING;
        if(g_current_dlg) {
            door_step_switch_to_purify(g_current_dlg);
        }
        start_purify_ui_tick();
        backend_start_purify([](void *) {
            if(g_state == CONTROL_STATE_D2_PURIFYING) {
                stop_purify_ui_tick();
                pf_start_open_door_then_close_check();
            }
        }, nullptr);
        return;
    }
    pf_start_open_door_then_close_check();
}

void pf_on_door_close_check_done(void * /*u*/)
{
    if(g_state != CONTROL_STATE_D4_DOOR_CLOSE_CHECK) {
        return;
    }
    enter_state(CONTROL_STATE_FILE_SELECT);
}

void pf_purify_tick(lv_timer_t *t)
{
    if(g_state != CONTROL_STATE_D2_PURIFYING || !g_current_dlg) {
        lv_timer_delete(t);
        return;
    }
    door_step_set_progress(g_current_dlg,
                           g_purify_progress);
    if(g_purify_progress >= 100) {
        lv_timer_delete(t);
    }
}

void start_purify_ui_tick(void)
{
    if(g_purify_ui_tick) {
        lv_timer_delete(g_purify_ui_tick);
    }
    g_purify_ui_tick = lv_timer_create(pf_purify_tick, 80, nullptr);
}

void stop_purify_ui_tick(void)
{
    if(g_purify_ui_tick) {
        lv_timer_delete(g_purify_ui_tick);
        g_purify_ui_tick = nullptr;
    }
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

void terminate_confirm_yes(void * /*u*/)
{
    backend_terminate_job();
    if(g_state == CONTROL_STATE_PRINTING) {
        ui_toast_show("Print terminated", user_color_status_err, 2000);
        enter_state(CONTROL_STATE_IDLE);
    }
}

void terminate_confirm_no(void * /*u*/) {}

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
    stop_purify_ui_tick();
    close_current_dialog();

    g_state = s;

    switch(s) {
    case CONTROL_STATE_IDLE:
        backend_cancel_pending();
        backend_unfreeze_files();
        page_user_home_exit_printing_mode();
        overlay_hide();
        break;

    case CONTROL_STATE_D1_INSERT_PROMPT:
        overlay_show();
        g_current_dlg = show_insert_prompt(g_overlay);
        break;

    case CONTROL_STATE_D2_DOOR_OPENING:
        overlay_show();
        g_current_dlg = show_door_step(g_overlay, DoorStepVariant::Check);
        backend_start_pm25(pf_on_pm25_check_done, nullptr);
        break;

    case CONTROL_STATE_D2_PURIFYING:
        overlay_show();
        if(!g_current_dlg) {
            g_current_dlg = show_door_step(g_overlay, DoorStepVariant::Purify);
        } else {
            door_step_switch_to_purify(g_current_dlg);
        }
        start_purify_ui_tick();
        break;

    case CONTROL_STATE_D4_DOOR_CLOSE_CHECK:
        overlay_show();
        g_current_dlg = show_door_close_check(g_overlay);
        backend_start_close_door(pf_on_door_close_check_done, nullptr);
        break;

    case CONTROL_STATE_FILE_SELECT:
        overlay_show();
        backend_freeze_files();
        g_current_dlg = show_file_select(g_overlay);
        break;

    case CONTROL_STATE_FILE_CONFIRM:
        backend_unfreeze_files();
        overlay_show();
        g_current_dlg = show_file_confirm(g_overlay, &g_active_file);
        break;

    case CONTROL_STATE_PRINTING:
        backend_unfreeze_files();
        overlay_hide();
        ui::PageManager::instance().switch_to("user_home");
        page_user_home_enter_printing_mode(&g_active_file);
        backend_start_job(&g_active_file,
                                     pf_on_print_progress,
                                     pf_on_print_done,
                                     nullptr);
        break;
    }
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
    enter_state(CONTROL_STATE_D1_INSERT_PROMPT);
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
    ui_confirm_show("Terminate Print",
                    "Are you sure you want to terminate the current print job? "
                    "Progress cannot be recovered.",
                    "Terminate", "Cancel",
                    terminate_confirm_yes, terminate_confirm_no, nullptr);
}
