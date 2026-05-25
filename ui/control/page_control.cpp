#include "page_control.hpp"
#include "ctrl_backend.hpp"
#include "job_status.hpp"
#include "gcode_parse.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "page_common.hpp"

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
static lv_obj_t *g_btn_reset   = nullptr;
static lv_obj_t *g_sw_layer    = nullptr;
static lv_obj_t *g_sw_cal      = nullptr;

static lv_obj_t *g_lbl_mcu_st = nullptr;

static lv_obj_t *g_ta_img_scale  = nullptr;
static lv_obj_t *g_ta_img_range   = nullptr;
static lv_obj_t *g_ta_speed_gain  = nullptr;

/* ---------- 事件回调 ---------- */
/** 根据 TCP 连接状态更新按钮颜色和文案 */
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

/** TCP Connect/Disconnect 按钮点击事件 */
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

/** 统一设置串口下拉框样式 */
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

/** 扫描 /dev/ttyUSB* /dev/ttyACM* /dev/ttyS*，填充串口设备下拉框 */
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

/** Serial Connect/Disconnect 按钮点击事件 */
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

/** 设置按钮的启用/禁用状态（安全判空） */
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

/** Start 可用：Ring 满一批，或小文件已全部解析入队，或打印完成后可再次 Start */
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

/** 发送作业流期间锁定 Batch / Intv（增益与换层/校准由专用 sync 处理） */
static void set_job_config_interactive(bool enabled)
{
    lv_obj_t *widgets[] = { g_batch_ta, g_interval_ta };

    for(lv_obj_t *obj : widgets) {
        if(!obj) {
            continue;
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

    if(!enabled) {
        ui_keyboard_hide();
    }
}

/** 根据发送/暂停/预填状态同步 Start 按钮文案 */
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

/** Start/Pause 按钮点击事件 */
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
    if(ctrl_is_start_pending()) {
        return;
    }
    if(!job_gcode_ready()) {
        return;
    }
    ctrl_start_sending();
}

/** Reset 按钮点击事件 */
static void on_reset_click(lv_event_t *)
{
    ctrl_send_reset();
}

/** 换层开关状态变化事件（须串口已连接） */
static void on_layer_sw(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    if(!ctrl_serial_is_open()) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ctrl_set_enable_layer_change(0);
        return;
    }
    ctrl_set_enable_layer_change(lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

/** 串口未连接时禁止勾选换层；发送中锁定 */
static void sync_layer_switch_interactive(bool serial_connected, bool is_sending)
{
    if(!g_sw_layer) {
        return;
    }
    if(is_sending) {
        lv_obj_add_state(g_sw_layer, LV_STATE_DISABLED);
        lv_obj_clear_flag(g_sw_layer, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    if(serial_connected) {
        lv_obj_remove_state(g_sw_layer, LV_STATE_DISABLED);
        lv_obj_add_flag(g_sw_layer, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    lv_obj_add_state(g_sw_layer, LV_STATE_DISABLED);
    lv_obj_clear_flag(g_sw_layer, LV_OBJ_FLAG_CLICKABLE);
    if(lv_obj_has_state(g_sw_layer, LV_STATE_CHECKED)) {
        lv_obj_remove_state(g_sw_layer, LV_STATE_CHECKED);
        ctrl_set_enable_layer_change(0);
    }
}

/** TCP 未连或未 Open CAL 文件（或无 CAL 增益行）时禁止校准使能；发送中锁定 */
static void sync_cal_switch_interactive(bool tcp_connected, bool cal_loaded,
                                        bool cal_has_gains, bool is_sending)
{
    if(!g_sw_cal) {
        return;
    }
    const bool can_use = tcp_connected && cal_loaded && cal_has_gains && !is_sending;
    if(!can_use) {
        lv_obj_add_state(g_sw_cal, LV_STATE_DISABLED);
        lv_obj_clear_flag(g_sw_cal, LV_OBJ_FLAG_CLICKABLE);
        if(!tcp_connected || !cal_loaded || !cal_has_gains) {
            if(lv_obj_has_state(g_sw_cal, LV_STATE_CHECKED)) {
                lv_obj_remove_state(g_sw_cal, LV_STATE_CHECKED);
            }
            ctrl_set_enable_cal_apply(0);
        }
        return;
    }
    lv_obj_remove_state(g_sw_cal, LV_STATE_DISABLED);
    lv_obj_add_flag(g_sw_cal, LV_OBJ_FLAG_CLICKABLE);
    if(ctrl_get_enable_cal_apply()) {
        lv_obj_add_state(g_sw_cal, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(g_sw_cal, LV_STATE_CHECKED);
    }
}

/** 校准使能时锁定 Parse 增益输入（改由 CAL 文件提供） */
static void sync_parse_gain_interactive(bool cal_enabled, bool is_sending)
{
    const bool editable = !is_sending && !cal_enabled;
    lv_obj_t *widgets[] = { g_ta_img_scale, g_ta_img_range, g_ta_speed_gain };
    for(lv_obj_t *obj : widgets) {
        if(!obj) {
            continue;
        }
        if(editable) {
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
    if(!editable) {
        ui_keyboard_hide();
    }
}

static void on_cal_sw(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    if(!ctrl_tcp_is_connected()) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ctrl_set_enable_cal_apply(0);
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
        return;
    }
    ctrl_set_enable_cal_apply(lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

/**
 * @brief UI 状态定时刷新回调（200ms 周期）
 *
 * 职责：
 * 1. 缓存 job_status 数据并释放锁
 * 2. 更新文件名称、MCU 状态栏
 * 3. 控制 Start/Reset 按钮可用性
 * 4. 同步 Start 按钮文案（Start / Pause）
 * 5. 锁定/解锁增益配置区
 */
static void status_refresh_cb(lv_timer_t *)
{
    /* 先缓存数据并释放锁，避免在持有锁期间长时间调用 LVGL API */
    char file_name[256];
    bool tcp_connected;
    bool is_sending, is_paused, mcu_cmd_tx_hold;
    bool cal_loaded, cal_enabled, cal_has_gains;
    uint16_t cmd_rate, data_rate;
    uint16_t cur_x, cur_y, cur_z, laser;

    {
        JOB_LOCK();
        strncpy(file_name, g_job_status.current_file, sizeof(file_name) - 1);
        file_name[sizeof(file_name) - 1] = '\0';
        tcp_connected = g_job_status.tcp_connected;
        cmd_rate  = g_job_status.cmd_cache_rate;
        data_rate = g_job_status.data_cache_rate;
        cur_x = g_job_status.current_x;
        cur_y = g_job_status.current_y;
        cur_z = g_job_status.current_z;
        laser = g_job_status.laser_state;
        mcu_cmd_tx_hold = g_job_status.mcu_cmd_tx_hold;
        cal_loaded = g_job_status.cal_file_loaded;
        cal_enabled = g_job_status.enable_cal_apply;
        cal_has_gains = g_job_status.cal_has_gains;
    }
    is_sending = ctrl_is_sending() != 0;
    is_paused = ctrl_is_paused() != 0;
    const bool start_pending = ctrl_is_start_pending() != 0;
    const bool serial_connected = ctrl_serial_is_open() != 0;

    if(file_name[0]) {
        lv_label_set_text_fmt(g_lbl_file, "File: %s", file_name);
    } else {
        lv_label_set_text(g_lbl_file, "File: --");
    }

    const bool has_file = (file_name[0] != '\0');
    const bool gcode_ready = has_file && job_gcode_ready();

    const bool can_reset = tcp_connected;
    const bool can_start_pause =
        tcp_connected && !start_pending && (is_sending || gcode_ready);

    set_job_btn_enabled(g_btn_reset, can_reset);
    set_job_btn_enabled(g_btn_start, can_start_pause);
    sync_start_button_label(is_sending, is_paused, start_pending);
    set_job_config_interactive(!is_sending);
    sync_layer_switch_interactive(serial_connected, is_sending);
    sync_cal_switch_interactive(tcp_connected, cal_loaded, cal_has_gains, is_sending);
    sync_parse_gain_interactive(cal_enabled, is_sending);

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

/** 创建表单标签（固定宽度、灰色、右对齐/左对齐） */
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

/**
 * @brief 创建带标签的单行文本输入框
 * @return 文本输入框对象
 */
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
    /* 统一样式（原 style_textarea 已内联） */
    lv_obj_set_height(ta, UI_INPUT_H);
    lv_obj_set_style_bg_color(ta, ui_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, ui_color_border, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(ta, UI_PAD_SM, 0);
    lv_obj_set_style_text_font(ta, ui_font_body(), 0);
    lv_obj_set_style_text_color(ta, ui_color_text, 0);
    ui_keyboard_attach(ta, kb_mode);
    return ta;
}

/** 创建增益表单行容器（横向 flex，无背景） */
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

/** 解析增益输入文本，无效时返回默认值 */
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

/** 从 UI 增益输入框读取数值并同步到 job_status */
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

/**
 * @brief 增益输入框事件处理（DEFOCUSED / READY / VALUE_CHANGED）
 *
 * 失去焦点或确认时：格式化显示为 3 位小数
 * 任何变更时：同步到 job_status 供解析线程读取
 */
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
        /* 原 format_gain_ta 已内联：格式化显示为 3 位小数 */
        char fmt_buf[24];
        snprintf(fmt_buf, sizeof(fmt_buf), "%.3f", parse_gain_text(lv_textarea_get_text(ta), def));
        lv_textarea_set_text(ta, fmt_buf);
    }

    if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY ||
       code == LV_EVENT_VALUE_CHANGED) {
        sync_parse_gains_from_ui();
    }
}

/** 创建连接卡片中的 Connect/Disconnect 按钮 */
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

/**
 * @brief 创建 Control 页面
 * @param parent LVGL 父对象
 * @return 页面根对象
 *
 * 页面分区（标题固定，其余在 scroll 内纵向滑动）：
 * 1. Connection 卡片（TCP IP/Port/Connect + Serial Dev/Baud/Connect）
 * 2. G-code Parse 卡片（Scale / Range / Speed 增益输入）
 * 3. Job Control 卡片（Batch / Intv / Start / Reset / Layer Switch）
 * 4. MCU 状态栏
 */
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

    /* 标题固定；下方为可纵向滑动的内容区（类似手机长页滚动） */
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

    /* === 连接卡片 === */
    lv_obj_t *conn_card = ui_make_card(content, "Connection");

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

    lv_obj_t *parse_card = ui_make_card(content, "G-code Parse");

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

    lv_obj_t *cal_row = make_gain_form_row(parse_card);
    make_form_label(cal_row, "Enable Calibration:", UI_SWITCH_LABEL_W, LV_TEXT_ALIGN_LEFT);
    g_sw_cal = lv_switch_create(cal_row);
    ui_theme_apply_switch(g_sw_cal);
    lv_obj_add_event_cb(g_sw_cal, on_cal_sw, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_state(g_sw_cal, LV_STATE_DISABLED);
    ctrl_set_enable_cal_apply(0);

    for(lv_obj_t *ta : {g_ta_img_scale, g_ta_img_range, g_ta_speed_gain}) {
        lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(ta, on_parse_gain_ta_event, LV_EVENT_READY, NULL);
    }
    sync_parse_gains_from_ui();

    lv_obj_t *gain_hint = lv_label_create(parse_card);
    lv_label_set_text(gain_hint,
                      "Change gains or calibration, then re-open G-code in Files.");
    lv_obj_set_style_text_color(gain_hint, ui_color_text_dim, 0);
    lv_obj_set_width(gain_hint, LV_PCT(100));
    lv_label_set_long_mode(gain_hint, LV_LABEL_LONG_MODE_WRAP);

    /* === 作业控制卡片 === */
    lv_obj_t *job_card = ui_make_card(content, "Job Control");
    lv_obj_set_height(job_card, LV_SIZE_CONTENT);

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
                                    UI_FORM_LABEL2_W, UI_TA_SHORT_W,
                                    LV_KEYBOARD_MODE_NUMBER, LV_TEXT_ALIGN_RIGHT);

    g_btn_start = lv_button_create(ctrl_row);
    lv_obj_set_size(g_btn_start, UI_BTN_JOB_W, UI_INPUT_H);
    lv_obj_add_style(g_btn_start, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_start, on_start_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_start = lv_label_create(g_btn_start);
    lv_label_set_text(lbl_start, "Start");
    lv_obj_center(lbl_start);
    lv_obj_add_state(g_btn_start, LV_STATE_DISABLED);
    /* 原 style_start_button 已内联：设置 DISABLED 态样式 */
    lv_obj_set_style_bg_color(g_btn_start, ui_color_panel_alt, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(g_btn_start, LV_OPA_COVER, LV_STATE_DISABLED);
    if(lbl_start) {
        lv_obj_set_style_text_color(lbl_start, ui_color_text_dim, LV_STATE_DISABLED);
    }

    g_btn_reset = lv_button_create(ctrl_row);
    lv_obj_set_size(g_btn_reset, UI_BTN_JOB_W, UI_INPUT_H);
    lv_obj_add_style(g_btn_reset, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_reset, on_reset_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(g_btn_reset);
    lv_label_set_text(lbl_reset, "Reset");
    lv_obj_center(lbl_reset);
    /* 原 style_start_button 已内联：设置 DISABLED 态样式 */
    lv_obj_set_style_bg_color(g_btn_reset, ui_color_panel_alt, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(g_btn_reset, LV_OPA_COVER, LV_STATE_DISABLED);
    if(lbl_reset) {
        lv_obj_set_style_text_color(lbl_reset, ui_color_text_dim, LV_STATE_DISABLED);
    }
    lv_obj_add_state(g_btn_reset, LV_STATE_DISABLED);

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
    lv_obj_set_width(lbl_layer, UI_SWITCH_LABEL_W);
    lv_label_set_long_mode(lbl_layer, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(lbl_layer, ui_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl_layer, ui_font_body(), 0);

    g_sw_layer = lv_switch_create(layer_row);
    ui_theme_apply_switch(g_sw_layer);
    lv_obj_add_event_cb(g_sw_layer, on_layer_sw, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_state(g_sw_layer, LV_STATE_DISABLED);
    ctrl_set_enable_layer_change(0);

    /* 状态栏（随内容一起滚动） */
    g_lbl_mcu_st = lv_label_create(content);
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
