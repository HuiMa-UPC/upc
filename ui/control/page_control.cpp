#include "page_control.hpp"
#include "ctrl_backend.hpp"
#include "job_status.hpp"
#include "gcode_parse_gains.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <glob.h>

/* ---------- 控件 ---------- */
static lv_obj_t *g_ip_ta      = nullptr;
static lv_obj_t *g_port_ta    = nullptr;
static lv_obj_t *g_btn_tcp    = nullptr;
static lv_obj_t *g_lbl_tcp_st = nullptr;

static lv_obj_t *g_serial_dev_dd  = nullptr;
static lv_obj_t *g_serial_baud_dd = nullptr;

static const int k_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
static lv_obj_t *g_btn_serial     = nullptr;
static lv_obj_t *g_lbl_serial_st  = nullptr;

static lv_obj_t *g_lbl_file    = nullptr;
static lv_obj_t *g_batch_ta    = nullptr;
static lv_obj_t *g_interval_ta = nullptr;
static lv_obj_t *g_btn_start   = nullptr;
static lv_obj_t *g_btn_pause   = nullptr;
static lv_obj_t *g_btn_stop    = nullptr;
static lv_obj_t *g_btn_reset   = nullptr;
static lv_obj_t *g_sw_layer    = nullptr;

static lv_obj_t *g_lbl_mcu_st = nullptr;

static lv_obj_t *g_ta_img_scale  = nullptr;
static lv_obj_t *g_ta_img_range   = nullptr;
static lv_obj_t *g_ta_speed_gain  = nullptr;

/* ---------- 辅助：创建卡片容器 ---------- */
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

/* ---------- 事件回调 ---------- */
static void update_tcp_btn(bool connected)
{
    if (connected) {
        lv_obj_set_style_bg_color(g_btn_tcp, ui_color_danger, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_tcp, 0), "Disconnect");
        lv_label_set_text(g_lbl_tcp_st, "Connected");
    } else {
        lv_obj_set_style_bg_color(g_btn_tcp, ui_color_accent_dim, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_tcp, 0), "Connect");
        lv_label_set_text(g_lbl_tcp_st, "Disconnected");
    }
}

static void on_tcp_click(lv_event_t *)
{
    if (ctrl_tcp_is_connected()) {
        ctrl_tcp_disconnect();
        update_tcp_btn(false);
    } else {
        const char *ip = lv_textarea_get_text(g_ip_ta);
        const char *port_str = lv_textarea_get_text(g_port_ta);
        int port = atoi(port_str);
        if (port <= 0) port = 1502;
        if (ctrl_tcp_connect(ip, port) == 0) {
            update_tcp_btn(true);
        } else {
            lv_label_set_text(g_lbl_tcp_st, "Connect failed");
        }
    }
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

static void scan_serial_ports(void)
{
    const char *patterns[] = {
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
        "/dev/ttyS*",
    };
    char options[512] = {0};

    for (size_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
        glob_t globbuf;
        if (glob(patterns[i], 0, NULL, &globbuf) == 0) {
            for (size_t j = 0; j < globbuf.gl_pathc; j++) {
                if (options[0]) strncat(options, "\n", sizeof(options) - strlen(options) - 1);
                strncat(options, globbuf.gl_pathv[j], sizeof(options) - strlen(options) - 1);
            }
            globfree(&globbuf);
        }
    }

    if (options[0]) {
        lv_dropdown_set_options(g_serial_dev_dd, options);
    } else {
        lv_dropdown_set_options(g_serial_dev_dd, "/dev/ttyUSB0");
    }
}

static void on_serial_click(lv_event_t *)
{
    if (ctrl_serial_is_open()) {
        ctrl_serial_close();
        lv_obj_set_style_bg_color(g_btn_serial, ui_color_accent_dim, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_serial, 0), "Connect");
        lv_label_set_text(g_lbl_serial_st, "Closed");
    } else {
        char dev[64] = {0};
        lv_dropdown_get_selected_str(g_serial_dev_dd, dev, sizeof(dev));
        uint16_t sel = lv_dropdown_get_selected(g_serial_baud_dd);
        int baud = (sel < 8) ? k_bauds[sel] : 115200;
        if (ctrl_serial_open(dev, baud) == 0) {
            lv_obj_set_style_bg_color(g_btn_serial, ui_color_danger, 0);
            lv_label_set_text(lv_obj_get_child(g_btn_serial, 0), "Disconnect");
            lv_label_set_text(g_lbl_serial_st, "Open");
        } else {
            lv_label_set_text(g_lbl_serial_st, "Open failed");
        }
    }
}

static bool job_gcode_ready(void)
{
    job_status_update_ring();

    bool ready = false;
    JOB_LOCK();
    if(g_job_status.current_file[0] == '\0') {
        return false;
    }
    /* 解析未完成：至少一批包入队即可开跑（与 Qt 边解析边发送一致） */
    if(g_job_status.is_parsing) {
        int batch = g_job_status.batch_size;
        if(batch <= 0) {
            batch = 100;
        }
        ready = (g_job_status.total_packets >= batch);
    } else {
        /* Reset 清空 Ring 后须等 Files 重新 Open，或解析线程仍在补队 */
        ready = (g_job_status.total_packets > 0 && g_job_status.ring_used > 0);
    }
    return ready;
}

static bool job_can_start(bool *out_layer_need_serial)
{
    if(!ctrl_tcp_is_connected() || !job_gcode_ready()) {
        return false;
    }
    const bool layer_en = lv_obj_has_state(g_sw_layer, LV_STATE_CHECKED);
    if(out_layer_need_serial) {
        *out_layer_need_serial = layer_en;
    }
    if(layer_en && !ctrl_serial_is_open()) {
        return false;
    }
    return true;
}

static void set_job_btn_enabled(lv_obj_t *btn, bool enabled)
{
    if(!btn) {
        return;
    }
    if(enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

static void sync_start_button_label(bool is_sending, bool is_paused)
{
    lv_obj_t *lbl = lv_obj_get_child(g_btn_start, 0);
    if(!lbl) {
        return;
    }
    if(!is_sending) {
        lv_label_set_text(lbl, "Start");
    } else if(is_paused) {
        lv_label_set_text(lbl, "Paused");
    } else {
        lv_label_set_text(lbl, "Running");
    }
}

static void on_start_click(lv_event_t *)
{
    if(!job_can_start(nullptr)) {
        return;
    }
    if(ctrl_is_sending()) {
        return;
    }
    ctrl_start_sending();
}

static void on_pause_click(lv_event_t *)
{
    if(!ctrl_is_sending() || !job_gcode_ready()) {
        return;
    }
    if(ctrl_is_paused()) {
        ctrl_resume_sending();
    } else {
        ctrl_pause_sending();
    }
}

static void on_stop_click(lv_event_t *)
{
    if(!ctrl_is_sending()) {
        return;
    }
    ctrl_stop_sending();
}

static void on_reset_click(lv_event_t *)
{
    ctrl_send_reset();
    lv_label_set_text(lv_obj_get_child(g_btn_start, 0), "Start");
}

static void on_layer_sw(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    ctrl_set_enable_layer_change(lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

static void status_refresh_cb(lv_timer_t *)
{
    /* 先缓存数据并释放锁，避免在持有锁期间长时间调用 LVGL API */
    char file_name[256];
    bool enable_layer, tcp_connected;
    bool is_sending, is_paused, mcu_cmd_tx_hold;
    uint16_t cmd_rate, data_rate;
    uint16_t cur_x, cur_y, cur_z, laser;

    {
        JOB_LOCK();
        strncpy(file_name, g_job_status.current_file, sizeof(file_name) - 1);
        file_name[sizeof(file_name) - 1] = '\0';
        enable_layer = g_job_status.enable_layer_change;
        tcp_connected = g_job_status.tcp_connected;
        is_sending = g_job_status.is_sending;
        cmd_rate  = g_job_status.cmd_cache_rate;
        data_rate = g_job_status.data_cache_rate;
        cur_x = g_job_status.current_x;
        cur_y = g_job_status.current_y;
        cur_z = g_job_status.current_z;
        laser = g_job_status.laser_state;
        mcu_cmd_tx_hold = g_job_status.mcu_cmd_tx_hold;
    }
    is_paused = ctrl_is_paused() != 0;

    if(file_name[0]) {
        lv_label_set_text_fmt(g_lbl_file, "File: %s", file_name);
    } else {
        lv_label_set_text(g_lbl_file, "File: --");
    }

    const bool gcode_ready = (file_name[0] != '\0' && job_gcode_ready());
    bool can_start = false;
    if(tcp_connected && gcode_ready && !is_sending) {
        can_start = !enable_layer || ctrl_serial_is_open();
    }
    const bool can_pause_stop = tcp_connected && gcode_ready && is_sending;

    set_job_btn_enabled(g_btn_start, can_start);
    set_job_btn_enabled(g_btn_pause, can_pause_stop);
    set_job_btn_enabled(g_btn_stop, can_pause_stop);
    sync_start_button_label(is_sending, is_paused);

    int cmd_pct  = percent_rounded(cmd_rate,  65535);
    int data_pct = percent_rounded(data_rate, 65535);
    const char *laser_txt = "--";
    if(tcp_connected) {
        laser_txt = (laser != 0) ? "ON" : "OFF";
    }
    if(mcu_cmd_tx_hold && is_sending) {
        lv_label_set_text_fmt(g_lbl_mcu_st,
            "Cmd %d%% TX hold  Data %d%%  X:%u Y:%u Z:%u  Laser:%s",
            cmd_pct, data_pct, cur_x, cur_y, cur_z, laser_txt);
    } else {
        lv_label_set_text_fmt(g_lbl_mcu_st,
            "Cmd %d%%  Data %d%%  X:%u Y:%u Z:%u  Laser:%s",
            cmd_pct, data_pct, cur_x, cur_y, cur_z, laser_txt);
    }
}

static void style_textarea(lv_obj_t *ta)
{
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

static lv_obj_t *make_form_label(lv_obj_t *parent, const char *text, int width,
                                 lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    return lbl;
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
    lv_obj_set_width(ta, ta_w);
    style_textarea(ta);
    ui_keyboard_attach(ta, kb_mode);
    return ta;
}

static lv_obj_t *make_gain_form_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD, 0);
    return row;
}

static float parse_gain_text(const char *text, float default_val)
{
    if(!text || text[0] == '\0') {
        return default_val;
    }
    char *end = nullptr;
    const float v = strtof(text, &end);
    if(end == text) {
        return default_val;
    }
    return v;
}

static void format_gain_ta(lv_obj_t *ta, float value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", value);
    lv_textarea_set_text(ta, buf);
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
        float def = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
        if(ta == g_ta_img_range) {
            def = GCODE_DEFAULT_IMAGE_RANG_GAIN;
        } else if(ta == g_ta_speed_gain) {
            def = GCODE_DEFAULT_SPEED_GAIN;
        }
        format_gain_ta(ta, parse_gain_text(lv_textarea_get_text(ta), def));
    }

    if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY ||
       code == LV_EVENT_VALUE_CHANGED) {
        sync_parse_gains_from_ui();
    }
}

static lv_obj_t *make_conn_button(lv_obj_t *parent, const char *text,
                                  lv_event_cb_t cb)
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

static void style_start_button(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, ui_color_panel_alt, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if(lbl) {
        lv_obj_set_style_text_color(lbl, ui_color_text_dim, LV_STATE_DISABLED);
    }
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

    /* 标题 */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, LV_SYMBOL_PLAY "  Control");
    lv_obj_add_style(title, ui_style_title(), 0);

    /* === 连接卡片 === */
    lv_obj_t *conn_card = make_card(page, "Connection");

    auto setup_conn_row = [](lv_obj_t *card) -> lv_obj_t * {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);
        return row;
    };

    /* TCP 行：标签列对齐 */
    lv_obj_t *tcp_row = setup_conn_row(conn_card);
    g_ip_ta = make_labeled_ta(tcp_row, "IP:", "192.168.1.30",
                              UI_FORM_LABEL_W, UI_TA_IP_W, LV_KEYBOARD_MODE_TEXT_LOWER,
                              LV_TEXT_ALIGN_RIGHT);
    g_port_ta = make_labeled_ta(tcp_row, "Port:", "1502",
                                UI_FORM_LABEL2_W, UI_TA_SHORT_W, LV_KEYBOARD_MODE_NUMBER,
                                LV_TEXT_ALIGN_RIGHT);
    g_btn_tcp = make_conn_button(tcp_row, "Connect", on_tcp_click);
    g_lbl_tcp_st = lv_label_create(tcp_row);
    lv_label_set_text(g_lbl_tcp_st, "Disconnected");
    lv_obj_set_style_text_color(g_lbl_tcp_st, ui_color_text_dim, 0);
    lv_obj_set_flex_grow(g_lbl_tcp_st, 1);
    lv_label_set_long_mode(g_lbl_tcp_st, LV_LABEL_LONG_MODE_CLIP);

    /* 串口行：与 TCP 同列宽 */
    lv_obj_t *ser_row = setup_conn_row(conn_card);
    make_form_label(ser_row, "Serial:", UI_FORM_LABEL_W, LV_TEXT_ALIGN_RIGHT);

    g_serial_dev_dd = lv_dropdown_create(ser_row);
    lv_obj_set_width(g_serial_dev_dd, UI_TA_IP_W);
    style_dropdown(g_serial_dev_dd);
    scan_serial_ports();

    make_form_label(ser_row, "Baud:", UI_FORM_LABEL2_W, LV_TEXT_ALIGN_RIGHT);

    g_serial_baud_dd = lv_dropdown_create(ser_row);
    lv_dropdown_set_options(g_serial_baud_dd, "9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600");
    lv_dropdown_set_selected(g_serial_baud_dd, 4);
    lv_obj_set_width(g_serial_baud_dd, UI_TA_SHORT_W);
    style_dropdown(g_serial_baud_dd);

    g_btn_serial = make_conn_button(ser_row, "Connect", on_serial_click);
    g_lbl_serial_st = lv_label_create(ser_row);
    lv_label_set_text(g_lbl_serial_st, "Closed");
    lv_obj_set_style_text_color(g_lbl_serial_st, ui_color_text_dim, 0);
    lv_obj_set_flex_grow(g_lbl_serial_st, 1);
    lv_label_set_long_mode(g_lbl_serial_st, LV_LABEL_LONG_MODE_CLIP);

    /* === G-code 解析增益（Files 重新 Open 时生效） === */
    float init_scale = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
    float init_range = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    float init_speed = GCODE_DEFAULT_SPEED_GAIN;
    {
        JOB_LOCK();
        if(g_job_status.parse_image_scale_gain > 1e-6f) {
            init_scale = g_job_status.parse_image_scale_gain;
        }
        if(g_job_status.parse_image_rang_gain > 1e-6f) {
            init_range = g_job_status.parse_image_rang_gain;
        }
        if(g_job_status.parse_speed_gain > 1e-6f) {
            init_speed = g_job_status.parse_speed_gain;
        }
    }
    char gain_init[3][16];
    snprintf(gain_init[0], sizeof(gain_init[0]), "%.3f", init_scale);
    snprintf(gain_init[1], sizeof(gain_init[1]), "%.3f", init_range);
    snprintf(gain_init[2], sizeof(gain_init[2]), "%.3f", init_speed);

    lv_obj_t *parse_card = make_card(page, "G-code Parse");

    lv_obj_t *gain_row1 = make_gain_form_row(parse_card);
    g_ta_img_scale = make_labeled_ta(gain_row1, "Scale:", gain_init[0],
                                     UI_GAIN_LABEL_W, UI_GAIN_TA_W,
                                     LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_LEFT);
    g_ta_img_range = make_labeled_ta(gain_row1, "Range:", gain_init[1],
                                     UI_GAIN_LABEL_W, UI_GAIN_TA_W,
                                     LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *gain_row2 = make_gain_form_row(parse_card);
    g_ta_speed_gain = make_labeled_ta(gain_row2, "Speed:", gain_init[2],
                                      UI_GAIN_LABEL_W, UI_GAIN_TA_W,
                                      LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_LEFT);

    for(lv_obj_t *ta : {g_ta_img_scale, g_ta_img_range, g_ta_speed_gain}) {
        lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_READY, NULL);
    }
    sync_parse_gains_from_ui();

    lv_obj_t *gain_hint = lv_label_create(parse_card);
    lv_label_set_text(gain_hint,
                      "Adjust gains here; re-open the G-code file in Files to re-parse.");
    lv_obj_set_style_text_color(gain_hint, ui_color_text_dim, 0);
    lv_obj_set_width(gain_hint, LV_PCT(100));
    lv_label_set_long_mode(gain_hint, LV_LABEL_LONG_MODE_WRAP);

    /* === 作业控制卡片 === */
    lv_obj_t *job_card = make_card(page, "Job Control");
    lv_obj_set_flex_grow(job_card, 1);

    g_lbl_file = lv_label_create(job_card);
    lv_label_set_text(g_lbl_file, "File: --");
    lv_obj_set_style_text_color(g_lbl_file, ui_color_text, 0);
    lv_obj_set_width(g_lbl_file, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_file, LV_LABEL_LONG_MODE_DOTS);

    /* 控制行 */
    lv_obj_t *ctrl_row = lv_obj_create(job_card);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_width(ctrl_row, LV_PCT(100));
    lv_obj_set_height(ctrl_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, UI_PAD_SM, 0);

    g_batch_ta = make_labeled_ta(ctrl_row, "Batch:", "100",
                                 UI_FORM_LABEL_W, UI_TA_SHORT_W, LV_KEYBOARD_MODE_NUMBER,
                                 LV_TEXT_ALIGN_RIGHT);
    g_interval_ta = make_labeled_ta(ctrl_row, "Intv:", "3",
                                    UI_FORM_LABEL2_W + 12, UI_TA_SHORT_W,
                                    LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_RIGHT);

    g_btn_start = lv_button_create(ctrl_row);
    lv_obj_set_size(g_btn_start, 120, UI_INPUT_H);
    lv_obj_add_style(g_btn_start, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_start, on_start_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_start = lv_label_create(g_btn_start);
    lv_label_set_text(lbl_start, "Start");
    lv_obj_center(lbl_start);
    lv_obj_add_state(g_btn_start, LV_STATE_DISABLED);
    style_start_button(g_btn_start);

    g_btn_pause = lv_button_create(ctrl_row);
    lv_obj_set_size(g_btn_pause, 108, UI_INPUT_H);
    lv_obj_add_style(g_btn_pause, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_pause, on_pause_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_pause = lv_label_create(g_btn_pause);
    lv_label_set_text(lbl_pause, "Pause");
    lv_obj_center(lbl_pause);
    style_start_button(g_btn_pause);
    lv_obj_add_state(g_btn_pause, LV_STATE_DISABLED);

    g_btn_stop = lv_button_create(ctrl_row);
    lv_obj_set_size(g_btn_stop, 108, UI_INPUT_H);
    lv_obj_add_style(g_btn_stop, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_stop, on_stop_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_stop = lv_label_create(g_btn_stop);
    lv_label_set_text(lbl_stop, "Stop");
    lv_obj_center(lbl_stop);
    style_start_button(g_btn_stop);
    lv_obj_add_state(g_btn_stop, LV_STATE_DISABLED);

    g_btn_reset = lv_button_create(ctrl_row);
    lv_obj_set_size(g_btn_reset, 108, UI_INPUT_H);
    lv_obj_add_style(g_btn_reset, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_reset, on_reset_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(g_btn_reset);
    lv_label_set_text(lbl_reset, "Reset");
    lv_obj_center(lbl_reset);

    /* 换层开关 */
    lv_obj_t *layer_row = lv_obj_create(job_card);
    lv_obj_remove_style_all(layer_row);
    lv_obj_set_width(layer_row, LV_PCT(100));
    lv_obj_set_height(layer_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(layer_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layer_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(layer_row, UI_PAD_SM, 0);

    lv_obj_t *lbl_layer = lv_label_create(layer_row);
    lv_label_set_text(lbl_layer, "Enable Layer Change:");
    lv_obj_set_style_text_color(lbl_layer, ui_color_text_dim, 0);

    g_sw_layer = lv_switch_create(layer_row);
    lv_obj_add_event_cb(g_sw_layer, on_layer_sw, LV_EVENT_VALUE_CHANGED, NULL);

    /* 状态栏 */
    g_lbl_mcu_st = lv_label_create(page);
    lv_label_set_text(g_lbl_mcu_st, "Cache: --");
    lv_obj_set_style_text_color(g_lbl_mcu_st, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_mcu_st, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_mcu_st, LV_LABEL_LONG_MODE_CLIP);

    /* 绑定数值变化 */
    lv_obj_add_event_cb(g_batch_ta, [](lv_event_t *) {
        ctrl_set_batch_size(atoi(lv_textarea_get_text(g_batch_ta)));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(g_interval_ta, [](lv_event_t *) {
        ctrl_set_interval_ms(atoi(lv_textarea_get_text(g_interval_ta)));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    /* 定时刷新 */
    lv_timer_create(status_refresh_cb, 200, nullptr);

    /* 初始化后端 */
    ctrl_backend_init();
    ctrl_set_ring_buffer(&g_ring_buffer);
    ctrl_set_batch_size(100);
    ctrl_set_interval_ms(3);

    return page;
}
