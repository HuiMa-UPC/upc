#include "page_settings.hpp"
#include "ctrl_backend.hpp"
#include "job_status.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "page_common.hpp"

#include <cstdio>
#include <cstdlib>

/* ---------- 控件 ---------- */
static lv_obj_t *g_ta_a_max    = nullptr;
static lv_obj_t *g_sw_fly_en   = nullptr;
static lv_obj_t *g_ta_fly_gain = nullptr;
static lv_obj_t *g_sw_acc_en   = nullptr;
static lv_obj_t *g_ta_acc_del  = nullptr;
static lv_obj_t *g_ta_laser_s  = nullptr;
static lv_obj_t *g_ta_laser_e  = nullptr;

static lv_obj_t *g_lbl_tcp_conn    = nullptr;
static lv_obj_t *g_lbl_serial_conn = nullptr;
static lv_obj_t *g_btn_read        = nullptr;
static lv_obj_t *g_btn_write       = nullptr;

static bool g_form_dirty     = false;
static bool g_last_editable  = false;
static bool g_last_tcp_conn  = false;
static uint32_t s_settings_last_ui_gen = 0;

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
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        if(lv_obj_has_state(obj, LV_STATE_FOCUSED)) {
            lv_obj_remove_state(obj, LV_STATE_FOCUSED);
        }
    }
}

static void set_params_form_editable(bool editable)
{
    static lv_obj_t *const widgets[] = {
        g_ta_a_max, g_ta_fly_gain, g_ta_acc_del, g_ta_laser_s, g_ta_laser_e,
        g_sw_fly_en, g_sw_acc_en, g_btn_read, g_btn_write,
    };
    static lv_obj_t *const textareas[] = {
        g_ta_a_max, g_ta_fly_gain, g_ta_acc_del, g_ta_laser_s, g_ta_laser_e,
    };
    for(lv_obj_t *obj : widgets) {
        set_obj_enabled(obj, editable);
    }
    /* 仅释放本页输入框上的键盘，勿 ui_keyboard_hide() 误关其它页面（如 Control IP） */
    if(!editable) {
        for(lv_obj_t *ta : textareas) {
            ui_keyboard_release_if_focused(ta);
        }
    }
}

static void set_ta_float(lv_obj_t *ta, float v)
{
    if(!ta) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", v);
    lv_textarea_set_text(ta, buf);
}

static void set_ta_int(lv_obj_t *ta, int32_t v)
{
    if(!ta) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)v);
    lv_textarea_set_text(ta, buf);
}

static void set_sw_checked(lv_obj_t *sw, uint8_t on)
{
    if(!sw) {
        return;
    }
    if(on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

static void sync_params_to_form(float param_a_max, uint8_t param_fly_en, float param_fly_g,
                                uint8_t param_acc_en, float param_acc_d,
                                int32_t param_ls, int32_t param_le)
{
    set_ta_float(g_ta_a_max, param_a_max);
    set_sw_checked(g_sw_fly_en, param_fly_en);
    set_ta_float(g_ta_fly_gain, param_fly_g);
    set_sw_checked(g_sw_acc_en, param_acc_en);
    set_ta_float(g_ta_acc_del, param_acc_d);
    set_ta_int(g_ta_laser_s, param_ls);
    set_ta_int(g_ta_laser_e, param_le);
}

/* ---------- UI 辅助 ---------- */
static void style_settings_label(lv_obj_t *lbl)
{
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl, ui_font_form(), 0);
    lv_obj_set_width(lbl, UI_SETTINGS_LABEL_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
}

static void style_settings_ta(lv_obj_t *ta)
{
    lv_obj_set_size(ta, UI_SETTINGS_TA_W, UI_SETTINGS_ROW_H);
    lv_obj_set_style_bg_color(ta, ui_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, ui_color_border, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(ta, UI_PAD_SM, 0);
    lv_obj_set_style_text_font(ta, ui_font_form(), 0);
    lv_obj_set_style_text_color(ta, ui_color_text, 0);
}

/** Settings 参数行：横向 flex，固定行高 */
static lv_obj_t *make_settings_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, UI_SETTINGS_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);
    return row;
}

static lv_obj_t *make_settings_label(lv_obj_t *row, const char *name)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    style_settings_label(lbl);
    return lbl;
}

static lv_obj_t *make_param_row(lv_obj_t *parent, const char *name,
                                const char *init_val, lv_obj_t **out_ta)
{
    lv_obj_t *row = make_settings_row(parent);
    make_settings_label(row, name);

    *out_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(*out_ta, true);
    lv_textarea_set_text(*out_ta, init_val);
    style_settings_ta(*out_ta);
    ui_keyboard_attach(*out_ta, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(*out_ta, mark_form_dirty, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

static lv_obj_t *make_switch_row(lv_obj_t *parent, const char *name, lv_obj_t **out_sw)
{
    lv_obj_t *row = make_settings_row(parent);
    make_settings_label(row, name);

    *out_sw = lv_switch_create(row);
    ui_theme_apply_switch(*out_sw);
    lv_obj_add_event_cb(*out_sw, mark_form_dirty, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *caption, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_ACTION_W, UI_INPUT_H);
    lv_obj_add_style(btn, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, caption);
    lv_obj_center(lbl);
    return btn;
}

/** 参数区单列（Motion / Laser） */
static lv_obj_t *make_param_column(lv_obj_t *parent, const char *section_title)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, LV_PCT(48));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, UI_PAD_SM, 0);

    lv_obj_t *hdr = lv_label_create(col);
    lv_label_set_text(hdr, section_title);
    lv_obj_set_style_text_color(hdr, ui_color_accent, 0);
    lv_obj_add_style(hdr, ui_style_subtitle(), 0);
    return col;
}

static lv_obj_t *make_flex_row(lv_obj_t *parent, lv_coord_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);
    return row;
}

/* ---------- 回调 ---------- */
/** @return true 表示可继续读/写参数 */
static bool guard_params_action(const char *busy_msg)
{
    if(!ctrl_tcp_is_connected()) {
        ui_toast_show("TCP not connected", ui_color_danger, 2000);
        return false;
    }
    if(ctrl_is_sending()) {
        ui_toast_show(busy_msg, ui_color_danger, 2000);
        return false;
    }
    return true;
}

static void on_read_params(lv_event_t *)
{
    if(!guard_params_action("Stop sending before read")) {
        return;
    }
    g_form_dirty = false;
    ctrl_request_read_params();
    ui_toast_show("Reading board parameters...", ui_color_text, 1500);
}

static void on_set_params(lv_event_t *)
{
    if(!guard_params_action("Stop sending before write")) {
        return;
    }

    const float a_max  = strtof(lv_textarea_get_text(g_ta_a_max), nullptr);
    const float fly_g  = strtof(lv_textarea_get_text(g_ta_fly_gain), nullptr);
    const float acc_d  = strtof(lv_textarea_get_text(g_ta_acc_del), nullptr);
    const int32_t ls   = strtol(lv_textarea_get_text(g_ta_laser_s), nullptr, 10);
    const int32_t le   = strtol(lv_textarea_get_text(g_ta_laser_e), nullptr, 10);
    const uint8_t fly_en = lv_obj_has_state(g_sw_fly_en, LV_STATE_CHECKED) ? 1 : 0;
    const uint8_t acc_en = lv_obj_has_state(g_sw_acc_en, LV_STATE_CHECKED) ? 1 : 0;

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

static void update_conn_labels(bool tcp_conn, bool serial_conn, bool is_sending)
{
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
}

static void page_settings_refresh_impl(void)
{
    bool tcp_conn, serial_conn, read_ok, is_sending;
    float param_a_max, param_fly_g, param_acc_d;
    uint8_t param_fly_en, param_acc_en;
    int32_t param_ls, param_le;

    {
        JOB_LOCK();
        tcp_conn     = g_job_status.tcp_connected;
        serial_conn  = g_job_status.serial_connected;
        read_ok      = g_job_status.param_read_valid;
        param_a_max  = g_job_status.param_a_max;
        param_fly_en = g_job_status.param_fly_enable;
        param_fly_g  = g_job_status.param_fly_gain;
        param_acc_en = g_job_status.param_acc_enable;
        param_acc_d  = g_job_status.param_acc_delat;
        param_ls     = g_job_status.param_laser_start;
        param_le     = g_job_status.param_laser_stop;
    }
    is_sending = ctrl_is_sending() != 0;

    const bool editable = tcp_conn && !is_sending;
    set_params_form_editable(editable);
    if(editable != g_last_editable) {
        g_last_editable = editable;
        if(!editable) {
            g_form_dirty = false;
        }
    }

    if(tcp_conn && !g_last_tcp_conn) {
        g_form_dirty = false;
    }
    g_last_tcp_conn = tcp_conn;

    update_conn_labels(tcp_conn, serial_conn, is_sending);

    if(!editable) {
        return;
    }

    if(read_ok && !g_form_dirty) {
        sync_params_to_form(param_a_max, param_fly_en, param_fly_g,
                            param_acc_en, param_acc_d, param_ls, param_le);
    }
}

void page_settings_refresh(bool force)
{
    const uint32_t gen = job_status_ui_generation();
    if(!force && gen == s_settings_last_ui_gen) {
        return;
    }
    s_settings_last_ui_gen = gen;
    page_settings_refresh_impl();
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

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_add_style(title, ui_style_title(), 0);

    lv_obj_t *conn_card = ui_make_card(page, "Connection Status");
    lv_obj_set_height(conn_card, LV_SIZE_CONTENT);

    g_lbl_tcp_conn = lv_label_create(conn_card);
    lv_label_set_text(g_lbl_tcp_conn, "TCP: --");
    lv_obj_set_style_text_color(g_lbl_tcp_conn, ui_color_text_dim, 0);

    g_lbl_serial_conn = lv_label_create(conn_card);
    lv_label_set_text(g_lbl_serial_conn, "Serial: --");
    lv_obj_set_style_text_color(g_lbl_serial_conn, ui_color_text_dim, 0);

    lv_obj_t *param_card = ui_make_card(page, "Board Parameters");
    lv_obj_set_flex_grow(param_card, 1);
    lv_obj_set_style_min_height(param_card, 0, 0);
    lv_obj_add_flag(param_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(param_card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(param_card, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *btn_row = make_flex_row(param_card, UI_INPUT_H);
    g_btn_read  = make_action_btn(btn_row, LV_SYMBOL_REFRESH " Read", on_read_params);
    g_btn_write = make_action_btn(btn_row, LV_SYMBOL_SAVE " Write", on_set_params);

    lv_obj_t *cols = lv_obj_create(param_card);
    lv_obj_remove_style_all(cols);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_height(cols, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(cols, UI_PAD, 0);
    lv_obj_set_style_pad_top(cols, UI_PAD_SM, 0);

    lv_obj_t *col_motion = make_param_column(cols, "Motion");
    /* 线上值 = 内部加速度 / 1e8；默认 10e6 内部 → 0.1 */
    make_param_row(col_motion, "A_MAX", "0.1000", &g_ta_a_max);
    make_switch_row(col_motion, "FLY_ENABLE", &g_sw_fly_en);
    make_param_row(col_motion, "FLY_GAIN", "500.0000", &g_ta_fly_gain);
    make_switch_row(col_motion, "ACC_ENABLE", &g_sw_acc_en);
    make_param_row(col_motion, "ACC_DELAT", "0.0000", &g_ta_acc_del);

    lv_obj_t *col_laser = make_param_column(cols, "Laser");
    make_param_row(col_laser, "LASER_START", "0", &g_ta_laser_s);
    make_param_row(col_laser, "LASER_STOP", "0", &g_ta_laser_e);

    set_params_form_editable(false);
    g_last_editable = false;

    return page;
}
