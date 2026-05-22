#include "page_files.hpp"
#include "files_open.hpp"
#include "gcode_worker.hpp"
#include "ctrl_backend.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "job_status.hpp"

#include <cstring>
#include <cstdio>

/* ---------- UI widgets ---------- */
static lv_obj_t *g_list = nullptr;
static lv_obj_t *g_lbl_name = nullptr;
static lv_obj_t *g_lbl_size = nullptr;
static lv_obj_t *g_lbl_lines = nullptr;
static lv_obj_t *g_lbl_status = nullptr;
static lv_obj_t *g_lbl_ring_full = nullptr;
static lv_obj_t *g_bar_parse = nullptr;
static lv_obj_t *g_btn_open = nullptr;
static lv_obj_t *g_open_lbl = nullptr;
static lv_obj_t *g_detail = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static bool g_page_visible = false;

static gcode_file_list_t g_file_list;
static int g_selected_idx = -1;
/** File bound to ring buffer / parse worker (-1 = none) */
static int g_active_idx = -1;
/** File currently parsing (-1 = parse idle) */
static int g_parsing_idx = -1;

typedef enum {
    FILE_SLOT_IDLE = 0,
    FILE_SLOT_PARSING,
    FILE_SLOT_READY,
    FILE_SLOT_FAILED,
} file_slot_state_t;

typedef struct {
    file_slot_state_t state;
    int total_lines;
    int parsed_lines;
    int packets;
} file_slot_t;

static file_slot_t g_slots[GCODE_MAX_FILES];

static void reset_file_slot(int idx)
{
    if(idx < 0 || idx >= GCODE_MAX_FILES) {
        return;
    }
    g_slots[idx].state        = FILE_SLOT_IDLE;
    g_slots[idx].total_lines  = 0;
    g_slots[idx].parsed_lines = 0;
    g_slots[idx].packets      = 0;
}

static void reset_all_slots(void)
{
    for(int i = 0; i < GCODE_MAX_FILES; i++) {
        reset_file_slot(i);
    }
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
    if(!g_btn_open) {
        return;
    }
    if(enabled) {
        lv_obj_remove_state(g_btn_open, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(g_btn_open, ui_color_accent_dim, 0);
    } else {
        lv_obj_add_state(g_btn_open, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(g_btn_open, ui_color_panel_alt, 0);
    }
}

static void set_open_btn_caption(const char *caption)
{
    if(g_open_lbl && caption) {
        lv_label_set_text(g_open_lbl, caption);
    }
}

static void update_open_button(void)
{
    if(g_selected_idx < 0 || g_selected_idx >= g_file_list.count) {
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
    if(g_parsing_idx == g_selected_idx) {
        set_open_btn_caption(LV_SYMBOL_LOOP "  Opening");
    } else {
        set_open_btn_caption(LV_SYMBOL_PLAY "  Open");
    }
}

/** Refresh right panel for g_selected_idx only (no disk I/O) */
static void refresh_detail_panel(void)
{
    const int idx = g_selected_idx;
    if(idx < 0 || idx >= g_file_list.count) {
        lv_label_set_text(g_lbl_name, "No file selected");
        lv_label_set_text(g_lbl_size, "Size: --");
        lv_label_set_text(g_lbl_lines, "G-code lines: --");
        lv_bar_set_value(g_bar_parse, 0, LV_ANIM_OFF);
        ui_status_set(g_lbl_status, "Select a file and tap Open", ui_color_text_dim);
        if(g_lbl_ring_full) {
            lv_obj_add_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);
        }
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
        update_open_button();
        return;
    }

    const gcode_file_info_t *info = &g_file_list.files[idx];
    const file_slot_t *slot = &g_slots[idx];
    char line_buf[64];
    char size_raw[48];
    char size_line[64];
    char name_line[280];
    char status_buf[192];

    snprintf(name_line, sizeof(name_line), "File: %s", info->name);
    lv_label_set_text(g_lbl_name, name_line);

    format_size(info->size, size_raw, sizeof(size_raw));
    snprintf(size_line, sizeof(size_line), "Size: %s", size_raw);
    lv_label_set_text(g_lbl_size, size_line);

    if(slot->total_lines > 0) {
        snprintf(line_buf, sizeof(line_buf), "G-code lines: %d", slot->total_lines);
    } else {
        snprintf(line_buf, sizeof(line_buf), "G-code lines: --");
    }
    lv_label_set_text(g_lbl_lines, line_buf);

    int pct = 0;
    lv_color_t status_color = ui_color_text_dim;
    bool ring_full = false;
    {
        JOB_LOCK();
        ring_full = g_job_status.ring_full;
    }

    const bool is_parsing_this =
        (g_parsing_idx == idx && gcode_is_parsing()) ||
        (slot->state == FILE_SLOT_PARSING && g_active_idx == idx);

    if(g_lbl_ring_full) {
        if(is_parsing_this && ring_full) {
            lv_obj_remove_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(is_parsing_this && slot->total_lines > 0) {
        pct = (slot->parsed_lines * 100) / slot->total_lines;
        if(pct > 100) {
            pct = 100;
        }
        if(ring_full) {
            snprintf(status_buf, sizeof(status_buf),
                     LV_SYMBOL_PAUSE " Parsing: %d%% (%d/%d lines, %d pkts) — "
                     "ring queue full, waiting for space",
                     pct, slot->parsed_lines, slot->total_lines, slot->packets);
            status_color = ui_color_text_dim;
        } else {
            snprintf(status_buf, sizeof(status_buf),
                     LV_SYMBOL_LOOP " Parsing: %d%% (%d/%d lines, %d pkts)",
                     pct, slot->parsed_lines, slot->total_lines, slot->packets);
            status_color = ui_color_text;
        }
        if(g_bar_parse) {
            lv_obj_set_style_bg_color(g_bar_parse, ui_color_accent, LV_PART_INDICATOR);
        }
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 1, 0);
            lv_obj_set_style_border_color(g_detail, ui_color_border, 0);
        }
    } else if(slot->state == FILE_SLOT_READY) {
        if(g_bar_parse) {
            lv_obj_set_style_bg_color(g_bar_parse, ui_color_accent, LV_PART_INDICATOR);
        }
        pct = 100;
        snprintf(status_buf, sizeof(status_buf),
                 LV_SYMBOL_OK " Ready: %d packets, %d lines",
                 slot->packets, slot->total_lines);
        status_color = ui_color_accent;
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 2, 0);
            lv_obj_set_style_border_color(g_detail, ui_color_accent, 0);
        }
    } else if(slot->state == FILE_SLOT_FAILED) {
        snprintf(status_buf, sizeof(status_buf),
                 LV_SYMBOL_WARNING " Open failed — tap Open to retry");
        status_color = ui_color_danger;
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
    } else if(g_parsing_idx >= 0 && g_parsing_idx < g_file_list.count &&
              g_parsing_idx != idx) {
        snprintf(status_buf, sizeof(status_buf),
                 "Background: parsing %s — tap Open here to switch",
                 g_file_list.files[g_parsing_idx].name);
        status_color = ui_color_text_dim;
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
    } else {
        snprintf(status_buf, sizeof(status_buf), "Selected — tap Open to load");
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
    }

    lv_bar_set_value(g_bar_parse, pct, LV_ANIM_OFF);
    ui_status_set(g_lbl_status, status_buf, status_color);
    update_open_button();
}

static void update_list_selection(void)
{
    const uint32_t cnt = lv_obj_get_child_count(g_list);
    for(uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(g_list, i);
        if(!lv_obj_check_type(btn, &lv_button_class)) {
            continue;
        }
        set_btn_selected_style(btn, (int)i == g_selected_idx);
    }
}

static void on_file_btn_click(lv_event_t *e)
{
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= g_file_list.count) {
        return;
    }
    g_selected_idx = idx;
    update_list_selection();
    refresh_detail_panel();
}

static void stop_active_job(void)
{
    if(ctrl_is_sending()) {
        ctrl_stop_sending();
    }
    gcode_close_file();
    g_ring_buffer.clear();
    g_parsing_idx = -1;
}

static void on_refresh_click(lv_event_t *)
{
    stop_active_job();
    g_active_idx = -1;
    reset_all_slots();

    lv_obj_clean(g_list);
    galva_scan_files(&g_file_list);

    if(g_file_list.count == 0) {
        lv_obj_t *lbl = lv_label_create(g_list);
        lv_label_set_text(lbl, "No G-code files in /home/Gcode");
        lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
        g_selected_idx = -1;
        refresh_detail_panel();
        return;
    }

    for(int i = 0; i < g_file_list.count; i++) {
        lv_obj_t *btn = lv_button_create(g_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 38);
        lv_obj_add_event_cb(btn, on_file_btn_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_set_style_radius(btn, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(btn, UI_PAD_SM, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 8, 0);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, LV_SYMBOL_FILE);
        lv_obj_set_style_text_font(icon, ui_font_icon(), 0);
        lv_obj_set_style_text_color(icon, ui_color_accent, 0);
        lv_obj_set_width(icon, 28);

        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text(name, g_file_list.files[i].name);
        lv_obj_set_style_text_color(name, ui_color_text, 0);
        lv_obj_set_flex_grow(name, 1);
    }

    g_selected_idx = 0;
    update_list_selection();
    refresh_detail_panel();
}

static void sync_parsing_slot_from_worker(void)
{
    if(g_parsing_idx < 0 || g_parsing_idx >= g_file_list.count) {
        return;
    }

    file_slot_t *slot = &g_slots[g_parsing_idx];
    slot->state        = FILE_SLOT_PARSING;
    slot->total_lines  = gcode_get_total_lines();
    slot->parsed_lines = gcode_get_parsed_lines();

    {
        JOB_LOCK();
        slot->packets = g_job_status.total_packets;
    }
}

static void files_ui_timer_cb(lv_timer_t *)
{
    job_status_update_ring();

    if(g_parsing_idx >= 0 && g_parsing_idx < g_file_list.count) {
        sync_parsing_slot_from_worker();

        if(gcode_is_parsing()) {
            if(g_page_visible && g_selected_idx == g_parsing_idx) {
                refresh_detail_panel();
            }
            return;
        }

        /* Parse finished */
        file_slot_t *slot = &g_slots[g_parsing_idx];
        const int finished_idx = g_parsing_idx;

        if(slot->total_lines <= 0) {
            slot->state = FILE_SLOT_FAILED;
            g_active_idx = -1;
        } else {
            slot->state = FILE_SLOT_READY;
            g_active_idx = finished_idx;
        }
        g_parsing_idx = -1;

        {
            JOB_LOCK();
            g_job_status.is_parsing = false;
        }

        if(g_selected_idx == finished_idx) {
            if(slot->state == FILE_SLOT_READY) {
                ui_toast_show("G-code loaded — go to Control to Start", ui_color_accent, 2200);
            } else {
                ui_toast_show("Failed to open G-code file", ui_color_danger, 2500);
            }
            refresh_detail_panel();
        }
        return;
    }

    if(g_page_visible && g_selected_idx >= 0) {
        refresh_detail_panel();
    } else if(g_selected_idx >= 0) {
        update_open_button();
    }
}

void page_files_set_visible(int visible)
{
    g_page_visible = (visible != 0);
    if(g_page_visible) {
        sync_parsing_slot_from_worker();
        refresh_detail_panel();
    }
}

static void begin_open_selected_file(void)
{
    if(g_selected_idx < 0 || g_selected_idx >= g_file_list.count) {
        ui_toast_show("Please select a file first", ui_color_danger, 1800);
        return;
    }

    if(ctrl_is_sending()) {
        ui_toast_show("Stop the job before switching G-code", ui_color_danger, 2500);
        refresh_detail_panel();
        return;
    }

    const int idx = g_selected_idx;
    char path[512];
    galva_build_full_path(g_file_list.files[idx].name, path, sizeof(path));

    /* Switching / re-parse: stop background worker without touching selection UI */
    if(g_active_idx >= 0 && g_active_idx < g_file_list.count && g_active_idx != idx) {
        reset_file_slot(g_active_idx);
    }
    if(g_parsing_idx >= 0 && g_parsing_idx < g_file_list.count && g_parsing_idx != idx) {
        reset_file_slot(g_parsing_idx);
    }

    stop_active_job();
    reset_file_slot(idx);

    g_slots[idx].state = FILE_SLOT_PARSING;
    g_parsing_idx = idx;
    g_active_idx = idx;

    if(gcode_open_file(path) != 0) {
        g_slots[idx].state = FILE_SLOT_FAILED;
        g_parsing_idx = -1;
        g_active_idx = -1;
        ui_toast_show("Cannot open file", ui_color_danger, 2500);
        refresh_detail_panel();
        return;
    }

    g_slots[idx].total_lines = gcode_get_total_lines();

    {
        JOB_LOCK();
        g_job_status.total_packets = 0;
        g_job_status.sent_packets = 0;
        g_job_status.parsed_lines = 0;
        g_job_status.total_lines = g_slots[idx].total_lines;
        g_job_status.is_parsing = true;
    }

    gcode_start_parse(&g_ring_buffer);
    refresh_detail_panel();
}

static void on_open_click(lv_event_t *)
{
    begin_open_selected_file();
}

lv_obj_t *page_files_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);

    lv_obj_t *title_row = lv_obj_create(page);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, LV_SYMBOL_FILE "  G-code Files");
    lv_obj_add_style(title, ui_style_title(), 0);

    lv_obj_t *btn_refresh = lv_button_create(title_row);
    lv_obj_add_style(btn_refresh, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn_refresh, on_refresh_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(btn_refresh);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_center(refresh_lbl);

    lv_obj_t *body = lv_obj_create(page);
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

    g_list = lv_obj_create(left_panel);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_width(g_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_list, UI_PAD_SM, 0);

    g_detail = lv_obj_create(body);
    lv_obj_add_style(g_detail, ui_style_panel(), 0);
    lv_obj_set_flex_grow(g_detail, 1);
    lv_obj_set_height(g_detail, LV_PCT(100));
    lv_obj_set_flex_flow(g_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_detail, UI_PAD, 0);
    lv_obj_set_style_pad_row(g_detail, UI_PAD, 0);

    g_lbl_name = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_name, "No file selected");
    lv_obj_add_style(g_lbl_name, ui_style_title(), 0);

    g_lbl_size = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_size, "");
    lv_obj_set_style_text_color(g_lbl_size, ui_color_text_dim, 0);

    g_lbl_lines = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_lines, "");
    lv_obj_set_style_text_color(g_lbl_lines, ui_color_text_dim, 0);

    g_bar_parse = lv_bar_create(g_detail);
    lv_obj_set_width(g_bar_parse, LV_PCT(100));
    lv_obj_set_height(g_bar_parse, 8);
    lv_bar_set_range(g_bar_parse, 0, 100);
    lv_bar_set_value(g_bar_parse, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar_parse, ui_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar_parse, ui_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar_parse, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar_parse, 4, LV_PART_INDICATOR);

    g_lbl_ring_full = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_ring_full,
        LV_SYMBOL_PAUSE " Ring queue full — parsing waits until send frees slots");
    lv_obj_set_style_text_color(g_lbl_ring_full, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_ring_full, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_ring_full, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);

    g_lbl_status = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_status, "Select a file and tap Open to load");
    lv_obj_set_style_text_color(g_lbl_status, ui_color_text_dim, 0);
    lv_label_set_long_mode(g_lbl_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(g_lbl_status, LV_PCT(100));

    lv_obj_t *btn_row = lv_obj_create(g_detail);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, UI_PAD_SM, 0);

    g_btn_open = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_open, 110, UI_INPUT_H);
    lv_obj_add_style(g_btn_open, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_open, on_open_click, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(g_btn_open, ui_color_accent, LV_STATE_PRESSED);
    g_open_lbl = lv_label_create(g_btn_open);
    lv_label_set_text(g_open_lbl, LV_SYMBOL_PLAY "  Open");
    lv_obj_center(g_open_lbl);

    reset_all_slots();
    g_ui_timer = lv_timer_create(files_ui_timer_cb, 120, nullptr);

    on_refresh_click(nullptr);

    return page;
}
