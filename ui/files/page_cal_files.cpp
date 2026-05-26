#include "page_cal_files.hpp"
#include "files_open.hpp"
#include "ctrl_backend.hpp"
#include "job_status.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"

#include <cstdio>
#include <cstring>

static lv_obj_t *g_cal_list = nullptr;
static lv_obj_t *g_cal_detail = nullptr;
static lv_obj_t *g_lbl_cal_name = nullptr;
static lv_obj_t *g_lbl_cal_size = nullptr;
static lv_obj_t *g_lbl_cal_grid = nullptr;
static lv_obj_t *g_lbl_cal_gains = nullptr;
static lv_obj_t *g_lbl_cal_status = nullptr;
static lv_obj_t *g_btn_cal_open = nullptr;
static lv_obj_t *g_cal_open_lbl = nullptr;
static lv_obj_t *g_btn_cal_load = nullptr;
static lv_obj_t *g_btn_cal_delete = nullptr;

static gcode_file_list_t g_cal_file_list;
static int g_cal_selected_idx = -1;
static int g_cal_loaded_idx = -1;
static char s_cal_delete_name[GCODE_NAME_LEN];
static void (*s_back_cb)(void) = nullptr;

void page_cal_files_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

static void format_size(long size, char *out, size_t out_size)
{
    if(size < 1024) {
        snprintf(out, out_size, "%ld B", size);
    } else if(size < 1024 * 1024) {
        snprintf(out, out_size, "%.2f KB", size / 1024.0);
    } else {
        snprintf(out, out_size, "%.2f MB", size / (1024.0 * 1024.0));
    }
}

static void set_btn_selected_style(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn,
        selected ? ui_color_accent_dim : ui_color_panel_alt, 0);
    lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn,
        selected ? ui_color_accent : ui_color_border, 0);
}

static void set_open_btn_enabled(bool enabled)
{
    if(!g_btn_cal_open) {
        return;
    }
    if(enabled) {
        lv_obj_remove_state(g_btn_cal_open, LV_STATE_DISABLED);
        lv_obj_add_flag(g_btn_cal_open, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(g_btn_cal_open, LV_STATE_DISABLED);
        lv_obj_clear_flag(g_btn_cal_open, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void set_open_btn_caption(const char *caption)
{
    if(g_cal_open_lbl && caption) {
        lv_label_set_text(g_cal_open_lbl, caption);
    }
}

/** 当前选中 CAL 是否已通过 Open 加载 */
static bool is_selected_cal_opened(void)
{
    if(g_cal_selected_idx < 0 || g_cal_selected_idx >= g_cal_file_list.count) {
        return false;
    }
    const char *name = g_cal_file_list.files[g_cal_selected_idx].name;
    bool loaded = false;
    {
        JOB_LOCK();
        loaded = g_job_status.cal_file_loaded &&
                 (strcmp(g_job_status.cal_file, name) == 0);
    }
    return loaded;
}

static void cal_copy_progress_cb(long done, long total, void *)
{
    char status[96];
    if(total > 0) {
        snprintf(status, sizeof(status),
                 LV_SYMBOL_LOOP " Copying to /home/CAL: %ld%%",
                 done * 100 / total);
    } else {
        snprintf(status, sizeof(status),
                 LV_SYMBOL_LOOP " Copying to /home/CAL...");
    }
    ui_status_set(g_lbl_cal_status, status, ui_color_accent);
    lv_timer_handler();
}

static void update_cal_action_buttons(void)
{
    const bool has_sel = (g_cal_selected_idx >= 0 && g_cal_selected_idx < g_cal_file_list.count);
    const bool usb = (cal_get_storage_kind() == FILES_STORAGE_USB);
    const bool locked = (ctrl_is_sending() != 0);
    const bool opened = is_selected_cal_opened();
    const bool enabled = has_sel && !opened && !locked;

    ui_action_btn_set_state(g_btn_cal_load, usb, enabled && usb, nullptr);
    ui_action_btn_set_state(g_btn_cal_delete, !usb, enabled && !usb, nullptr);
}

static void update_cal_list_selection(void)
{
    const uint32_t cnt = lv_obj_get_child_count(g_cal_list);
    for(uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(g_cal_list, i);
        if(!lv_obj_check_type(btn, &lv_button_class)) {
            continue;
        }
        set_btn_selected_style(btn, (int)i == g_cal_selected_idx);
    }
}

static void update_cal_open_button(void)
{
    if(g_cal_selected_idx < 0 || g_cal_selected_idx >= g_cal_file_list.count) {
        set_open_btn_enabled(false);
        set_open_btn_caption(LV_SYMBOL_PLAY "  Open");
        return;
    }
    if(ctrl_is_sending()) {
        set_open_btn_enabled(false);
        set_open_btn_caption(LV_SYMBOL_STOP "  Locked");
        return;
    }
    set_open_btn_enabled(true);
    if(g_cal_loaded_idx == g_cal_selected_idx) {
        set_open_btn_caption(LV_SYMBOL_OK "  Loaded");
    } else {
        set_open_btn_caption(LV_SYMBOL_PLAY "  Open");
    }
}

static void cal_set_io_buttons_locked(bool locked)
{
    if(locked) {
        const bool usb = (cal_get_storage_kind() == FILES_STORAGE_USB);
        ui_action_btn_set_state(g_btn_cal_load, usb, false, nullptr);
        ui_action_btn_set_state(g_btn_cal_delete, !usb, false, nullptr);
        set_open_btn_enabled(false);
        return;
    }
    update_cal_open_button();
    update_cal_action_buttons();
}

static void refresh_cal_detail(void)
{
    update_cal_open_button();
    update_cal_action_buttons();

    if(g_cal_selected_idx < 0 || g_cal_selected_idx >= g_cal_file_list.count) {
        lv_label_set_text(g_lbl_cal_name, "No file selected");
        lv_label_set_text(g_lbl_cal_size, "");
        lv_label_set_text(g_lbl_cal_grid, "");
        lv_label_set_text(g_lbl_cal_gains, "");
        ui_status_set(g_lbl_cal_status, "Select a .csv file and tap Open", ui_color_text_dim);
        if(g_cal_detail) {
            lv_obj_set_style_border_width(g_cal_detail, 0, 0);
        }
        return;
    }

    const gcode_file_info_t *info = &g_cal_file_list.files[g_cal_selected_idx];
    char line[320];
    char size_raw[48];

    snprintf(line, sizeof(line), "File: %s", info->name);
    lv_label_set_text(g_lbl_cal_name, line);

    format_size(info->size, size_raw, sizeof(size_raw));
    snprintf(line, sizeof(line), "Size: %s", size_raw);
    lv_label_set_text(g_lbl_cal_size, line);

    bool loaded_this = false;
    bool cal_loaded = false;
    bool has_gains = false;
    int nx = 0;
    int ny = 0;
    float scale = 0.f;
    float range = 0.f;
    float speed = 0.f;
    char loaded_name[256];

    {
        JOB_LOCK();
        cal_loaded = g_job_status.cal_file_loaded;
        has_gains = g_job_status.cal_has_gains;
        nx = g_job_status.cal_grid_nx;
        ny = g_job_status.cal_grid_ny;
        scale = g_job_status.cal_image_scale_gain;
        range = g_job_status.cal_image_rang_gain;
        speed = g_job_status.cal_speed_gain;
        strncpy(loaded_name, g_job_status.cal_file, sizeof(loaded_name) - 1);
        loaded_name[sizeof(loaded_name) - 1] = '\0';
    }

    loaded_this = cal_loaded && (strcmp(loaded_name, info->name) == 0);
    if(loaded_this && nx > 0 && ny > 0) {
        snprintf(line, sizeof(line), "Grid: %d x %d", nx, ny);
        lv_label_set_text(g_lbl_cal_grid, line);
    } else {
        lv_label_set_text(g_lbl_cal_grid, "Grid: --");
    }

    if(loaded_this && has_gains) {
        snprintf(line, sizeof(line), "CAL gains: scale %.3f  range %.3f  speed %.3f",
                 scale, range, speed);
        lv_label_set_text(g_lbl_cal_gains, line);
    } else if(loaded_this) {
        lv_label_set_text(g_lbl_cal_gains, "CAL gains: (no CAL row in file)");
    } else {
        lv_label_set_text(g_lbl_cal_gains, "CAL gains: --");
    }

    if(loaded_this) {
        ui_status_set(g_lbl_cal_status,
                      LV_SYMBOL_OK " Loaded — enable calibration in Control if needed",
                      ui_color_accent);
        if(g_cal_detail) {
            lv_obj_set_style_border_width(g_cal_detail, 2, 0);
            lv_obj_set_style_border_color(g_cal_detail, ui_color_accent, 0);
        }
        g_cal_loaded_idx = g_cal_selected_idx;
    } else {
        ui_status_set(g_lbl_cal_status, "Selected — tap Open to load calibration", ui_color_text_dim);
        if(g_cal_detail) {
            lv_obj_set_style_border_width(g_cal_detail, 0, 0);
        }
    }
}

static void on_cal_file_btn_click(lv_event_t *e)
{
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= g_cal_file_list.count) {
        return;
    }
    g_cal_selected_idx = idx;
    update_cal_list_selection();
    refresh_cal_detail();
}

static void rebuild_cal_file_list_ui(void)
{
    lv_obj_clean(g_cal_list);
    cal_scan_files(&g_cal_file_list);

    if(g_cal_file_list.count == 0) {
        lv_obj_t *lbl = lv_label_create(g_cal_list);
        lv_label_set_text(lbl, "No .csv files found");
        lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
        g_cal_selected_idx = -1;
        refresh_cal_detail();
        return;
    }

    for(int i = 0; i < g_cal_file_list.count; i++) {
        lv_obj_t *btn = lv_button_create(g_cal_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, UI_LIST_ITEM_H);
        lv_obj_add_event_cb(btn, on_cal_file_btn_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_set_style_radius(btn, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(btn, UI_PAD_SM, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 8, 0);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, LV_SYMBOL_FILE);
        lv_obj_set_style_text_font(icon, ui_font_icon(), 0);
        lv_obj_set_style_text_color(icon, ui_color_accent, 0);
        lv_obj_set_width(icon, 36);

        char size_buf[48];
        format_size(g_cal_file_list.files[i].size, size_buf, sizeof(size_buf));

        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text_fmt(name, "%s  (%s)", g_cal_file_list.files[i].name, size_buf);
        lv_obj_set_style_text_color(name, ui_color_text, 0);
        lv_obj_set_flex_grow(name, 1);
    }

    g_cal_selected_idx = 0;
    update_cal_list_selection();
    refresh_cal_detail();
}

static void on_cal_refresh_click(lv_event_t *)
{
    rebuild_cal_file_list_ui();
}

static void on_cal_open_click(lv_event_t *)
{
    if(g_cal_selected_idx < 0 || g_cal_selected_idx >= g_cal_file_list.count) {
        ui_toast_show("Please select a CAL file first", ui_color_danger, 1800);
        return;
    }
    if(ctrl_is_sending()) {
        ui_toast_show("Stop the job before loading CAL file", ui_color_danger, 2500);
        refresh_cal_detail();
        return;
    }

    char err[128];
    err[0] = '\0';
    const char *name = g_cal_file_list.files[g_cal_selected_idx].name;
    if(cal_load_file(name, err, sizeof(err)) != 0) {
        ui_toast_show(err[0] ? err : "Failed to load CAL file", ui_color_danger, 3500);
        refresh_cal_detail();
        return;
    }

    g_cal_loaded_idx = g_cal_selected_idx;

    bool cal_has_gains = false;
    {
        JOB_LOCK();
        cal_has_gains = g_job_status.cal_has_gains;
    }
    if(!cal_has_gains) {
        ui_toast_show(
            "CAL format error: need CAL,scale,range,speed row (Control cannot enable)",
            ui_color_danger, 4500);
    } else {
        ui_toast_show("CAL file loaded", ui_color_accent, 2000);
    }
    refresh_cal_detail();
}

static void on_cal_load_click(lv_event_t *)
{
    if(g_cal_selected_idx < 0 || g_cal_selected_idx >= g_cal_file_list.count) {
        ui_toast_show("Please select a CAL file first", ui_color_danger, 1800);
        return;
    }
    if(is_selected_cal_opened()) {
        ui_toast_show("File is open — Load unavailable", ui_color_danger, 2500);
        return;
    }

    const char *name = g_cal_file_list.files[g_cal_selected_idx].name;
    cal_set_io_buttons_locked(true);
    ui_status_set(g_lbl_cal_status, LV_SYMBOL_LOOP " Copying to /home/CAL...", ui_color_accent);
    lv_timer_handler();

    char err[128];
    err[0] = '\0';
    const int rc = cal_copy_usb_to_local(name, err, sizeof(err),
                                         cal_copy_progress_cb, nullptr);
    cal_set_io_buttons_locked(false);
    if(rc != 0) {
        ui_toast_show(err[0] ? err : "Copy failed", ui_color_danger, 3000);
        refresh_cal_detail();
        return;
    }

    ui_toast_show("Copied to /home/CAL", ui_color_accent, 2200);
    refresh_cal_detail();
}

static void cal_delete_confirmed(void *)
{
    const char *name = s_cal_delete_name;
    cal_set_io_buttons_locked(true);
    ui_status_set(g_lbl_cal_status, LV_SYMBOL_TRASH " Deleting file...", ui_color_text_dim);
    lv_timer_handler();

    char err[128];
    err[0] = '\0';
    const int rc = cal_delete_local_file(name, err, sizeof(err));
    cal_set_io_buttons_locked(false);
    if(rc != 0) {
        ui_toast_show(err[0] ? err : "Delete failed", ui_color_danger, 3000);
        refresh_cal_detail();
        return;
    }

    ui_toast_show("File deleted", ui_color_accent, 1800);
    rebuild_cal_file_list_ui();
}

static void on_cal_delete_click(lv_event_t *)
{
    if(g_cal_selected_idx < 0 || g_cal_selected_idx >= g_cal_file_list.count) {
        ui_toast_show("Please select a CAL file first", ui_color_danger, 1800);
        return;
    }
    if(ctrl_is_sending() || is_selected_cal_opened()) {
        ui_toast_show(ctrl_is_sending()
                          ? "Stop the job before deleting"
                          : "File is open — Delete unavailable",
                      ui_color_danger, 2500);
        return;
    }

    const char *name = g_cal_file_list.files[g_cal_selected_idx].name;
    snprintf(s_cal_delete_name, sizeof(s_cal_delete_name), "%s", name);

    char msg[320];
    snprintf(msg, sizeof(msg),
             "Delete \"%s\"?\nThis action cannot be undone.", name);
    ui_confirm_show("Delete file", msg, "Delete", "Cancel",
                    cal_delete_confirmed, nullptr, nullptr);
}

static void cal_ui_refresh_impl(void)
{
    update_cal_open_button();
    update_cal_action_buttons();
}

void page_cal_files_refresh_ui(void)
{
    cal_ui_refresh_impl();
}

static void on_cal_back_click(lv_event_t *)
{
    if(s_back_cb) {
        s_back_cb();
    }
}

void page_cal_files_build(lv_obj_t *parent)
{
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, UI_PAD, 0);
    lv_obj_set_style_pad_row(parent, UI_PAD, 0);

    lv_obj_t *title_row = lv_obj_create(parent);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, UI_PAD_SM, 0);

    lv_obj_t *btn_back = lv_button_create(title_row);
    lv_obj_add_style(btn_back, ui_style_btn_primary(), 0);
    lv_obj_set_style_bg_color(btn_back, ui_color_panel_alt, 0);
    lv_obj_add_event_cb(btn_back, on_cal_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Dir");
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, LV_SYMBOL_FILE "  CAL Files");
    lv_obj_add_style(title, ui_style_title(), 0);
    lv_obj_set_flex_grow(title, 1);

    lv_obj_t *btn_refresh = lv_button_create(title_row);
    lv_obj_add_style(btn_refresh, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn_refresh, on_cal_refresh_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(btn_refresh);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_center(refresh_lbl);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " Swipe left for G-code Files");
    lv_obj_set_style_text_color(hint, ui_color_text_dim, 0);

    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, UI_PAD, 0);

    lv_obj_t *left_panel = lv_obj_create(body);
    lv_obj_add_style(left_panel, ui_style_panel(), 0);
    lv_obj_set_width(left_panel, LV_PCT(38));
    lv_obj_set_height(left_panel, LV_PCT(100));
    lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left_panel, UI_PAD_SM, 0);
    lv_obj_set_style_pad_row(left_panel, UI_PAD_SM, 0);

    g_cal_list = lv_obj_create(left_panel);
    lv_obj_remove_style_all(g_cal_list);
    lv_obj_set_width(g_cal_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_cal_list, 1);
    lv_obj_set_flex_flow(g_cal_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_cal_list, UI_PAD_SM, 0);
    lv_obj_add_flag(g_cal_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_cal_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_cal_list, LV_SCROLLBAR_MODE_AUTO);

    g_cal_detail = lv_obj_create(body);
    lv_obj_add_style(g_cal_detail, ui_style_panel(), 0);
    lv_obj_set_flex_grow(g_cal_detail, 1);
    lv_obj_set_height(g_cal_detail, LV_PCT(100));
    lv_obj_set_flex_flow(g_cal_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_cal_detail, UI_PAD, 0);
    lv_obj_set_style_pad_row(g_cal_detail, UI_PAD, 0);

    g_lbl_cal_name = lv_label_create(g_cal_detail);
    lv_label_set_text(g_lbl_cal_name, "No file selected");
    lv_obj_add_style(g_lbl_cal_name, ui_style_title(), 0);

    g_lbl_cal_size = lv_label_create(g_cal_detail);
    lv_label_set_text(g_lbl_cal_size, "");
    lv_obj_set_style_text_color(g_lbl_cal_size, ui_color_text_dim, 0);

    g_lbl_cal_grid = lv_label_create(g_cal_detail);
    lv_label_set_text(g_lbl_cal_grid, "");
    lv_obj_set_style_text_color(g_lbl_cal_grid, ui_color_text_dim, 0);

    g_lbl_cal_gains = lv_label_create(g_cal_detail);
    lv_label_set_text(g_lbl_cal_gains, "");
    lv_obj_set_style_text_color(g_lbl_cal_gains, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_cal_gains, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_cal_gains, LV_LABEL_LONG_MODE_WRAP);

    g_lbl_cal_status = lv_label_create(g_cal_detail);
    lv_label_set_text(g_lbl_cal_status, "Select a .csv file and tap Open");
    lv_obj_set_style_text_color(g_lbl_cal_status, ui_color_text_dim, 0);
    lv_label_set_long_mode(g_lbl_cal_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(g_lbl_cal_status, LV_PCT(100));

    lv_obj_t *btn_row = lv_obj_create(g_cal_detail);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, UI_PAD_SM, 0);

    g_btn_cal_open = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_cal_open, UI_BTN_JOB_W, UI_INPUT_H);
    lv_obj_add_style(g_btn_cal_open, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_cal_open, on_cal_open_click, LV_EVENT_CLICKED, NULL);
    g_cal_open_lbl = lv_label_create(g_btn_cal_open);
    lv_label_set_text(g_cal_open_lbl, LV_SYMBOL_PLAY "  Open");
    lv_obj_center(g_cal_open_lbl);

    g_btn_cal_load = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_cal_load, UI_BTN_JOB_W, UI_INPUT_H);
    lv_obj_add_style(g_btn_cal_load, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_cal_load, on_cal_load_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *load_lbl = lv_label_create(g_btn_cal_load);
    lv_label_set_text(load_lbl, LV_SYMBOL_DOWNLOAD "  Load");
    lv_obj_center(load_lbl);
    lv_obj_add_flag(g_btn_cal_load, LV_OBJ_FLAG_HIDDEN);

    g_btn_cal_delete = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_cal_delete, UI_BTN_JOB_W, UI_INPUT_H);
    lv_obj_add_style(g_btn_cal_delete, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_cal_delete, on_cal_delete_click, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(g_btn_cal_delete, ui_color_danger, LV_STATE_PRESSED);
    lv_obj_t *delete_lbl = lv_label_create(g_btn_cal_delete);
    lv_label_set_text(delete_lbl, LV_SYMBOL_TRASH "  Delete");
    lv_obj_center(delete_lbl);
    lv_obj_add_flag(g_btn_cal_delete, LV_OBJ_FLAG_HIDDEN);
}

void page_cal_files_refresh_list(void)
{
    rebuild_cal_file_list_ui();
}
