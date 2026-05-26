#include "page_home.hpp"
#include "page_common.hpp"
#include "job_status.hpp"
#include "ctrl_backend.hpp"
#include "ui_theme.hpp"

#include <cstdio>
#include <cstring>

/* ---------- 控件 ---------- */
static lv_obj_t *g_bar_parse = nullptr;
static lv_obj_t *g_bar_ring  = nullptr;
static lv_obj_t *g_bar_cmd   = nullptr;
static lv_obj_t *g_bar_data  = nullptr;

static lv_obj_t *g_lbl_file      = nullptr;
static lv_obj_t *g_lbl_pos       = nullptr;
static lv_obj_t *g_lbl_status    = nullptr;
static lv_obj_t *g_lbl_stat      = nullptr;
static lv_obj_t *g_lbl_parse_pct = nullptr;
static lv_obj_t *g_lbl_ring_pct  = nullptr;
static lv_obj_t *g_lbl_cmd_pct   = nullptr;
static lv_obj_t *g_lbl_data_pct  = nullptr;

static lv_obj_t *g_ind_tcp_tx       = nullptr;
static lv_obj_t *g_ind_layer_change = nullptr;
static lv_obj_t *g_ind_cal_apply    = nullptr;

static uint32_t s_last_ui_gen = 0;

static void set_indicator_dot(lv_obj_t *dot, bool on)
{
    if(!dot) {
        return;
    }
    lv_obj_set_style_bg_color(dot, on ? ui_color_accent : ui_color_text_dim, 0);
}

static lv_obj_t *make_home_flex_row(lv_obj_t *parent, lv_coord_t pad_bottom)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);
    if(pad_bottom > 0) {
        lv_obj_set_style_pad_bottom(row, pad_bottom, 0);
    }
    return row;
}

static void add_indicator_item(lv_obj_t *row, const char *label, lv_obj_t **out_dot)
{
    lv_obj_t *item = lv_obj_create(row);
    lv_obj_remove_style_all(item);
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 8, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);

    *out_dot = lv_obj_create(item);
    lv_obj_remove_style_all(*out_dot);
    lv_obj_set_size(*out_dot, UI_INDICATOR_SZ, UI_INDICATOR_SZ);
    lv_obj_set_style_radius(*out_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(*out_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(*out_dot, ui_color_text_dim, 0);
    lv_obj_set_style_border_width(*out_dot, 0, 0);
}

static void make_progress_indicator_row(lv_obj_t *parent)
{
    lv_obj_t *row = make_home_flex_row(parent, UI_PAD_SM);
    lv_obj_set_style_pad_column(row, UI_PAD, 0);

    static const struct {
        const char *label;
        lv_obj_t **dot;
    } items[] = {
        { "TCP Tx", &g_ind_tcp_tx },
        { "Layer",  &g_ind_layer_change },
        { "CAL",    &g_ind_cal_apply },
    };
    for(const auto &it : items) {
        add_indicator_item(row, it.label, it.dot);
    }
}

static void make_bar_row(lv_obj_t *parent, const char *title,
                         lv_color_t bar_color, lv_obj_t **out_bar, lv_obj_t **out_lbl)
{
    lv_obj_t *row = make_home_flex_row(parent, 0);

    lv_obj_t *lbl_title = lv_label_create(row);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_color(lbl_title, ui_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl_title, ui_font_body(), 0);
    lv_obj_set_width(lbl_title, UI_BAR_TITLE_W);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_CLIP);

    *out_lbl = lv_label_create(row);
    lv_label_set_text(*out_lbl, "0%");
    lv_obj_set_style_text_color(*out_lbl, ui_color_text, 0);
    lv_obj_set_width(*out_lbl, UI_BAR_VALUE_W);
    lv_label_set_long_mode(*out_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(*out_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    *out_bar = lv_bar_create(row);
    lv_obj_set_flex_grow(*out_bar, 1);
    lv_obj_set_height(*out_bar, UI_BAR_H);
    lv_obj_set_style_min_width(*out_bar, 48, 0);
    lv_obj_set_style_bg_color(*out_bar, ui_color_panel_alt, 0);
    lv_obj_set_style_bg_color(*out_bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(*out_bar, UI_RADIUS, 0);
    lv_obj_set_style_radius(*out_bar, UI_RADIUS, LV_PART_INDICATOR);
    lv_bar_set_range(*out_bar, 0, 100);
    lv_bar_set_value(*out_bar, 0, LV_ANIM_OFF);
}

static void update_bar_percent(lv_obj_t *bar, lv_obj_t *lbl, int pct)
{
    if(bar) {
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    }
    if(lbl) {
        lv_label_set_text_fmt(lbl, "%d%%", pct);
    }
}

static void update_bar_mcu_rate(lv_obj_t *bar, lv_obj_t *lbl, uint16_t rate)
{
    if(bar) {
        lv_bar_set_value(bar, percent_rounded(rate, 65535), LV_ANIM_OFF);
    }
    if(lbl) {
        char buf[16];
        format_mcu_queue_rate_label(buf, sizeof(buf), rate);
        lv_label_set_text(lbl, buf);
    }
}

static const char *home_status_text(bool waiting_layer, bool mcu_cmd_tx_hold,
                                    bool is_sending, bool is_parsing,
                                    bool ring_full, int parsed_lines, int total_lines)
{
    if(waiting_layer) {
        return "Layer Change — waiting";
    }
    if(mcu_cmd_tx_hold && is_sending) {
        return "MCU Cmd High (TX hold)";
    }
    if(ring_full && (is_parsing || is_sending)) {
        return "Ring Buffer Full";
    }
    if(is_sending) {
        return "Running";
    }
    if(is_parsing) {
        return "Parsing";
    }
    if(parsed_lines > 0 && total_lines > 0 && parsed_lines >= total_lines) {
        return "Parse Complete — ready to Start";
    }
    return "Idle";
}

static void page_home_refresh_impl(void)
{
    job_status_update_ring();

    char file_name[256];
    int total_lines, parsed_lines;
    size_t ring_used, ring_capacity;
    uint16_t cmd_rate, data_rate, cur_x, cur_y, cur_z, laser;
    bool is_sending, is_parsing, tcp_connected, ring_full, mcu_cmd_tx_hold;
    bool cal_enabled, enable_layer_change;

    {
        JOB_LOCK();
        strncpy(file_name, g_job_status.current_file, sizeof(file_name) - 1);
        file_name[sizeof(file_name) - 1] = '\0';
        total_lines    = g_job_status.total_lines;
        parsed_lines   = g_job_status.parsed_lines;
        ring_used      = g_job_status.ring_used;
        ring_capacity  = g_job_status.ring_capacity;
        ring_full      = g_job_status.ring_full;
        cmd_rate       = g_job_status.cmd_cache_rate;
        data_rate      = g_job_status.data_cache_rate;
        cur_x          = g_job_status.current_x;
        cur_y          = g_job_status.current_y;
        cur_z          = g_job_status.current_z;
        laser          = g_job_status.laser_state;
        is_parsing     = g_job_status.is_parsing;
        tcp_connected  = g_job_status.tcp_connected;
        mcu_cmd_tx_hold = g_job_status.mcu_cmd_tx_hold;
        cal_enabled         = g_job_status.enable_cal_apply;
        enable_layer_change = g_job_status.enable_layer_change;
        if(g_job_status.flash_saved) {
            g_job_status.flash_saved = 0;
        }
    }

    is_sending = ctrl_is_sending() != 0;
    const int waiting_layer = ctrl_is_waiting_layer();

    lv_label_set_text_fmt(g_lbl_file, LV_SYMBOL_FILE " Job: %s",
        file_name[0] ? file_name : "--");

    const int parse_pct = percent_rounded(parsed_lines, total_lines);
    const int ring_pct  = percent_rounded((int)ring_used, (int)ring_capacity);
    update_bar_percent(g_bar_parse, g_lbl_parse_pct, parse_pct);
    update_bar_percent(g_bar_ring, g_lbl_ring_pct, ring_pct);
    update_bar_mcu_rate(g_bar_cmd, g_lbl_cmd_pct, cmd_rate);
    update_bar_mcu_rate(g_bar_data, g_lbl_data_pct, data_rate);

    set_indicator_dot(g_ind_tcp_tx,
        tcp_connected && is_sending && (ctrl_is_paused() == 0) && (waiting_layer == 0));
    set_indicator_dot(g_ind_layer_change, enable_layer_change && (waiting_layer != 0));
    set_indicator_dot(g_ind_cal_apply, cal_enabled);

    const char *laser_txt = "--";
    if(tcp_connected) {
        laser_txt = (laser != 0) ? "ON" : "OFF";
    }
    lv_label_set_text_fmt(g_lbl_pos,
        LV_SYMBOL_GPS " X:%u  Y:%u  Z:%u   " LV_SYMBOL_CHARGE " Laser:%s",
        cur_x, cur_y, cur_z, laser_txt);

    lv_label_set_text_fmt(g_lbl_status, LV_SYMBOL_HOME " Status: %s",
        home_status_text(waiting_layer != 0, mcu_cmd_tx_hold, is_sending, is_parsing,
                         ring_full, parsed_lines, total_lines));

    char cmd_val[16];
    char data_val[16];
    format_mcu_queue_rate_label(cmd_val, sizeof(cmd_val), cmd_rate);
    format_mcu_queue_rate_label(data_val, sizeof(data_val), data_rate);

    lv_label_set_text_fmt(g_lbl_stat,
        "Ring %u/%u (%d%%)%s  Cmd %s  Data %s",
        (unsigned)ring_used, (unsigned)ring_capacity, ring_pct,
        ring_full ? " full" : "",
        tcp_connected ? cmd_val : "--",
        tcp_connected ? data_val : "--");
}

void page_home_refresh(bool force)
{
    const uint32_t gen = job_status_ui_generation();
    if(!force && gen == s_last_ui_gen) {
        return;
    }
    s_last_ui_gen = gen;
    page_home_refresh_impl();
}

lv_obj_t *page_home_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, LV_SYMBOL_HOME "  Home ");
    lv_obj_add_style(title, ui_style_title(), 0);

    g_lbl_file = lv_label_create(page);
    lv_label_set_text(g_lbl_file, LV_SYMBOL_FILE " Job: --");
    lv_obj_set_style_text_color(g_lbl_file, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_file, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_file, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_pad_bottom(g_lbl_file, UI_PAD_SM, 0);

    lv_obj_t *prog_card = ui_make_card(page, "Progress");
    lv_obj_set_height(prog_card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(prog_card, LV_OBJ_FLAG_SCROLLABLE);

    make_progress_indicator_row(prog_card);
    make_bar_row(prog_card, "Parse", ui_color_accent,     &g_bar_parse, &g_lbl_parse_pct);
    make_bar_row(prog_card, "Ring",  ui_color_danger,     &g_bar_ring,  &g_lbl_ring_pct);

    lv_obj_t *mcu_card = ui_make_card(page, "MCU Status");
    lv_obj_set_height(mcu_card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(mcu_card, LV_OBJ_FLAG_SCROLLABLE);

    make_bar_row(mcu_card, "Cmd",  ui_color_accent,     &g_bar_cmd,  &g_lbl_cmd_pct);
    make_bar_row(mcu_card, "Data", ui_color_accent_dim, &g_bar_data, &g_lbl_data_pct);

    g_lbl_pos = lv_label_create(mcu_card);
    lv_label_set_text(g_lbl_pos,
        LV_SYMBOL_GPS " X:-- Y:-- Z:--   " LV_SYMBOL_CHARGE " Laser:--");
    lv_obj_set_style_text_color(g_lbl_pos, ui_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_pos, ui_font_body(), 0);
    lv_obj_set_width(g_lbl_pos, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_pos, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_pad_top(g_lbl_pos, UI_PAD_SM, 0);

    g_lbl_status = lv_label_create(page);
    lv_label_set_text(g_lbl_status, LV_SYMBOL_HOME " Status: Idle");
    lv_obj_set_style_text_color(g_lbl_status, ui_color_text_dim, 0);
    lv_obj_set_style_pad_top(g_lbl_status, UI_PAD_SM, 0);

    g_lbl_stat = lv_label_create(page);
    lv_label_set_text(g_lbl_stat, "Ring 0%  Cmd --  Data --");
    lv_obj_set_style_text_color(g_lbl_stat, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_stat, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_stat, LV_LABEL_LONG_MODE_CLIP);

    return page;
}
