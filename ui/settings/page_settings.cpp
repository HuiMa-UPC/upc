#include "page_settings.hpp"
#include "ctrl_backend.hpp"
#include "job_status.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---------- 控件 ---------- */
static lv_obj_t *g_ta_a_max   = nullptr;
static lv_obj_t *g_sw_fly_en  = nullptr;
static lv_obj_t *g_ta_fly_gain= nullptr;
static lv_obj_t *g_sw_acc_en  = nullptr;
static lv_obj_t *g_ta_acc_del = nullptr;
static lv_obj_t *g_ta_laser_s = nullptr;
static lv_obj_t *g_ta_laser_e = nullptr;

static lv_obj_t *g_lbl_tcp_conn  = nullptr;
static lv_obj_t *g_lbl_serial_conn = nullptr;
static lv_obj_t *g_btn_read = nullptr;
static lv_obj_t *g_btn_write = nullptr;

static bool g_form_dirty = false;
static bool g_last_editable = false;

static void mark_form_dirty(lv_event_t *)
{
    g_form_dirty = true;
}

static void set_obj_enabled(lv_obj_t *obj, bool enabled)
{
    if(!obj) {
        return;
    }
    if(enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        if(lv_obj_has_state(obj, LV_STATE_FOCUSED)) {
            lv_obj_remove_state(obj, LV_STATE_FOCUSED);
        }
    }
}

static void set_params_form_editable(bool editable)
{
    set_obj_enabled(g_ta_a_max, editable);
    set_obj_enabled(g_ta_fly_gain, editable);
    set_obj_enabled(g_ta_acc_del, editable);
    set_obj_enabled(g_ta_laser_s, editable);
    set_obj_enabled(g_ta_laser_e, editable);
    set_obj_enabled(g_sw_fly_en, editable);
    set_obj_enabled(g_sw_acc_en, editable);
    set_obj_enabled(g_btn_read, editable);
    set_obj_enabled(g_btn_write, editable);

    if(!editable) {
        ui_keyboard_hide();
    }
}

static void sync_params_to_form(float param_a_max, uint8_t param_fly_en, float param_fly_g,
                                uint8_t param_acc_en, float param_acc_d,
                                int32_t param_ls, int32_t param_le)
{
    char buf[32];
    if(g_ta_a_max) {
        snprintf(buf, sizeof(buf), "%.4f", param_a_max);
        lv_textarea_set_text(g_ta_a_max, buf);
    }
    if(g_sw_fly_en) {
        if(param_fly_en) {
            lv_obj_add_state(g_sw_fly_en, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_sw_fly_en, LV_STATE_CHECKED);
        }
    }
    if(g_ta_fly_gain) {
        snprintf(buf, sizeof(buf), "%.4f", param_fly_g);
        lv_textarea_set_text(g_ta_fly_gain, buf);
    }
    if(g_sw_acc_en) {
        if(param_acc_en) {
            lv_obj_add_state(g_sw_acc_en, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_sw_acc_en, LV_STATE_CHECKED);
        }
    }
    if(g_ta_acc_del) {
        snprintf(buf, sizeof(buf), "%.4f", param_acc_d);
        lv_textarea_set_text(g_ta_acc_del, buf);
    }
    if(g_ta_laser_s) {
        snprintf(buf, sizeof(buf), "%d", (int)param_ls);
        lv_textarea_set_text(g_ta_laser_s, buf);
    }
    if(g_ta_laser_e) {
        snprintf(buf, sizeof(buf), "%d", (int)param_le);
        lv_textarea_set_text(g_ta_laser_e, buf);
    }
}

/* ---------- 辅助 ---------- */
static lv_obj_t *make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, ui_style_panel(), 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, UI_PAD, 0);
    lv_obj_set_style_pad_row(card, UI_PAD_SM, 0);

    if (title) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, title);
        lv_obj_add_style(lbl, ui_style_subtitle(), 0);
    }
    return card;
}

static lv_obj_t *make_param_row(lv_obj_t *parent, const char *name,
                                const char *init_val, lv_obj_t **out_ta)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_width(lbl, 120);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);

    *out_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(*out_ta, true);
    lv_textarea_set_text(*out_ta, init_val);
    lv_obj_set_flex_grow(*out_ta, 1);
    lv_obj_set_style_bg_color(*out_ta, ui_color_panel_alt, 0);
    lv_obj_set_style_border_color(*out_ta, ui_color_border, 0);
    lv_obj_set_style_border_width(*out_ta, 1, 0);
    lv_obj_set_style_radius(*out_ta, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(*out_ta, UI_PAD_SM, 0);

    ui_keyboard_attach(*out_ta, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(*out_ta, mark_form_dirty, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

static lv_obj_t *make_switch_row(lv_obj_t *parent, const char *name, lv_obj_t **out_sw)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_width(lbl, 120);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);

    *out_sw = lv_switch_create(row);
    lv_obj_add_event_cb(*out_sw, mark_form_dirty, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

/* ---------- 回调 ---------- */
static void on_read_params(lv_event_t *)
{
    if(!ctrl_tcp_is_connected()) {
        ui_toast_show("TCP not connected", ui_color_danger, 2000);
        return;
    }
    if(ctrl_is_sending()) {
        ui_toast_show("Stop sending before read", ui_color_danger, 2000);
        return;
    }
    g_form_dirty = false;
    ctrl_request_read_params();
    ui_toast_show("Reading board parameters...", ui_color_text, 1500);
}

static void on_set_params(lv_event_t *)
{
    if(!ctrl_tcp_is_connected()) {
        ui_toast_show("TCP not connected", ui_color_danger, 2000);
        return;
    }
    if(ctrl_is_sending()) {
        ui_toast_show("Stop sending before write", ui_color_danger, 2000);
        return;
    }

    float a_max = strtof(lv_textarea_get_text(g_ta_a_max), nullptr);
    float fly_g = strtof(lv_textarea_get_text(g_ta_fly_gain), nullptr);
    float acc_d = strtof(lv_textarea_get_text(g_ta_acc_del), nullptr);
    int32_t ls  = strtol(lv_textarea_get_text(g_ta_laser_s), nullptr, 10);
    int32_t le  = strtol(lv_textarea_get_text(g_ta_laser_e), nullptr, 10);
    uint8_t fly_en = lv_obj_has_state(g_sw_fly_en, LV_STATE_CHECKED) ? 1 : 0;
    uint8_t acc_en = lv_obj_has_state(g_sw_acc_en, LV_STATE_CHECKED) ? 1 : 0;

    const int wr = ctrl_commit_params(a_max, fly_en, fly_g, acc_en, acc_d, ls, le);
    if(wr == -1) {
        ui_toast_show("Write failed", ui_color_danger, 2500);
    } else if(wr == -2) {
        ui_toast_show("Stopped job, parameters saved", ui_color_accent, 2200);
    } else {
        ui_toast_show("Parameters saved", ui_color_accent, 2200);
    }
    g_form_dirty = false;
}

static void settings_refresh_cb(lv_timer_t *)
{
    bool tcp_conn, serial_conn, read_ok, is_sending;
    float param_a_max, param_fly_g, param_acc_d;
    uint8_t param_fly_en, param_acc_en;
    int32_t param_ls, param_le;

    {
        JOB_LOCK();
        tcp_conn     = g_job_status.tcp_connected;
        serial_conn  = g_job_status.serial_connected;
        is_sending   = g_job_status.is_sending;
        read_ok      = g_job_status.param_read_valid;
        param_a_max  = g_job_status.param_a_max;
        param_fly_en = g_job_status.param_fly_enable;
        param_fly_g  = g_job_status.param_fly_gain;
        param_acc_en = g_job_status.param_acc_enable;
        param_acc_d  = g_job_status.param_acc_delat;
        param_ls     = g_job_status.param_laser_start;
        param_le     = g_job_status.param_laser_stop;
    }

    const bool editable = tcp_conn && !is_sending;
    if(editable != g_last_editable) {
        set_params_form_editable(editable);
        g_last_editable = editable;
        if(!editable) {
            g_form_dirty = false;
        }
    }

    if(tcp_conn && is_sending) {
        lv_label_set_text_fmt(g_lbl_tcp_conn, "TCP: %s  " LV_SYMBOL_WARNING " Sending (locked)",
            LV_SYMBOL_OK " Connected");
        lv_obj_set_style_text_color(g_lbl_tcp_conn, ui_color_danger, 0);
    } else {
        lv_label_set_text_fmt(g_lbl_tcp_conn, "TCP: %s",
            tcp_conn ? LV_SYMBOL_OK " Connected" : LV_SYMBOL_CLOSE " Disconnected");
        lv_obj_set_style_text_color(g_lbl_tcp_conn,
            tcp_conn ? ui_color_accent : ui_color_text_dim, 0);
    }

    lv_label_set_text_fmt(g_lbl_serial_conn, "Serial: %s",
        serial_conn ? LV_SYMBOL_OK " Open" : LV_SYMBOL_CLOSE " Closed");
    lv_obj_set_style_text_color(g_lbl_serial_conn,
        serial_conn ? ui_color_accent : ui_color_text_dim, 0);

    if(!editable) {
        return;
    }

    if(read_ok && !g_form_dirty) {
        sync_params_to_form(param_a_max, param_fly_en, param_fly_g,
                            param_acc_en, param_acc_d, param_ls, param_le);
    }
}

lv_obj_t *page_settings_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_add_style(title, ui_style_title(), 0);

    /* 连接状态卡片 */
    lv_obj_t *conn_card = make_card(page, "Connection Status");
    lv_obj_set_height(conn_card, LV_SIZE_CONTENT);

    g_lbl_tcp_conn = lv_label_create(conn_card);
    lv_label_set_text(g_lbl_tcp_conn, "TCP: --");
    lv_obj_set_style_text_color(g_lbl_tcp_conn, ui_color_text_dim, 0);

    g_lbl_serial_conn = lv_label_create(conn_card);
    lv_label_set_text(g_lbl_serial_conn, "Serial: --");
    lv_obj_set_style_text_color(g_lbl_serial_conn, ui_color_text_dim, 0);

    /* 参数卡片（两列布局） */
    lv_obj_t *param_card = make_card(page, "Board Parameters");
    lv_obj_set_flex_grow(param_card, 1);

    /* 使用一个横向 flex 容器放左右两列 */
    lv_obj_t *cols = lv_obj_create(param_card);
    lv_obj_remove_style_all(cols);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_height(cols, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(cols, UI_PAD, 0);

    /* 左列：Motion */
    lv_obj_t *col_left = lv_obj_create(cols);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_width(col_left, LV_PCT(48));
    lv_obj_set_height(col_left, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col_left, UI_PAD_SM, 0);

    lv_obj_t *lbl_motion = lv_label_create(col_left);
    lv_label_set_text(lbl_motion, "Motion");
    lv_obj_set_style_text_color(lbl_motion, ui_color_accent, 0);
    lv_obj_add_style(lbl_motion, ui_style_subtitle(), 0);

    make_param_row(col_left, "A_MAX",     "0.0000", &g_ta_a_max);
    make_switch_row(col_left, "FLY_ENABLE", &g_sw_fly_en);
    make_param_row(col_left, "FLY_GAIN",  "0.0000", &g_ta_fly_gain);
    make_switch_row(col_left, "ACC_ENABLE", &g_sw_acc_en);
    make_param_row(col_left, "ACC_DELAT", "0.0000", &g_ta_acc_del);

    /* 右列：Laser */
    lv_obj_t *col_right = lv_obj_create(cols);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_width(col_right, LV_PCT(48));
    lv_obj_set_height(col_right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col_right, UI_PAD_SM, 0);

    lv_obj_t *lbl_laser = lv_label_create(col_right);
    lv_label_set_text(lbl_laser, "Laser");
    lv_obj_set_style_text_color(lbl_laser, ui_color_accent, 0);
    lv_obj_add_style(lbl_laser, ui_style_subtitle(), 0);

    make_param_row(col_right, "LASER_START", "0", &g_ta_laser_s);
    make_param_row(col_right, "LASER_STOP",  "0", &g_ta_laser_e);

    /* 按钮行 */
    lv_obj_t *btn_row = lv_obj_create(param_card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, UI_PAD_SM, 0);
    lv_obj_set_style_pad_top(btn_row, UI_PAD, 0);

    g_btn_read = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_read, 90, 36);
    lv_obj_add_style(g_btn_read, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_read, on_read_params, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_r = lv_label_create(g_btn_read);
    lv_label_set_text(lbl_r, LV_SYMBOL_REFRESH " Read");
    lv_obj_center(lbl_r);

    g_btn_write = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_write, 90, 36);
    lv_obj_add_style(g_btn_write, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_write, on_set_params, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_s = lv_label_create(g_btn_write);
    lv_label_set_text(lbl_s, LV_SYMBOL_SAVE " Write");
    lv_obj_center(lbl_s);

    set_params_form_editable(false);
    g_last_editable = false;

    /* 定时刷新 */
    lv_timer_create(settings_refresh_cb, 300, nullptr);

    return page;
}
