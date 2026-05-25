#include "page_home.hpp"
#include "page_common.hpp"
#include "job_status.hpp"
#include "ctrl_backend.hpp"
#include "ui_theme.hpp"

#include <cstdio>
#include <cstring>

/* ---------- 进度条控件 ---------- */
static lv_obj_t *g_bar_parse = nullptr;
static lv_obj_t *g_bar_ring  = nullptr;
static lv_obj_t *g_bar_cmd   = nullptr;
static lv_obj_t *g_bar_data  = nullptr;

static lv_obj_t *g_lbl_file   = nullptr;
static lv_obj_t *g_lbl_pos    = nullptr;
static lv_obj_t *g_lbl_status = nullptr;
static lv_obj_t *g_lbl_stat      = nullptr;
static lv_obj_t *g_lbl_parse_pct = nullptr;
static lv_obj_t *g_lbl_ring_pct  = nullptr;
static lv_obj_t *g_lbl_cmd_pct   = nullptr;
static lv_obj_t *g_lbl_data_pct  = nullptr;

static lv_obj_t *g_ind_tcp_tx = nullptr;
static lv_obj_t *g_ind_layer_change = nullptr;
static lv_obj_t *g_ind_cal_apply = nullptr;

static void set_indicator_dot(lv_obj_t *dot, bool on)
{
    if(!dot) {
        return;
    }
    lv_obj_set_style_bg_color(dot, on ? ui_color_accent : ui_color_text_dim, 0);
}

/** Progress 卡片：TCP Tx / Layer / CAL 单行指示灯 */
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
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(row, UI_PAD_SM, 0);

    add_indicator_item(row, "TCP Tx", &g_ind_tcp_tx);
    add_indicator_item(row, "Layer", &g_ind_layer_change);
    add_indicator_item(row, "CAL", &g_ind_cal_apply);
}

static void format_ring_value_label(char *buf, size_t buf_size, size_t used, size_t capacity)
{
    const int pct = percent_rounded((int)used, (int)capacity);
    snprintf(buf, buf_size, "%d%%", pct);
}

static int mcu_rate_bar_value(uint16_t rate)
{
    const int tenths = percent_tenths(rate, 65535);
    if(tenths <= 0) {
        return 0;
    }
    const int pct = (tenths + 5) / 10;
    return pct > 100 ? 100 : pct;
}

static void make_bar_row(lv_obj_t *parent, const char *title,
                         lv_color_t bar_color, lv_obj_t **out_bar, lv_obj_t **out_lbl)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);

    lv_obj_t *lbl_title = lv_label_create(row);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_color(lbl_title, ui_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl_title, ui_font_body(), 0);
    lv_obj_set_width(lbl_title, UI_BAR_TITLE_W);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_CLIP);

    /* 百分比在进度条左侧，避免条块向右伸展遮挡文字 */
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

static void status_update_cb(lv_timer_t *)
{
    job_status_update_ring();

    char file_name[256];
    int total_lines, parsed_lines;
    size_t ring_used, ring_capacity;
    uint16_t cmd_rate, data_rate, cur_x, cur_y, cur_z, laser;
    bool is_sending, is_parsing, tcp_connected, ring_full, mcu_cmd_tx_hold;
    bool cal_enabled;
    bool enable_layer_change;
    const int waiting_layer = ctrl_is_waiting_layer();

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
        cal_enabled = g_job_status.enable_cal_apply;
        enable_layer_change = g_job_status.enable_layer_change;
        if(g_job_status.flash_saved) {
            g_job_status.flash_saved = 0;
        }
    }
    is_sending = ctrl_is_sending() != 0;

    if(file_name[0]) {
        lv_label_set_text_fmt(g_lbl_file, LV_SYMBOL_FILE " Job: %s", file_name);
    } else {
        lv_label_set_text(g_lbl_file, LV_SYMBOL_FILE " Job: --");
    }

    const int parse_pct = percent_rounded(parsed_lines, total_lines);
    lv_bar_set_value(g_bar_parse, parse_pct, LV_ANIM_OFF);
    if(g_lbl_parse_pct) {
        lv_label_set_text_fmt(g_lbl_parse_pct, "%d%%", parse_pct);
    }

    const int ring_pct = percent_rounded((int)ring_used, (int)ring_capacity);
    lv_bar_set_value(g_bar_ring, ring_pct, LV_ANIM_OFF);
    if(g_lbl_ring_pct) {
        char ring_val[24];
        format_ring_value_label(ring_val, sizeof(ring_val), ring_used, ring_capacity);
        lv_label_set_text(g_lbl_ring_pct, ring_val);
    }

    char cmd_val[16];
    char data_val[16];
    format_mcu_queue_rate_label(cmd_val, sizeof(cmd_val), cmd_rate);
    format_mcu_queue_rate_label(data_val, sizeof(data_val), data_rate);
    lv_bar_set_value(g_bar_cmd,  mcu_rate_bar_value(cmd_rate),  LV_ANIM_OFF);
    lv_bar_set_value(g_bar_data, mcu_rate_bar_value(data_rate), LV_ANIM_OFF);
    if(g_lbl_cmd_pct) {
        lv_label_set_text(g_lbl_cmd_pct, cmd_val);
    }
    if(g_lbl_data_pct) {
        lv_label_set_text(g_lbl_data_pct, data_val);
    }

    /* TCP Tx：运动帧下发阶段亮（发送中、未暂停、非换层等待；背压短暂停发仍视为下发中） */
    const bool tcp_tx_on = tcp_connected && is_sending &&
                           (ctrl_is_paused() == 0) && (waiting_layer == 0);
    const bool layer_on = enable_layer_change && (waiting_layer != 0);
    const bool cal_on = cal_enabled;

    set_indicator_dot(g_ind_tcp_tx, tcp_tx_on);
    set_indicator_dot(g_ind_layer_change, layer_on);
    set_indicator_dot(g_ind_cal_apply, cal_on);

    const char *laser_txt = "--";
    if(tcp_connected) {
        laser_txt = (laser != 0) ? "ON" : "OFF";
    }

    lv_label_set_text_fmt(g_lbl_pos,
        LV_SYMBOL_GPS " X:%u  Y:%u  Z:%u   " LV_SYMBOL_CHARGE " Laser:%s",
        cur_x, cur_y, cur_z, laser_txt);

    const char *st = "Idle";
    if(waiting_layer != 0) {
        st = "Layer Change — waiting";
    } else if(mcu_cmd_tx_hold && is_sending) {
        st = "MCU Cmd High (TX hold)";
    } else if(ring_full && (is_parsing || is_sending)) {
        st = "Ring Buffer Full";
    } else if(is_sending) {
        st = "Running";
    } else if(is_parsing) {
        st = "Parsing";
    } else if(parsed_lines > 0 && total_lines > 0 && parsed_lines >= total_lines) {
        st = "Parse Complete — ready to Start";
    }

    lv_label_set_text_fmt(g_lbl_status, LV_SYMBOL_HOME " Status: %s", st);

    const char *cmd_txt = "--";
    const char *data_txt = "--";
    if(tcp_connected) {
        cmd_txt = cmd_val;
        data_txt = data_val;
    }
    lv_label_set_text_fmt(g_lbl_stat,
        "Ring %u/%u (%d%%)%s  Cmd %s  Data %s",
        (unsigned)ring_used, (unsigned)ring_capacity, ring_pct,
        ring_full ? " full" : "",
        cmd_txt, data_txt);
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

    /* 标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, LV_SYMBOL_HOME "  Home / Monitor");
    lv_obj_add_style(title, ui_style_title(), 0);

    /* 当前文件 */
    g_lbl_file = lv_label_create(page);
    lv_label_set_text(g_lbl_file, LV_SYMBOL_FILE " Job: --");
    lv_obj_set_style_text_color(g_lbl_file, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_file, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_file, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_pad_bottom(g_lbl_file, UI_PAD_SM, 0);

    /* 进度面板 */
    lv_obj_t *prog_card = ui_make_card(page, "Progress");
    lv_obj_set_height(prog_card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(prog_card, LV_OBJ_FLAG_SCROLLABLE);

    make_progress_indicator_row(prog_card);
    make_bar_row(prog_card, "Parse",   ui_color_accent,     &g_bar_parse, &g_lbl_parse_pct);
    make_bar_row(prog_card, "Ring",    ui_color_danger,     &g_bar_ring,  &g_lbl_ring_pct);

    /* 下位机状态面板（固定内容高度，避免 flex 拉伸后出现内部滚动条） */
    lv_obj_t *mcu_card = ui_make_card(page, "MCU Status");
    lv_obj_set_height(mcu_card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(mcu_card, LV_OBJ_FLAG_SCROLLABLE);

    make_bar_row(mcu_card, "Cmd",  ui_color_accent,     &g_bar_cmd,  &g_lbl_cmd_pct);
    make_bar_row(mcu_card, "Data", ui_color_accent_dim, &g_bar_data, &g_lbl_data_pct);

    /* 位置与激光状态 */
    g_lbl_pos = lv_label_create(mcu_card);
    lv_label_set_text(g_lbl_pos,
        LV_SYMBOL_GPS " X:-- Y:-- Z:--   " LV_SYMBOL_CHARGE " Laser:--");
    lv_obj_set_style_text_color(g_lbl_pos, ui_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_pos, ui_font_body(), 0);
    lv_obj_set_width(g_lbl_pos, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_pos, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_pad_top(g_lbl_pos, UI_PAD_SM, 0);

    /* 状态栏 */
    g_lbl_status = lv_label_create(page);
    lv_label_set_text(g_lbl_status, LV_SYMBOL_HOME " Status: Idle");
    lv_obj_set_style_text_color(g_lbl_status, ui_color_text_dim, 0);
    lv_obj_set_style_pad_top(g_lbl_status, UI_PAD_SM, 0);

    g_lbl_stat = lv_label_create(page);
    lv_label_set_text(g_lbl_stat, "Ring 0%  Cmd --  Data --");
    lv_obj_set_style_text_color(g_lbl_stat, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_stat, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_stat, LV_LABEL_LONG_MODE_CLIP);

    /* 定时刷新 */
    lv_timer_create(status_update_cb, 200, nullptr);

    return page;
}
