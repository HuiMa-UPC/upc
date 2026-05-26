#include "page_control.hpp"
#include "ctrl_backend.hpp"
#include "job_status.hpp"
#include "gcode_parse.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "page_common.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <glob.h>

/* ---------- 控件 ---------- */
static lv_obj_t *g_ip_ta         = nullptr;
static lv_obj_t *g_port_ta       = nullptr;
static lv_obj_t *g_btn_tcp       = nullptr;
static lv_obj_t *g_lbl_tcp_st    = nullptr;

static lv_obj_t *g_serial_dev_dd  = nullptr;
static lv_obj_t *g_serial_baud_dd = nullptr;
static const int k_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
static lv_obj_t *g_btn_serial    = nullptr;
static lv_obj_t *g_lbl_serial_st = nullptr;

static lv_obj_t *g_lbl_file      = nullptr;
static lv_obj_t *g_batch_ta      = nullptr;
static lv_obj_t *g_interval_ta   = nullptr;
static lv_obj_t *g_btn_start     = nullptr;
static lv_obj_t *g_btn_reset     = nullptr;
static lv_obj_t *g_sw_layer      = nullptr;
static lv_obj_t *g_sw_cal        = nullptr;

static lv_obj_t *g_ta_img_scale  = nullptr;
static lv_obj_t *g_ta_img_range  = nullptr;
static lv_obj_t *g_ta_speed_gain = nullptr;

enum {
    JOG_W_Y_PLUS = 0,
    JOG_W_Y_MINUS,
    JOG_W_X_MINUS,
    JOG_W_HOME,
    JOG_W_X_PLUS,
    JOG_W_STEP_0,
    JOG_W_STEP_1,
    JOG_W_STEP_2,
    JOG_W_STEP_3,
    JOG_W_COUNT,
};

static lv_obj_t *g_jog_widgets[JOG_W_COUNT] = {};
static lv_obj_t *g_lbl_center_off_x = nullptr;
static lv_obj_t *g_lbl_center_off_y = nullptr;
static lv_obj_t *g_btn_center_enable = nullptr;

static float g_jog_step = 1.f;
static const float k_jog_steps[] = {0.01f, 0.1f, 1.f, 10.f};

/* ---------- UI 辅助 ---------- */
static lv_obj_t *make_control_flex_row(lv_obj_t *parent, lv_coord_t pad_column)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, pad_column, 0);
    return row;
}

static lv_obj_t *make_form_label(lv_obj_t *parent, const char *text, int width,
                                 lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl, ui_font_body(), 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    return lbl;
}

static void style_control_ta(lv_obj_t *ta, lv_coord_t width)
{
    lv_obj_set_width(ta, width);
    lv_obj_set_height(ta, UI_INPUT_H);
    lv_obj_set_style_bg_color(ta, ui_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, ui_color_border, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(ta, UI_PAD_SM, 0);
    lv_obj_set_style_text_font(ta, ui_font_body(), 0);
    lv_obj_set_style_text_color(ta, ui_color_text, 0);
}

static lv_obj_t *make_labeled_ta(lv_obj_t *parent, const char *label,
                                 const char *init_text, int label_w, int ta_w,
                                 lv_keyboard_mode_t kb_mode,
                                 lv_text_align_t label_align)
{
    make_form_label(parent, label, label_w, label_align);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, init_text);
    style_control_ta(ta, ta_w);
    ui_keyboard_attach(ta, kb_mode);
    return ta;
}

static lv_obj_t *make_conn_status_label(lv_obj_t *row, const char *initial)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, initial);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_flex_grow(lbl, 1);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    return lbl;
}

static lv_obj_t *make_conn_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_CONN_W, UI_INPUT_H);
    lv_obj_add_style(btn, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btn, ui_color_accent, LV_STATE_PRESSED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static void update_conn_button(lv_obj_t *btn, bool connected)
{
    lv_obj_set_style_bg_color(btn, connected ? ui_color_danger : ui_color_accent_dim, 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if(lbl) {
        lv_label_set_text(lbl, connected ? "Disconnect" : "Connect");
    }
}

static lv_obj_t *make_job_button(lv_obj_t *parent, const char *caption, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_JOB_W, UI_INPUT_H);
    lv_obj_add_style(btn, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btn, ui_color_panel_alt, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, caption);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, LV_STATE_DISABLED);
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    return btn;
}

static lv_obj_t *make_labeled_switch_row(lv_obj_t *parent, const char *label_text,
                                         int label_w, lv_obj_t **out_sw,
                                         lv_event_cb_t cb)
{
    lv_obj_t *row = make_control_flex_row(parent, UI_PAD_SM);
    make_form_label(row, label_text, label_w, LV_TEXT_ALIGN_LEFT);
    *out_sw = lv_switch_create(row);
    ui_theme_apply_switch(*out_sw);
    lv_obj_add_event_cb(*out_sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

static void style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_height(dd, UI_INPUT_H);
    lv_obj_set_style_bg_color(dd, ui_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dd, ui_color_border, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(dd, UI_PAD_SM, 0);
    lv_obj_set_style_text_font(dd, ui_font_body(), 0);
    lv_obj_set_style_text_color(dd, ui_color_text, 0);
}

static void set_obj_enabled(lv_obj_t *obj, bool enabled)
{
    if(!obj) {
        return;
    }
    const bool currently_disabled = lv_obj_has_state(obj, LV_STATE_DISABLED);
    if(enabled && currently_disabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else if(!enabled && !currently_disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

static void set_widget_editable(lv_obj_t *obj, bool editable, bool manage_clickable)
{
    if(!obj) {
        return;
    }
    LV_UNUSED(manage_clickable);

    /* 编辑中保持可输入；强制锁定时仍须 disable */
    if(editable && ui_keyboard_is_bound(obj)) {
        return;
    }

    const bool currently_disabled = lv_obj_has_state(obj, LV_STATE_DISABLED);
    const bool currently_clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    if(editable) {
        if(currently_disabled) {
            lv_obj_remove_state(obj, LV_STATE_DISABLED);
        }
        if(!currently_clickable) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        }
    } else {
        if(!currently_disabled) {
            lv_obj_add_state(obj, LV_STATE_DISABLED);
        }
        if(currently_clickable) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        }
        ui_keyboard_release_if_focused(obj);
        if(lv_obj_has_state(obj, LV_STATE_FOCUSED)) {
            lv_obj_remove_state(obj, LV_STATE_FOCUSED);
        }
    }
}

static void set_widgets_editable(lv_obj_t *const *widgets, size_t count, bool editable,
                                 bool manage_clickable)
{
    for(size_t i = 0; i < count; i++) {
        set_widget_editable(widgets[i], editable, manage_clickable);
    }
}

/* ---------- 连接 ---------- */
static void update_tcp_ui(bool connected)
{
    update_conn_button(g_btn_tcp, connected);
    lv_label_set_text(g_lbl_tcp_st, connected ? "Connected" : "Disconnected");
}

static void on_tcp_click(lv_event_t *)
{
    if(ctrl_tcp_is_connected()) {
        ctrl_tcp_disconnect();
        update_tcp_ui(false);
        return;
    }
    const char *ip = lv_textarea_get_text(g_ip_ta);
    int port = atoi(lv_textarea_get_text(g_port_ta));
    if(port <= 0) {
        port = 1502;
    }
    if(ctrl_tcp_connect(ip, port) == 0) {
        update_tcp_ui(true);
    } else {
        lv_label_set_text(g_lbl_tcp_st, "Connect failed");
    }
}

static void scan_serial_ports(void)
{
    static const char *patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*"};
    char options[512] = {0};

    for(const char *pat : patterns) {
        glob_t globbuf;
        if(glob(pat, 0, nullptr, &globbuf) != 0) {
            continue;
        }
        for(size_t j = 0; j < globbuf.gl_pathc; j++) {
            if(options[0]) {
                strncat(options, "\n", sizeof(options) - strlen(options) - 1);
            }
            strncat(options, globbuf.gl_pathv[j], sizeof(options) - strlen(options) - 1);
        }
        globfree(&globbuf);
    }

    lv_dropdown_set_options(g_serial_dev_dd,
        options[0] ? options : "/dev/ttyUSB0");
}

static void on_serial_click(lv_event_t *)
{
    if(ctrl_serial_is_open()) {
        ctrl_serial_close();
        update_conn_button(g_btn_serial, false);
        lv_label_set_text(g_lbl_serial_st, "Closed");
        return;
    }

    char dev[64] = {0};
    lv_dropdown_get_selected_str(g_serial_dev_dd, dev, sizeof(dev));
    const uint16_t sel = lv_dropdown_get_selected(g_serial_baud_dd);
    const int baud = (sel < sizeof(k_bauds) / sizeof(k_bauds[0])) ? k_bauds[sel] : 115200;

    if(ctrl_serial_open(dev, baud) == 0) {
        update_conn_button(g_btn_serial, true);
        lv_label_set_text(g_lbl_serial_st, "Open");
    } else {
        lv_label_set_text(g_lbl_serial_st, "Open failed");
    }
}

/* ---------- 作业 / 增益 ---------- */
static bool job_gcode_ready(void)
{
    bool has_file = false;
    bool print_done = false;
    {
        JOB_LOCK();
        has_file = (g_job_status.current_file[0] != '\0');
        print_done = g_job_status.job_print_completed;
    }
    if(!has_file) {
        return false;
    }
    if(print_done && ctrl_is_sending() == 0) {
        return true;
    }
    return ctrl_ring_ready_for_start() != 0;
}

static void sync_start_button_label(bool is_sending, bool is_paused, bool start_pending)
{
    lv_obj_t *lbl = lv_obj_get_child(g_btn_start, 0);
    if(!lbl) {
        return;
    }
    if(is_sending && !is_paused) {
        lv_label_set_text(lbl, "Pause");
    } else if(start_pending) {
        lv_label_set_text(lbl, "Preparing...");
    } else {
        lv_label_set_text(lbl, "Start");
    }
}

static void on_start_click(lv_event_t *)
{
    if(!ctrl_tcp_is_connected()) {
        return;
    }
    if(ctrl_is_sending()) {
        if(ctrl_is_paused()) {
            ctrl_resume_sending();
        } else {
            ctrl_pause_sending();
        }
        return;
    }
    if(ctrl_is_start_pending() || !job_gcode_ready()) {
        return;
    }
    ctrl_start_sending();
}

static void on_reset_click(lv_event_t *)
{
    ctrl_send_reset();
}

static void on_layer_sw(lv_event_t *e)
{
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if(!ctrl_serial_is_open()) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ctrl_set_enable_layer_change(0);
        return;
    }
    ctrl_set_enable_layer_change(lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

static void sync_layer_switch_interactive(bool serial_connected, bool is_sending)
{
    if(!g_sw_layer) {
        return;
    }
    if(is_sending || !serial_connected) {
        set_widget_editable(g_sw_layer, false, true);
        if(!serial_connected && lv_obj_has_state(g_sw_layer, LV_STATE_CHECKED)) {
            lv_obj_remove_state(g_sw_layer, LV_STATE_CHECKED);
            ctrl_set_enable_layer_change(0);
        }
        return;
    }
    set_widget_editable(g_sw_layer, true, true);
}

static void on_cal_sw(lv_event_t *e)
{
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int want_en = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;

    if(!ctrl_tcp_is_connected()) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ctrl_set_enable_cal_apply(0);
        ui_toast_show("TCP not connected", ui_color_danger, 2000);
        return;
    }
    bool cal_loaded = false;
    bool cal_has_gains = false;
    {
        JOB_LOCK();
        cal_loaded = g_job_status.cal_file_loaded;
        cal_has_gains = g_job_status.cal_has_gains;
    }
    if(!cal_loaded || !cal_has_gains) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ctrl_set_enable_cal_apply(0);
        ui_toast_show("Load valid CAL file first (need CAL,scale,range,speed)",
                      ui_color_danger, 4000);
        return;
    }
    if(ctrl_is_sending() || ctrl_is_start_pending()) {
        if(want_en) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        ui_toast_show("Stop the job before changing calibration", ui_color_danger, 2500);
        return;
    }
    bool has_file = false;
    {
        JOB_LOCK();
        has_file = (g_job_status.current_file[0] != '\0');
    }
    if(!has_file) {
        if(want_en) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        ui_toast_show("Open G-code first", ui_color_danger, 2000);
        return;
    }

    ctrl_set_enable_cal_apply(want_en);
    ctrl_reparse_gcode();
    if(want_en) {
        ui_toast_show("Calibration enabled, re-parsing G-code", ui_color_accent, 2500);
    } else {
        ui_toast_show("Calibration disabled, re-parsing G-code", ui_color_accent, 2500);
    }
}

static void sync_cal_switch_interactive(bool tcp_connected, bool cal_loaded,
                                        bool cal_has_gains, bool is_sending)
{
    if(!g_sw_cal) {
        return;
    }
    const bool can_use = tcp_connected && cal_loaded && cal_has_gains && !is_sending;
    if(!can_use) {
        set_widget_editable(g_sw_cal, false, true);
        if(!tcp_connected || !cal_loaded || !cal_has_gains) {
            if(lv_obj_has_state(g_sw_cal, LV_STATE_CHECKED)) {
                lv_obj_remove_state(g_sw_cal, LV_STATE_CHECKED);
            }
            ctrl_set_enable_cal_apply(0);
        }
        return;
    }
    set_widget_editable(g_sw_cal, true, true);
    if(ctrl_get_enable_cal_apply()) {
        lv_obj_add_state(g_sw_cal, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(g_sw_cal, LV_STATE_CHECKED);
    }
}

static float parse_gain_text(const char *text, float default_val)
{
    if(!text || text[0] == '\0') {
        return default_val;
    }
    char *end = nullptr;
    const float v = strtof(text, &end);
    return (end == text) ? default_val : v;
}

static float default_gain_for_ta(const lv_obj_t *ta)
{
    if(ta == g_ta_img_range) {
        return GCODE_DEFAULT_IMAGE_RANG_GAIN;
    }
    if(ta == g_ta_speed_gain) {
        return GCODE_DEFAULT_SPEED_GAIN;
    }
    return GCODE_DEFAULT_IMAGE_SCALE_GAIN;
}

static void sync_parse_gains_from_ui(void)
{
    const float scale = parse_gain_text(lv_textarea_get_text(g_ta_img_scale),
                                        GCODE_DEFAULT_IMAGE_SCALE_GAIN);
    const float range = parse_gain_text(lv_textarea_get_text(g_ta_img_range),
                                        GCODE_DEFAULT_IMAGE_RANG_GAIN);
    const float spd   = parse_gain_text(lv_textarea_get_text(g_ta_speed_gain),
                                        GCODE_DEFAULT_SPEED_GAIN);

    JOB_LOCK();
    g_job_status.parse_image_scale_gain = scale;
    g_job_status.parse_image_rang_gain  = (range > 1e-6f) ? range : GCODE_DEFAULT_IMAGE_RANG_GAIN;
    g_job_status.parse_speed_gain       = (spd > 1e-6f) ? spd : GCODE_DEFAULT_SPEED_GAIN;
}

static void on_parse_gain_ta_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));

    if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY) {
        char fmt_buf[24];
        const float def = default_gain_for_ta(ta);
        snprintf(fmt_buf, sizeof(fmt_buf), "%.3f",
                 parse_gain_text(lv_textarea_get_text(ta), def));
        lv_textarea_set_text(ta, fmt_buf);
    }

    if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY ||
       code == LV_EVENT_VALUE_CHANGED) {
        sync_parse_gains_from_ui();
    }
}

static void attach_parse_gain_handlers(lv_obj_t *ta)
{
    lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_READY, nullptr);
}

static void load_gain_init_strings(char init[][16])
{
    float scale = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
    float range = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    float speed = GCODE_DEFAULT_SPEED_GAIN;
    {
        JOB_LOCK();
        if(g_job_status.parse_image_scale_gain > 1e-6f) {
            scale = g_job_status.parse_image_scale_gain;
        }
        if(g_job_status.parse_image_rang_gain > 1e-6f) {
            range = g_job_status.parse_image_rang_gain;
        }
        if(g_job_status.parse_speed_gain > 1e-6f) {
            speed = g_job_status.parse_speed_gain;
        }
    }
    snprintf(init[0], 16, "%.3f", scale);
    snprintf(init[1], 16, "%.3f", range);
    snprintf(init[2], 16, "%.3f", speed);
}

static void on_batch_ta_changed(lv_event_t *)
{
    ctrl_set_batch_size(atoi(lv_textarea_get_text(g_batch_ta)));
}

static void on_interval_ta_changed(lv_event_t *)
{
    ctrl_set_interval_ms(atoi(lv_textarea_get_text(g_interval_ta)));
}

static lv_obj_t *make_jog_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                 const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_add_style(btn, ui_style_btn_primary(), 0);
    lv_obj_set_style_bg_color(btn, ui_color_panel, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, ui_color_accent_dim, 0);
    lv_obj_set_style_bg_color(btn, ui_color_accent_dim, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, ui_font_body(), 0);
    lv_obj_center(lbl);
    return btn;
}

/** D-pad 内行：固定内容宽，勿用 LV_PCT(100)（父为 SIZE_CONTENT 时宽度会塌成 0） */
static lv_obj_t *make_jog_inner_row(lv_obj_t *parent, lv_coord_t pad_column)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, pad_column, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *make_jog_spacer(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *sp = lv_obj_create(parent);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, w, h);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    return sp;
}

static void on_jog_y_plus(lv_event_t *)
{
    ctrl_center_offset_nudge(0.f, g_jog_step);
}

static void on_jog_y_minus(lv_event_t *)
{
    ctrl_center_offset_nudge(0.f, -g_jog_step);
}

static void on_jog_x_minus(lv_event_t *)
{
    ctrl_center_offset_nudge(-g_jog_step, 0.f);
}

static void on_jog_x_plus(lv_event_t *)
{
    ctrl_center_offset_nudge(g_jog_step, 0.f);
}

static void on_jog_home(lv_event_t *)
{
    ctrl_center_offset_reset();
}

static void on_center_offset_enable(lv_event_t *)
{
    if(ctrl_is_sending() || ctrl_is_start_pending()) {
        return;
    }
    bool has_file = false;
    {
        JOB_LOCK();
        has_file = (g_job_status.current_file[0] != '\0');
    }
    if(!has_file) {
        ui_toast_show("Open G-code first", ui_color_danger, 2000);
        return;
    }
    ctrl_reparse_gcode();
    ui_toast_show("Re-parsing G-code", ui_color_accent, 2000);
}

static void on_jog_step_click(lv_event_t *e)
{
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if(idx < 0 || idx >= 4) {
        return;
    }
    g_jog_step = k_jog_steps[idx];
    for(int i = 0; i < 4; ++i) {
        lv_obj_t *btn = g_jog_widgets[JOG_W_STEP_0 + i];
        if(!btn) {
            continue;
        }
        if(i == idx) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(btn, LV_STATE_CHECKED);
        }
    }
}

static void sync_jog_interactive(bool can_edit)
{
    for(int i = 0; i < JOG_W_COUNT; ++i) {
        if(g_jog_widgets[i]) {
            set_obj_enabled(g_jog_widgets[i], can_edit);
        }
    }
}

static void sync_center_offset_panel(bool can_enable, float off_x, float off_y)
{
    if(g_lbl_center_off_x) {
        lv_label_set_text_fmt(g_lbl_center_off_x, "X: %.3f", off_x);
    }
    if(g_lbl_center_off_y) {
        lv_label_set_text_fmt(g_lbl_center_off_y, "Y: %.3f", off_y);
    }
    if(g_btn_center_enable) {
        set_obj_enabled(g_btn_center_enable, can_enable);
    }
}

static void make_center_jog_panel(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Center Offset");
    lv_obj_add_style(title, ui_style_subtitle(), 0);

    lv_obj_t *jog_block = lv_obj_create(parent);
    lv_obj_remove_style_all(jog_block);
    lv_obj_set_width(jog_block, LV_PCT(100));
    lv_obj_set_height(jog_block, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(jog_block, UI_JOG_BLOCK_MIN_H, 0);
    lv_obj_set_flex_flow(jog_block, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(jog_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(jog_block, UI_PAD, 0);
    lv_obj_set_style_pad_column(jog_block, UI_PAD, 0);
    lv_obj_set_style_bg_color(jog_block, ui_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(jog_block, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(jog_block, UI_RADIUS, 0);
    lv_obj_set_style_border_width(jog_block, 1, 0);
    lv_obj_set_style_border_color(jog_block, ui_color_border, 0);
    lv_obj_clear_flag(jog_block, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t gap = UI_PAD_SM;
    const lv_coord_t dpad_w = UI_JOG_BTN_SZ * 3 + gap * 2;

    lv_obj_t *offset_col = lv_obj_create(jog_block);
    lv_obj_remove_style_all(offset_col);
    lv_obj_set_width(offset_col, UI_FORM_LABEL2_W + UI_JOG_STEP_W);
    lv_obj_set_height(offset_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(offset_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(offset_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(offset_col, UI_PAD_SM, 0);
    lv_obj_clear_flag(offset_col, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_center_off_x = lv_label_create(offset_col);
    lv_label_set_text(g_lbl_center_off_x, "X: 0.000");
    lv_obj_set_style_text_color(g_lbl_center_off_x, ui_color_text, 0);
    lv_obj_set_width(g_lbl_center_off_x, LV_PCT(100));

    g_lbl_center_off_y = lv_label_create(offset_col);
    lv_label_set_text(g_lbl_center_off_y, "Y: 0.000");
    lv_obj_set_style_text_color(g_lbl_center_off_y, ui_color_text, 0);
    lv_obj_set_width(g_lbl_center_off_y, LV_PCT(100));

    lv_obj_t *dpad = lv_obj_create(jog_block);
    lv_obj_remove_style_all(dpad);
    lv_obj_set_size(dpad, dpad_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dpad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dpad, gap, 0);
    lv_obj_clear_flag(dpad, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row_up = make_jog_inner_row(dpad, gap);
    make_jog_spacer(row_up, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ);
    g_jog_widgets[JOG_W_Y_PLUS] = make_jog_button(row_up, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ, "Y+",
                                                  on_jog_y_plus);
    make_jog_spacer(row_up, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ);

    lv_obj_t *row_mid = make_jog_inner_row(dpad, gap);
    g_jog_widgets[JOG_W_X_MINUS] = make_jog_button(row_mid, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ, "X-",
                                                   on_jog_x_minus);
    g_jog_widgets[JOG_W_HOME] = make_jog_button(row_mid, UI_JOG_HOME_SZ, UI_JOG_HOME_SZ,
                                                LV_SYMBOL_HOME, on_jog_home);
    g_jog_widgets[JOG_W_X_PLUS] = make_jog_button(row_mid, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ, "X+",
                                                  on_jog_x_plus);

    lv_obj_t *row_dn = make_jog_inner_row(dpad, gap);
    make_jog_spacer(row_dn, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ);
    g_jog_widgets[JOG_W_Y_MINUS] = make_jog_button(row_dn, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ, "Y-",
                                                   on_jog_y_minus);
    make_jog_spacer(row_dn, UI_JOG_BTN_SZ, UI_JOG_BTN_SZ);

    lv_obj_t *step_col = lv_obj_create(jog_block);
    lv_obj_remove_style_all(step_col);
    lv_obj_set_width(step_col, UI_JOG_STEP_W);
    lv_obj_set_height(step_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(step_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(step_col, UI_PAD_SM, 0);
    lv_obj_clear_flag(step_col, LV_OBJ_FLAG_SCROLLABLE);

    static const char *const step_labels[] = {"0.01", "0.1", "1", "10"};
    for(int i = 0; i < 4; ++i) {
        lv_obj_t *btn = lv_button_create(step_col);
        lv_obj_set_width(btn, UI_JOG_STEP_W);
        lv_obj_set_height(btn, UI_JOG_STEP_BTN_H);
        lv_obj_add_style(btn, ui_style_btn_primary(), 0);
        lv_obj_set_style_bg_color(btn, ui_color_panel, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, ui_color_border, 0);
        lv_obj_set_style_bg_color(btn, ui_color_accent_dim, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, ui_color_accent, LV_STATE_CHECKED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_event_cb(btn, on_jog_step_click, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, step_labels[i]);
        lv_obj_center(lbl);
        g_jog_widgets[JOG_W_STEP_0 + i] = btn;
    }
    if(g_jog_widgets[JOG_W_STEP_2]) {
        lv_obj_add_state(g_jog_widgets[JOG_W_STEP_2], LV_STATE_CHECKED);
    }
}

/** G-code Parse 底行：左侧 Calibration 开关，右侧 Enable（应用中心偏移并重解析） */
static void make_parse_cal_enable_row(lv_obj_t *parent)
{
    lv_obj_t *row = make_control_flex_row(parent, UI_PAD_SM);
    make_form_label(row, "Calibration:", UI_SWITCH_LABEL_W, LV_TEXT_ALIGN_LEFT);
    g_sw_cal = lv_switch_create(row);
    ui_theme_apply_switch(g_sw_cal);
    lv_obj_add_event_cb(g_sw_cal, on_cal_sw, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *fill = lv_obj_create(row);
    lv_obj_remove_style_all(fill);
    lv_obj_set_height(fill, 1);
    lv_obj_set_flex_grow(fill, 1);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);

    g_btn_center_enable = lv_button_create(row);
    lv_obj_set_size(g_btn_center_enable, UI_BTN_JOB_W, UI_JOG_STEP_BTN_H);
    lv_obj_add_style(g_btn_center_enable, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_center_enable, on_center_offset_enable, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *enable_lbl = lv_label_create(g_btn_center_enable);
    if(enable_lbl) {
        lv_label_set_text(enable_lbl, "Enable");
        lv_obj_center(enable_lbl);
    }
}

static void page_control_refresh_impl(void)
{
    char file_name[256];
    bool tcp_connected;
    bool is_sending, is_paused;
    bool cal_loaded, cal_enabled, cal_has_gains;
    float center_off_x = 0.f;
    float center_off_y = 0.f;

    {
        JOB_LOCK();
        strncpy(file_name, g_job_status.current_file, sizeof(file_name) - 1);
        file_name[sizeof(file_name) - 1] = '\0';
        tcp_connected   = g_job_status.tcp_connected;
        cal_loaded      = g_job_status.cal_file_loaded;
        cal_enabled     = g_job_status.enable_cal_apply;
        cal_has_gains   = g_job_status.cal_has_gains;
        center_off_x    = g_job_status.center_offset_x;
        center_off_y    = g_job_status.center_offset_y;
    }

    is_sending = ctrl_is_sending() != 0;
    is_paused  = ctrl_is_paused() != 0;
    const bool start_pending    = ctrl_is_start_pending() != 0;
    const bool serial_connected = ctrl_serial_is_open() != 0;
    const bool offset_idle      = !is_sending && !start_pending;
    const bool has_gcode_file   = (file_name[0] != '\0');

    sync_center_offset_panel(tcp_connected && offset_idle && has_gcode_file,
                             center_off_x, center_off_y);

    lv_label_set_text_fmt(g_lbl_file, "File: %s", file_name[0] ? file_name : "--");

    /* 键盘打开时不刷新门控，避免 disable/失焦把键盘立刻关掉 */
    if(ui_keyboard_is_open()) {
        return;
    }

    if(!tcp_connected) {
        /* 未连 TCP：仅 IP/Port 可编辑；其余输入框与按钮全部禁用 */
        set_widget_editable(g_ip_ta, true, false);
        set_widget_editable(g_port_ta, true, false);
        set_widget_editable(g_serial_dev_dd, false, false);
        set_widget_editable(g_serial_baud_dd, false, false);
        set_obj_enabled(g_btn_tcp, true);
        set_obj_enabled(g_btn_serial, false);

        set_obj_enabled(g_btn_reset, false);
        set_obj_enabled(g_btn_start, false);
        set_widget_editable(g_batch_ta, false, false);
        set_widget_editable(g_interval_ta, false, false);

        set_widget_editable(g_sw_layer, false, true);
        if(lv_obj_has_state(g_sw_layer, LV_STATE_CHECKED)) {
            lv_obj_remove_state(g_sw_layer, LV_STATE_CHECKED);
            ctrl_set_enable_layer_change(0);
        }

        set_widget_editable(g_sw_cal, false, true);
        if(lv_obj_has_state(g_sw_cal, LV_STATE_CHECKED)) {
            lv_obj_remove_state(g_sw_cal, LV_STATE_CHECKED);
            ctrl_set_enable_cal_apply(0);
        }

        static lv_obj_t *const gain_widgets[] = {
            g_ta_img_scale, g_ta_img_range, g_ta_speed_gain,
        };
        set_widgets_editable(gain_widgets, 3, false, true);
        sync_jog_interactive(false);
        sync_center_offset_panel(false, center_off_x, center_off_y);
    } else {
        /* 已连 TCP：锁定 IP/Port，其余解锁（Layer 仍须串口已连） */
        set_widget_editable(g_ip_ta, false, false);
        set_widget_editable(g_port_ta, false, false);
        set_widget_editable(g_serial_dev_dd, true, false);
        set_widget_editable(g_serial_baud_dd, true, false);
        set_obj_enabled(g_btn_tcp, true);
        set_obj_enabled(g_btn_serial, true);

        const bool gcode_ready = (file_name[0] != '\0') && job_gcode_ready();
        const bool can_start_pause = !start_pending && (is_sending || gcode_ready);

        set_obj_enabled(g_btn_reset, true);
        set_obj_enabled(g_btn_start, can_start_pause);
        sync_start_button_label(is_sending, is_paused, start_pending);

        static lv_obj_t *const job_cfg_widgets[] = { g_batch_ta, g_interval_ta };
        set_widgets_editable(job_cfg_widgets, 2, !is_sending, false);

        sync_layer_switch_interactive(serial_connected, is_sending);
        sync_cal_switch_interactive(tcp_connected, cal_loaded, cal_has_gains, is_sending);

        static lv_obj_t *const gain_widgets[] = {
            g_ta_img_scale, g_ta_img_range, g_ta_speed_gain,
        };
        set_widgets_editable(gain_widgets, 3, !is_sending && !cal_enabled, true);
        sync_jog_interactive(offset_idle);
    }
}

void page_control_refresh(void)
{
    page_control_refresh_impl();
}

lv_obj_t *page_control_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, LV_SYMBOL_PLAY "  Control");
    lv_obj_add_style(title, ui_style_title(), 0);

    lv_obj_t *scroll = lv_obj_create(page);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_width(scroll, LV_PCT(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_min_height(scroll, 0, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scroll, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(scroll, UI_PAD, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *content = scroll;

    lv_obj_t *conn_card = ui_make_card(content, "Connection");

    lv_obj_t *tcp_row = make_control_flex_row(conn_card, UI_PAD_SM);
    g_ip_ta = make_labeled_ta(tcp_row, "IP:", "192.168.1.30",
                              UI_FORM_LABEL_W, UI_TA_IP_W, LV_KEYBOARD_MODE_TEXT_LOWER,
                              LV_TEXT_ALIGN_RIGHT);
    g_port_ta = make_labeled_ta(tcp_row, "Port:", "1502",
                                UI_FORM_LABEL2_W, UI_TA_SHORT_W, LV_KEYBOARD_MODE_NUMBER,
                                LV_TEXT_ALIGN_RIGHT);
    g_btn_tcp = make_conn_button(tcp_row, "Connect", on_tcp_click);
    g_lbl_tcp_st = make_conn_status_label(tcp_row, "Disconnected");

    lv_obj_t *ser_row = make_control_flex_row(conn_card, UI_PAD_SM);
    make_form_label(ser_row, "Serial:", UI_FORM_LABEL_W, LV_TEXT_ALIGN_RIGHT);
    g_serial_dev_dd = lv_dropdown_create(ser_row);
    lv_obj_set_width(g_serial_dev_dd, UI_TA_IP_W);
    style_dropdown(g_serial_dev_dd);
    scan_serial_ports();

    make_form_label(ser_row, "Baud:", UI_FORM_LABEL2_W, LV_TEXT_ALIGN_RIGHT);
    g_serial_baud_dd = lv_dropdown_create(ser_row);
    lv_dropdown_set_options(g_serial_baud_dd,
        "9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600");
    lv_dropdown_set_selected(g_serial_baud_dd, 4);
    lv_obj_set_width(g_serial_baud_dd, UI_TA_SHORT_W);
    style_dropdown(g_serial_baud_dd);

    g_btn_serial = make_conn_button(ser_row, "Connect", on_serial_click);
    g_lbl_serial_st = make_conn_status_label(ser_row, "Closed");

    char gain_init[3][16];
    load_gain_init_strings(gain_init);

    lv_obj_t *parse_card = ui_make_card(content, "G-code Parse");

    lv_obj_t *gain_row = make_control_flex_row(parse_card, UI_PAD_SM);
    g_ta_img_scale = make_labeled_ta(gain_row, "Scale:", gain_init[0],
                                     UI_GAIN_ROW_LABEL_W, UI_GAIN_ROW_TA_W,
                                     LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_LEFT);
    g_ta_img_range = make_labeled_ta(gain_row, "Range:", gain_init[1],
                                     UI_GAIN_ROW_LABEL_W, UI_GAIN_ROW_TA_W,
                                     LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_LEFT);
    g_ta_speed_gain = make_labeled_ta(gain_row, "Speed:", gain_init[2],
                                      UI_GAIN_ROW_LABEL_W, UI_GAIN_ROW_TA_W,
                                      LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_LEFT);

    make_center_jog_panel(parse_card);

    make_parse_cal_enable_row(parse_card);
    lv_obj_add_state(g_sw_cal, LV_STATE_DISABLED);
    ctrl_set_enable_cal_apply(0);

    for(lv_obj_t *ta : {g_ta_img_scale, g_ta_img_range, g_ta_speed_gain}) {
        attach_parse_gain_handlers(ta);
    }
    sync_parse_gains_from_ui();

    lv_obj_t *job_card = ui_make_card(content, "Job Control");
    lv_obj_set_height(job_card, LV_SIZE_CONTENT);

    g_lbl_file = lv_label_create(job_card);
    lv_label_set_text(g_lbl_file, "File: --");
    lv_obj_set_style_text_color(g_lbl_file, ui_color_text, 0);
    lv_obj_set_width(g_lbl_file, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_file, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *ctrl_row = make_control_flex_row(job_card, UI_PAD_SM);
    g_batch_ta = make_labeled_ta(ctrl_row, "Batch:", "100",
                                 UI_FORM_LABEL_W, UI_TA_SHORT_W, LV_KEYBOARD_MODE_NUMBER,
                                 LV_TEXT_ALIGN_RIGHT);
    g_interval_ta = make_labeled_ta(ctrl_row, "Intv:", "3",
                                    UI_FORM_LABEL2_W, UI_TA_SHORT_W,
                                    LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_RIGHT);
    g_btn_start = make_job_button(ctrl_row, "Start", on_start_click);
    g_btn_reset = make_job_button(ctrl_row, "Reset", on_reset_click);

    make_labeled_switch_row(job_card, "Layer", UI_SWITCH_LABEL_W, &g_sw_layer, on_layer_sw);
    lv_obj_add_state(g_sw_layer, LV_STATE_DISABLED);
    ctrl_set_enable_layer_change(0);

    lv_obj_add_event_cb(g_batch_ta, on_batch_ta_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(g_interval_ta, on_interval_ta_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    ctrl_backend_init();
    ctrl_set_ring_buffer(&g_ring_buffer);
    ctrl_set_batch_size(100);
    ctrl_set_interval_ms(3);

    return page;
}
