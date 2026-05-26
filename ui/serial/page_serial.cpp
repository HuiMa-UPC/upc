#include "page_serial.hpp"
#include "serial_backend.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

/* ============================================================================
 *  Serial Page  ——  Klipper 风格的三 Tab 调试面板
 *  ----------------------------------------------------------------------------
 *  Tab 1: Motor    — Z/S/D 三轴 Jog、归零、增量移动；右侧解析数据与功能按键
 *  Tab 2: Print    — Z 轴零点、推杆/回粉/激光/洗气/风机、传感器数据；功能按键
 *  Tab 3: Settings — 1#/2# 粉桶启停 + 状态指示
 *  ----------------------------------------------------------------------------
 *  所有用户操作只调用 serial_backend.hpp 提供的接口；UI 周期性查询状态刷新显示。
 * ============================================================================ */

/* ============================ 局部常量 / 尺寸 ============================ */
#define SP_AXIS_LABEL_W     110     /* "Z (Jog):" 列宽 */
#define SP_SPEED_TA_W       100     /* 速度输入框 */
#define SP_DIR_BTN_W        80      /* 方向按钮（三角图标 / CW/CCW 文字） */
#define SP_HOME_BTN_W       80      /* rst 归零按钮 */
#define SP_INC_TA_W         120     /* 增量值输入框 */
#define SP_INC_BTN_W        100     /* 启动按钮 */
#define SP_INC_LABEL_W      100     /* "Use Inc" 标签 */

#define SP_PARSE_CARD_W     280     /* 解析数据卡片 */
#define SP_FUNC_CARD_W      160     /* 功能按键卡片（窄一些给左侧让位） */
#define SP_FUNC_BTN_H       56      /* 功能按键高 */

#define SP_PRINT_BTN_H      52      /* Print Tab 大按钮高 */
#define SP_SENSOR_CELL_H    120     /* Sensor 网格单元高（更高更醒目） */
#define SP_BUCKET_BTN_H     68      /* 粉桶按钮高 */
#define SP_DOT_SIZE         18      /* 状态指示圆点 */

/** 开关橙色（激活态背景） */
#define SP_SWITCH_ON_HEX    0xff8800
/** 开关 / Sensor / 按钮的 user_data 标签语义见 build_func_card / Outputs */

/* ============================ Motor Tab 控件 ============================ */
static lv_obj_t *g_ta_z_speed   = nullptr;
static lv_obj_t *g_ta_s_speed   = nullptr;
static lv_obj_t *g_ta_d_speed   = nullptr;
static lv_obj_t *g_sw_z_inc     = nullptr;
static lv_obj_t *g_sw_s_inc     = nullptr;
static lv_obj_t *g_sw_d_inc     = nullptr;
static lv_obj_t *g_ta_z_inc     = nullptr;
static lv_obj_t *g_ta_s_inc     = nullptr;
static lv_obj_t *g_ta_d_inc     = nullptr;
static lv_obj_t *g_btn_z_inc    = nullptr;
static lv_obj_t *g_btn_s_inc    = nullptr;
static lv_obj_t *g_btn_d_inc    = nullptr;

/* Z/S/D 方向键（press/release jog），便于 Inc 互斥时禁用/启用 */
static lv_obj_t *g_btn_z_pos    = nullptr;  /* ▲ */
static lv_obj_t *g_btn_z_neg    = nullptr;  /* ▼ */
static lv_obj_t *g_btn_s_pos    = nullptr;  /* ◀ */
static lv_obj_t *g_btn_s_neg    = nullptr;  /* ▶ */
static lv_obj_t *g_btn_d_pos    = nullptr;  /* CW */
static lv_obj_t *g_btn_d_neg    = nullptr;  /* CCW */

static lv_obj_t *g_lbl_z_pos    = nullptr;
static lv_obj_t *g_lbl_s_pos    = nullptr;
static lv_obj_t *g_lbl_d_pos    = nullptr;

/* ============================ Print Tab 控件 ============================ */
static lv_obj_t *g_lbl_zwork    = nullptr;
static lv_obj_t *g_lbl_o2       = nullptr;
static lv_obj_t *g_lbl_press    = nullptr;
static lv_obj_t *g_lbl_temp     = nullptr;
static lv_obj_t *g_lbl_hum      = nullptr;
static lv_obj_t *g_lbl_pm25     = nullptr;
static lv_obj_t *g_ta_fan_duty  = nullptr;

/* ============================ Settings Tab 控件 ============================ */
static lv_obj_t *g_dot_bucket1  = nullptr;
static lv_obj_t *g_dot_bucket2  = nullptr;
static lv_obj_t *g_lbl_bucket1  = nullptr;
static lv_obj_t *g_lbl_bucket2  = nullptr;

/* ============================ Print Tab Outputs 开关 =================== */
static lv_obj_t *g_sw_laser     = nullptr;
static lv_obj_t *g_sw_gas       = nullptr;
static lv_obj_t *g_sw_fan       = nullptr;

/* ============================ 功能按键（两份：Motor / Print） =========== */
struct func_btn_set_t {
    lv_obj_t *wake     = nullptr;
    lv_obj_t *sleep    = nullptr;
    lv_obj_t *enable   = nullptr;
    lv_obj_t *disable  = nullptr;
    lv_obj_t *light_sw = nullptr;   /* 滑动开关 */
    lv_obj_t *door_sw  = nullptr;   /* 滑动开关 */
};
static func_btn_set_t g_func_motor;
static func_btn_set_t g_func_print;

/** refresh_cb 同步 switch checked 状态时短路 VALUE_CHANGED 回调，避免循环 */
static bool g_sync_in_progress = false;

/* ============================ 串口状态栏 =============================== */
static lv_obj_t *g_lbl_serial_stat = nullptr;

/* ============================================================================
 *  通用样式工具
 * ============================================================================ */
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

    /* DISABLED：更暗背景 + 暗灰文字（视觉禁用反馈） */
    lv_obj_set_style_bg_color(ta, ui_color_bg, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(ta, ui_color_text_dim, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(ta, ui_color_border, LV_STATE_DISABLED);
}

/** 把任意容器刷成"页面底色"——用于解决 tabview 内部组件默认白底问题 */
static void paint_dark(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, ui_color_bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
}

static void style_card(lv_obj_t *card)
{
    lv_obj_add_style(card, ui_style_panel(), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, UI_PAD, 0);
    lv_obj_set_style_pad_row(card, UI_PAD_SM, 0);
}

static lv_obj_t *make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_card(card);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    /* 卡片本身不滚（高度自适应），避免吞掉父容器的滚动手势 */
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    if(title) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, title);
        lv_obj_add_style(lbl, ui_style_subtitle(), 0);
        lv_obj_set_style_text_color(lbl, ui_color_accent, 0);
    }
    return card;
}

static lv_obj_t *make_row(lv_obj_t *parent, lv_flex_align_t main_align = LV_FLEX_ALIGN_START)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main_align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);
    return row;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int width,
                            const lv_color_t *color = nullptr,
                            lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    if(width > 0) {
        lv_obj_set_width(lbl, width);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    }
    lv_obj_set_style_text_color(lbl, color ? *color : ui_color_text_dim, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    return lbl;
}

static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder,
                               int width, lv_keyboard_mode_t kb)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder ? placeholder : "");
    if(width > 0) lv_obj_set_width(ta, width);
    style_textarea(ta);
    ui_keyboard_attach(ta, kb);
    return ta;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int width, int height,
                             lv_event_cb_t cb, void *user_data = nullptr,
                             const lv_color_t *bg = nullptr)
{
    lv_obj_t *btn = lv_button_create(parent);
    if(width  > 0) lv_obj_set_width(btn, width);
    if(height > 0) lv_obj_set_height(btn, height);
    lv_obj_add_style(btn, ui_style_btn_primary(), 0);
    if(bg) {
        lv_obj_set_style_bg_color(btn, *bg, 0);
    }
    lv_obj_set_style_bg_color(btn, ui_color_accent, LV_STATE_PRESSED);

    /* DISABLED：暗灰 bg，文字也变暗 */
    lv_obj_set_style_bg_color(btn, ui_color_panel_alt, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);

    if(cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, ui_color_text_dim, LV_STATE_DISABLED);
    return btn;
}

/** 设置控件禁用/启用状态（视觉由 LV_STATE_DISABLED 样式自动反馈） */
static void set_obj_disabled(lv_obj_t *obj, bool disabled)
{
    if(!obj) return;
    if(disabled) lv_obj_add_state(obj, LV_STATE_DISABLED);
    else         lv_obj_remove_state(obj, LV_STATE_DISABLED);
}

static void btn_set_color(lv_obj_t *btn, lv_color_t c)
{
    if(!btn) return;
    lv_obj_set_style_bg_color(btn, c, 0);
}

/** 圆形状态指示点 */
static lv_obj_t *make_dot(lv_obj_t *parent)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, SP_DOT_SIZE, SP_DOT_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, ui_color_text_dim, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    return dot;
}

/** 把开关内部 label 的文字/位置同步到当前 checked 状态 */
static void update_toggle_label(lv_obj_t *sw)
{
    if(!sw) return;
    lv_obj_t *lbl = lv_obj_get_child(sw, 0);
    if(!lbl) return;
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_label_set_text(lbl, on ? "ON" : "OFF");
    /* 文字在 knob 对侧，避免被圆球遮住 */
    lv_obj_align(lbl, on ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 on ? 8 : -8, 0);
}

/** 用户操作触发的 VALUE_CHANGED：同步内部 label，再让其他回调继续 */
static void toggle_internal_sync_cb(lv_event_t *e)
{
    update_toggle_label((lv_obj_t *)lv_event_get_target(e));
}

/**
 *  统一风格的滑动开关（64×24 紧凑版，内带 ON/OFF 白色小字）
 *  - 未激活：indicator 灰底（ui_color_panel_alt）+ "OFF" 字
 *  - 激活：indicator 橙底（SP_SWITCH_ON_HEX）+ "ON" 字
 *  - knob：白色圆
 */
static lv_obj_t *make_toggle(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 80, 32);

    /* MAIN：开关边框/背景 */
    lv_obj_set_style_bg_color(sw, ui_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, ui_color_border, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);

    /* INDICATOR：未激活灰，激活橙 */
    lv_obj_set_style_bg_color(sw, ui_color_panel_alt, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(SP_SWITCH_ON_HEX),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* KNOB：白色圆 */
    lv_obj_set_style_bg_color(sw, ui_color_text, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);

    /* 内部 OFF/ON 文字 label（child 0，不可点击，不阻挡触摸） */
    lv_obj_t *lbl = lv_label_create(sw);
    lv_label_set_text(lbl, "OFF");
    lv_obj_set_style_text_color(lbl, ui_color_text, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    update_toggle_label(sw);

    /* 内部同步回调（先于用户 cb 注册 → 先于 user cb 执行 → 文字总是先更新） */
    lv_obj_add_event_cb(sw, toggle_internal_sync_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if(cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return sw;
}

/* ============================================================================
 *  Motor Tab：回调
 * ============================================================================ */
static float read_float(lv_obj_t *ta, float dflt)
{
    if(!ta) return dflt;
    const char *t = lv_textarea_get_text(ta);
    if(!t || !*t) return dflt;
    char *end = nullptr;
    float v = strtof(t, &end);
    return (end == t) ? dflt : v;
}

/** 按下：启动持续运动 */
static void on_jog_press(lv_event_t *e)
{
    uintptr_t code = (uintptr_t)lv_event_get_user_data(e);
    jog_axis_t axis = (jog_axis_t)((code >> 4) & 0x0F);
    jog_dir_t  dir  = (jog_dir_t)(code & 0x0F);

    float speed = 0.0f;
    switch(axis) {
        case JOG_AXIS_Z: speed = read_float(g_ta_z_speed, 100.0f); break;
        case JOG_AXIS_S: speed = read_float(g_ta_s_speed, 100.0f); break;
        case JOG_AXIS_D: speed = read_float(g_ta_d_speed, 100.0f); break;
    }
    serial_motor_jog_start(axis, dir, speed);
}

/** 松开：停止运动（含手指滑出按钮 LV_EVENT_PRESS_LOST 也视为松开） */
static void on_jog_release(lv_event_t *e)
{
    uintptr_t code = (uintptr_t)lv_event_get_user_data(e);
    jog_axis_t axis = (jog_axis_t)((code >> 4) & 0x0F);
    serial_motor_jog_stop(axis);
}

static void on_home_click(lv_event_t *e)
{
    jog_axis_t axis = (jog_axis_t)(uintptr_t)lv_event_get_user_data(e);
    serial_motor_home(axis);
}

static void on_inc_click(lv_event_t *e)
{
    jog_axis_t axis = (jog_axis_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = nullptr, *ta = nullptr;
    switch(axis) {
        case JOG_AXIS_Z: sw = g_sw_z_inc; ta = g_ta_z_inc; break;
        case JOG_AXIS_S: sw = g_sw_s_inc; ta = g_ta_s_inc; break;
        case JOG_AXIS_D: sw = g_sw_d_inc; ta = g_ta_d_inc; break;
    }
    if(!sw || !lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        ui_toast_show("Enable 'Use Inc' first", ui_color_danger, 1500);
        return;
    }
    float dist = read_float(ta, 0.0f);
    jog_dir_t dir = (dist >= 0) ? JOG_DIR_POS : JOG_DIR_NEG;
    if(dist < 0) dist = -dist;
    serial_motor_increment(axis, dir, dist);
}

/** 构造一颗 Jog 方向键：按下→jog_start，松开/失焦→jog_stop
 *  use_icon_font=true 时给 label 套大号 icon 字体，让三角图标更醒目
 */
static lv_obj_t *make_jog_direction_btn(lv_obj_t *parent, const char *label,
                                        jog_axis_t axis, jog_dir_t dir,
                                        bool use_icon_font)
{
    uintptr_t code = ((uintptr_t)axis << 4) | (uintptr_t)dir;
    lv_obj_t *btn = make_button(parent, label, SP_DIR_BTN_W, UI_INPUT_H,
                                /*cb=*/nullptr, (void *)code);

    /* 同一按钮绑定 PRESSED + RELEASED + PRESS_LOST 三个事件
     * PRESS_LOST 处理"按下后手指滑出按钮"的情况，确保电机不会卡在持续运动状态
     */
    lv_obj_add_event_cb(btn, on_jog_press,   LV_EVENT_PRESSED,    (void *)code);
    lv_obj_add_event_cb(btn, on_jog_release, LV_EVENT_RELEASED,   (void *)code);
    lv_obj_add_event_cb(btn, on_jog_release, LV_EVENT_PRESS_LOST, (void *)code);

    if(use_icon_font) {
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if(lbl) lv_obj_set_style_text_font(lbl, ui_font_icon(), 0);
    }
    return btn;
}

/* ---------- 构造一行 Jog ---------- */
static void build_jog_row(lv_obj_t *parent, const char *axis_name,
                          jog_axis_t axis,
                          const char *pos_label, const char *neg_label,
                          bool use_triangle_icons,
                          bool show_home, lv_obj_t **out_speed_ta,
                          lv_obj_t **out_pos_btn, lv_obj_t **out_neg_btn)
{
    lv_obj_t *row = make_row(parent);

    char head[24];
    snprintf(head, sizeof(head), "%s (Jog):", axis_name);
    make_label(row, head, SP_AXIS_LABEL_W, &ui_color_text);

    *out_speed_ta = make_textarea(row, "speed", SP_SPEED_TA_W, LV_KEYBOARD_MODE_NUMBER);

    lv_obj_t *btn_p = make_jog_direction_btn(row, pos_label, axis, JOG_DIR_POS, use_triangle_icons);
    lv_obj_t *btn_n = make_jog_direction_btn(row, neg_label, axis, JOG_DIR_NEG, use_triangle_icons);
    if(out_pos_btn) *out_pos_btn = btn_p;
    if(out_neg_btn) *out_neg_btn = btn_n;

    if(show_home) {
        /* rst 按钮：暗灰底，与方向键拉开层次，避免误触 */
        lv_color_t rst_bg = ui_color_panel_alt;
        make_button(row, "rst", SP_HOME_BTN_W, UI_INPUT_H,
                    on_home_click, (void *)(uintptr_t)axis, &rst_bg);
    } else {
        /* 占位，让多行右侧对齐 */
        lv_obj_t *spacer = lv_obj_create(row);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_size(spacer, SP_HOME_BTN_W, 1);
    }
}

/** Jog ↔ Inc 互斥：Inc 开 → speed/方向键 disabled，inc 输入框/Start enabled；反之亦然 */
static void apply_inc_mutex(jog_axis_t axis, bool inc_on)
{
    lv_obj_t *speed = nullptr, *dir_p = nullptr, *dir_n = nullptr;
    lv_obj_t *inc_ta = nullptr, *inc_btn = nullptr;
    switch(axis) {
        case JOG_AXIS_Z:
            speed = g_ta_z_speed; dir_p = g_btn_z_pos; dir_n = g_btn_z_neg;
            inc_ta = g_ta_z_inc;  inc_btn = g_btn_z_inc; break;
        case JOG_AXIS_S:
            speed = g_ta_s_speed; dir_p = g_btn_s_pos; dir_n = g_btn_s_neg;
            inc_ta = g_ta_s_inc;  inc_btn = g_btn_s_inc; break;
        case JOG_AXIS_D:
            speed = g_ta_d_speed; dir_p = g_btn_d_pos; dir_n = g_btn_d_neg;
            inc_ta = g_ta_d_inc;  inc_btn = g_btn_d_inc; break;
    }
    /* Jog 侧 */
    set_obj_disabled(speed, inc_on);
    set_obj_disabled(dir_p, inc_on);
    set_obj_disabled(dir_n, inc_on);
    /* Inc 侧 */
    set_obj_disabled(inc_ta,  !inc_on);
    set_obj_disabled(inc_btn, !inc_on);
}

static void on_inc_mutex_sw(lv_event_t *e)
{
    if(g_sync_in_progress) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    jog_axis_t axis = (jog_axis_t)(uintptr_t)lv_event_get_user_data(e);
    apply_inc_mutex(axis, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* ---------- 构造一行增量移动（不再带 "X Inc Move:" 头标签） ---------- */
static void build_inc_row(lv_obj_t *parent, const char *axis_name, jog_axis_t axis,
                          lv_obj_t **out_sw, lv_obj_t **out_ta, lv_obj_t **out_btn)
{
    (void)axis_name;  /* 不再需要 */
    lv_obj_t *row = make_row(parent);

    /* 直接以 toggle 开关起手（带 ON/OFF 文字），作为 Jog↔Inc 互斥控制 */
    *out_sw = make_toggle(row, on_inc_mutex_sw, (void *)(uintptr_t)axis);

    make_label(row, "Use Inc", SP_INC_LABEL_W, &ui_color_text_dim);
    *out_ta = make_textarea(row, "value", SP_INC_TA_W, LV_KEYBOARD_MODE_NUMBER);
    *out_btn = make_button(row, "Start", SP_INC_BTN_W, UI_INPUT_H,
                           on_inc_click, (void *)(uintptr_t)axis);

    /* 默认 Inc=OFF：增量输入框 + Start 灰禁；Jog 侧由 apply_inc_mutex 兜底 */
    lv_obj_add_state(*out_ta,  LV_STATE_DISABLED);
    lv_obj_add_state(*out_btn, LV_STATE_DISABLED);
}

/* ============================================================================
 *  解析数据卡片（Motor Tab 右侧）
 * ============================================================================ */
static lv_obj_t *build_parse_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, LV_SYMBOL_LIST "  Parse Data");
    lv_obj_set_width(card, SP_PARSE_CARD_W);
    lv_obj_set_height(card, LV_PCT(100));

    auto add_field = [&](const char *name, lv_obj_t **out_val) {
        lv_obj_t *blk = lv_obj_create(card);
        lv_obj_remove_style_all(blk);
        lv_obj_set_width(blk, LV_PCT(100));
        lv_obj_set_height(blk, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(blk, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(blk, 4, 0);

        lv_obj_t *t = lv_label_create(blk);
        lv_label_set_text(t, name);
        lv_obj_set_style_text_color(t, ui_color_text_dim, 0);

        *out_val = lv_label_create(blk);
        lv_label_set_text(*out_val, "--");
        lv_obj_set_style_text_color(*out_val, ui_color_text, 0);
        lv_obj_set_style_text_font(*out_val, ui_font_body(), 0);
        lv_obj_set_width(*out_val, LV_PCT(100));
        lv_label_set_long_mode(*out_val, LV_LABEL_LONG_MODE_CLIP);

        lv_obj_t *sep = lv_obj_create(card);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(sep, ui_color_border, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    };

    add_field("Z Position", &g_lbl_z_pos);
    add_field("S Position", &g_lbl_s_pos);
    add_field("D Position", &g_lbl_d_pos);

    return card;
}

/* ============================================================================
 *  功能按键卡片（Motor / Print Tab 共用）
 * ============================================================================ */
static void on_func_wake(lv_event_t *)    { serial_func_wake(); }
static void on_func_sleep(lv_event_t *)   { serial_func_sleep(); }
static void on_func_enable(lv_event_t *)  { serial_func_enable(); }
static void on_func_disable(lv_event_t *) { serial_func_disable(); }

static void on_light_sw(lv_event_t *e)
{
    if(g_sync_in_progress) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    serial_func_light_set(lv_obj_has_state(sw, LV_STATE_CHECKED));
}
static void on_door_sw(lv_event_t *e)
{
    if(g_sync_in_progress) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    /* checked = OPEN, unchecked = CLOSE，与 status.door_closed 取反对应 */
    serial_func_door_set(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/** 在 Functions 卡片里构造一行 "label + switch" */
static lv_obj_t *make_func_sw_row(lv_obj_t *parent, const char *name,
                                  lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD_SM, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, ui_color_text, 0);
    /* 返回 switch 句柄 */
    return make_toggle(row, cb, nullptr);
}

static lv_obj_t *build_func_card(lv_obj_t *parent, func_btn_set_t *out)
{
    lv_obj_t *card = make_card(parent, LV_SYMBOL_KEYBOARD "  Functions");
    lv_obj_set_width(card, SP_FUNC_CARD_W);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_style_pad_row(card, UI_PAD_SM, 0);

    out->wake    = make_button(card, "Wake",    LV_PCT(100), SP_FUNC_BTN_H, on_func_wake);
    out->sleep   = make_button(card, "Sleep",   LV_PCT(100), SP_FUNC_BTN_H, on_func_sleep);
    out->enable  = make_button(card, "Enable",  LV_PCT(100), SP_FUNC_BTN_H, on_func_enable);
    out->disable = make_button(card, "Disable", LV_PCT(100), SP_FUNC_BTN_H, on_func_disable);

    /* 分隔线 */
    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, ui_color_border, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_ver(sep, UI_PAD_SM, 0);

    out->light_sw = make_func_sw_row(card, "Light", on_light_sw);
    out->door_sw  = make_func_sw_row(card, "Door",  on_door_sw);
    return card;
}

/* ============================================================================
 *  Motor Tab
 * ============================================================================ */
static void build_motor_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(tab, UI_PAD, 0);
    lv_obj_set_style_pad_column(tab, UI_PAD, 0);
    lv_obj_set_style_pad_row(tab, UI_PAD, 0);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    /* === 左：电机控制 === */
    lv_obj_t *motor_card = make_card(tab, LV_SYMBOL_DRIVE "  Motor Control");
    lv_obj_set_flex_grow(motor_card, 1);
    lv_obj_set_height(motor_card, LV_PCT(100));

    build_jog_row(motor_card, "Z", JOG_AXIS_Z,
                  LV_SYMBOL_UP, LV_SYMBOL_DOWN, /*icons=*/true,
                  /*show_home=*/true, &g_ta_z_speed,
                  &g_btn_z_pos, &g_btn_z_neg);
    build_inc_row(motor_card, "Z", JOG_AXIS_Z, &g_sw_z_inc, &g_ta_z_inc, &g_btn_z_inc);

    /* 分隔线 */
    auto add_divider = [&](lv_obj_t *parent) {
        lv_obj_t *sep = lv_obj_create(parent);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(sep, ui_color_border, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    };
    add_divider(motor_card);

    build_jog_row(motor_card, "S", JOG_AXIS_S,
                  LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, /*icons=*/true,
                  /*show_home=*/true, &g_ta_s_speed,
                  &g_btn_s_pos, &g_btn_s_neg);
    build_inc_row(motor_card, "S", JOG_AXIS_S, &g_sw_s_inc, &g_ta_s_inc, &g_btn_s_inc);

    add_divider(motor_card);

    /* D 轴：CW/CCW 保留文字，但同样走按下/松开双事件（旋转长按持续） */
    build_jog_row(motor_card, "D", JOG_AXIS_D,
                  "CW", "CCW", /*icons=*/false,
                  /*show_home=*/false, &g_ta_d_speed,
                  &g_btn_d_pos, &g_btn_d_neg);
    build_inc_row(motor_card, "D", JOG_AXIS_D, &g_sw_d_inc, &g_ta_d_inc, &g_btn_d_inc);

    /* === 中：解析数据 === */
    build_parse_card(tab);

    /* === 右：功能按键 === */
    build_func_card(tab, &g_func_motor);
}

/* ============================================================================
 *  Print Tab：回调
 * ============================================================================ */
static void on_print_zero(lv_event_t *)             { serial_print_set_z_work_zero(); }
static void on_print_block(lv_event_t *)            { serial_print_three_way(true); }
static void on_print_open(lv_event_t *)             { serial_print_three_way(false); }
static void on_print_powder(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    serial_print_powder_motor(dir);
}
static void on_laser_sw(lv_event_t *e)
{
    if(g_sync_in_progress) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    serial_print_laser_set(lv_obj_has_state(sw, LV_STATE_CHECKED));
}
static void on_gas_sw(lv_event_t *e)
{
    if(g_sync_in_progress) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    serial_print_wash_gas_set(lv_obj_has_state(sw, LV_STATE_CHECKED));
}
static void on_fan_sw(lv_event_t *e)
{
    if(g_sync_in_progress) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    serial_print_fan_set(lv_obj_has_state(sw, LV_STATE_CHECKED));
}
static void on_fan_duty(lv_event_t *)
{
    int v = atoi(lv_textarea_get_text(g_ta_fan_duty));
    if(v < 0)   v = 0;
    if(v > 100) v = 100;
    serial_print_fan_duty((uint8_t)v);
    ui_toast_show("Fan duty sent", ui_color_accent, 1200);
}

/* ============================================================================
 *  Print Tab：布局
 *  ----------------------------------------------------------------------------
 *  - 左列：可垂直滚动容器，依次堆叠
 *      Row1: Z Axis (3) | 3-Way (2) | Powder (2)
 *      Row2: Outputs (无标题，3 个开关 + Fan Duty 行)
 *      Row3: Sensor 网格（无标题，2×3 大格子）
 *    内容总高超过视口时自动出右侧滚动条。
 *  - 右列：Functions 卡片（窄 170w，固定不滚）
 * ============================================================================ */
static lv_obj_t *make_print_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UI_PAD, 0);
    return row;
}

static void build_print_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(tab, UI_PAD, 0);
    lv_obj_set_style_pad_column(tab, UI_PAD, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    /* ---- 左：可垂直滚动的 column ---- */
    lv_obj_t *left = lv_obj_create(tab);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, UI_PAD, 0);
    /* 内容比视口高时滚动；右侧细滚动条 */
    lv_obj_add_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(left, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(left, LV_SCROLLBAR_MODE_AUTO);

    /* === Row1 === */
    lv_obj_t *row1 = make_print_row(left);

    /* Z 轴 */
    {
        lv_obj_t *card = make_card(row1, "Z Axis");
        lv_obj_set_flex_grow(card, 3);
        make_button(card, "Set Work Zero", LV_PCT(100), SP_PRINT_BTN_H, on_print_zero);

        lv_obj_t *r = make_row(card);
        make_label(r, "Z Work Pos:", 150, &ui_color_text_dim);
        g_lbl_zwork = lv_label_create(r);
        lv_label_set_text(g_lbl_zwork, "--");
        lv_obj_set_style_text_color(g_lbl_zwork, ui_color_text, 0);
        lv_obj_set_flex_grow(g_lbl_zwork, 1);
    }

    /* 三通推杆 */
    {
        lv_obj_t *card = make_card(row1, "3-Way Pusher");
        lv_obj_set_flex_grow(card, 2);
        lv_obj_t *r = make_row(card);
        make_button(r, "Block",   LV_PCT(48), SP_PRINT_BTN_H, on_print_block);
        make_button(r, "Release", LV_PCT(48), SP_PRINT_BTN_H, on_print_open);
    }

    /* 回粉电机 */
    {
        lv_obj_t *card = make_card(row1, "Powder Motor");
        lv_obj_set_flex_grow(card, 2);
        lv_obj_t *r = make_row(card);
        make_button(r, "FWD", LV_PCT(31), SP_PRINT_BTN_H, on_print_powder, (void *)(intptr_t)+1);
        make_button(r, "REV", LV_PCT(31), SP_PRINT_BTN_H, on_print_powder, (void *)(intptr_t)-1);
        make_button(r, "Off", LV_PCT(31), SP_PRINT_BTN_H, on_print_powder, (void *)(intptr_t) 0);
    }

    /* === Row2 ：Laser / Gas / Fan 单张面板（无标题，节省高度） === */
    {
        lv_obj_t *card = make_card(left, nullptr);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);

        /* 第一行：Laser / Gas Wash / Fan 三组（label + switch），等分宽 */
        lv_obj_t *sw_row = lv_obj_create(card);
        lv_obj_remove_style_all(sw_row);
        lv_obj_set_width(sw_row, LV_PCT(100));
        lv_obj_set_height(sw_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_AROUND,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(sw_row, UI_PAD, 0);

        auto add_sw_cell = [&](const char *name, lv_event_cb_t cb,
                               lv_obj_t **out_sw) {
            lv_obj_t *cell = lv_obj_create(sw_row);
            lv_obj_remove_style_all(cell);
            lv_obj_set_width(cell, LV_PCT(31));
            lv_obj_set_height(cell, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(cell, UI_PAD_SM, 0);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_label_set_text(lbl, name);
            lv_obj_set_style_text_color(lbl, ui_color_text, 0);

            *out_sw = make_toggle(cell, cb, nullptr);
        };
        add_sw_cell("Laser",    on_laser_sw, &g_sw_laser);
        add_sw_cell("Gas Wash", on_gas_sw,   &g_sw_gas);
        add_sw_cell("Fan",      on_fan_sw,   &g_sw_fan);

        /* 第二行：Fan Duty 输入 + Send */
        lv_obj_t *duty_row = make_row(card);
        make_label(duty_row, "Fan Duty %:", 150, &ui_color_text_dim);
        g_ta_fan_duty = make_textarea(duty_row, "0-100", 160, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_t *send_btn = make_button(duty_row, "Send", 0, UI_INPUT_H, on_fan_duty);
        lv_obj_set_flex_grow(send_btn, 1);
    }

    /* === Row3：Sensor 数据网格（无标题，2×3 大格子，固定高度由 cell 决定）=== */
    {
        lv_obj_t *card = make_card(left, nullptr);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);  /* 自适应高 → 左列可能超过视口 → 滚动 */

        lv_obj_t *grid = lv_obj_create(card);
        lv_obj_remove_style_all(grid);
        lv_obj_set_width(grid, LV_PCT(100));
        lv_obj_set_height(grid, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(grid, UI_PAD_SM, 0);
        lv_obj_set_style_pad_column(grid, UI_PAD_SM, 0);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

        auto add_cell = [&](const char *name, lv_obj_t **out_val) {
            lv_obj_t *cell = lv_obj_create(grid);
            lv_obj_remove_style_all(cell);
            lv_obj_set_style_bg_color(cell, ui_color_panel_alt, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, ui_color_border, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, UI_RADIUS, 0);
            lv_obj_set_style_pad_all(cell, UI_PAD_SM, 0);

            lv_obj_set_width(cell, LV_PCT(32));   /* 3 列；32% 留出列间距 */
            lv_obj_set_height(cell, SP_SENSOR_CELL_H);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(cell, 2, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            *out_val = lv_label_create(cell);
            lv_label_set_text(*out_val, "--");
            lv_obj_set_style_text_color(*out_val, ui_color_text, 0);
            lv_obj_set_style_text_font(*out_val, ui_font_title(), 0); /* 大号数值 */

            lv_obj_t *t = lv_label_create(cell);
            lv_label_set_text(t, name);
            lv_obj_set_style_text_color(t, ui_color_text_dim, 0);
        };

        add_cell("O2 (%)",          &g_lbl_o2);
        add_cell("Pressure (kPa)",  &g_lbl_press);
        add_cell("Temperature",     &g_lbl_temp);
        add_cell("Humidity (%)",    &g_lbl_hum);
        add_cell("PM2.5 (ug/m3)",   &g_lbl_pm25);
    }

    /* ---- 右：功能按键 ---- */
    build_func_card(tab, &g_func_print);
}

/* ============================================================================
 *  Settings Tab
 *  ----------------------------------------------------------------------------
 *  左：Powder Buckets 2×2 按钮网格
 *  右：Bucket Status 含彩色圆点 + 文字
 * ============================================================================ */
static void on_bucket_click(lv_event_t *e)
{
    intptr_t code = (intptr_t)lv_event_get_user_data(e);
    int idx  = (int)(code >> 1);
    bool open = (code & 1);
    serial_settings_bucket_set(idx, open);
}

/** 用于状态卡片：圆点 + 标题 + 状态文字 */
static void build_bucket_status_row(lv_obj_t *parent, const char *name,
                                    lv_obj_t **out_dot, lv_obj_t **out_lbl)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_PAD, 0);
    lv_obj_set_style_pad_ver(row, UI_PAD_SM, 0);

    *out_dot = make_dot(row);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_color(title, ui_color_text, 0);
    lv_obj_set_width(title, 140);

    *out_lbl = lv_label_create(row);
    lv_label_set_text(*out_lbl, "Unknown");
    lv_obj_set_style_text_color(*out_lbl, ui_color_text_dim, 0);
    lv_obj_set_flex_grow(*out_lbl, 1);
}

static void build_settings_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(tab, UI_PAD, 0);
    lv_obj_set_style_pad_column(tab, UI_PAD, 0);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    /* === 左：粉桶控制 2×2 === */
    lv_obj_t *card = make_card(tab, LV_SYMBOL_SETTINGS "  Powder Buckets");
    lv_obj_set_flex_grow(card, 3);
    lv_obj_set_height(card, LV_SIZE_CONTENT);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, UI_PAD_SM, 0);
    lv_obj_set_style_pad_column(grid, UI_PAD_SM, 0);

    /* code = (idx << 1) | open */
    make_button(grid, "Open Bucket #1",  LV_PCT(48), SP_BUCKET_BTN_H,
                on_bucket_click, (void *)(intptr_t)((1 << 1) | 1));
    make_button(grid, "Open Bucket #2",  LV_PCT(48), SP_BUCKET_BTN_H,
                on_bucket_click, (void *)(intptr_t)((2 << 1) | 1));

    lv_color_t dim = ui_color_accent_dim;
    make_button(grid, "Close Bucket #1", LV_PCT(48), SP_BUCKET_BTN_H,
                on_bucket_click, (void *)(intptr_t)((1 << 1) | 0), &dim);
    make_button(grid, "Close Bucket #2", LV_PCT(48), SP_BUCKET_BTN_H,
                on_bucket_click, (void *)(intptr_t)((2 << 1) | 0), &dim);

    /* === 右：状态指示 === */
    lv_obj_t *st_card = make_card(tab, LV_SYMBOL_EYE_OPEN "  Bucket Status");
    lv_obj_set_flex_grow(st_card, 2);
    lv_obj_set_height(st_card, LV_SIZE_CONTENT);

    build_bucket_status_row(st_card, "Bucket #1", &g_dot_bucket1, &g_lbl_bucket1);

    /* 分隔线 */
    lv_obj_t *sep = lv_obj_create(st_card);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, ui_color_border, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    build_bucket_status_row(st_card, "Bucket #2", &g_dot_bucket2, &g_lbl_bucket2);
}

/* ============================================================================
 *  状态周期刷新
 * ============================================================================ */
static void format_pos(lv_obj_t *lbl, bool valid, int32_t v, const char *unit)
{
    if(!lbl) return;
    if(valid) lv_label_set_text_fmt(lbl, "%d %s", (int)v, unit ? unit : "");
    else      lv_label_set_text(lbl, "--");
}

static void format_float(lv_obj_t *lbl, bool valid, float v, const char *fmt)
{
    if(!lbl) return;
    if(valid) lv_label_set_text_fmt(lbl, fmt, (double)v);
    else      lv_label_set_text(lbl, "--");
}

/** 同步开关 checked 状态：用户业务回调短路，但内部 label 仍需刷新 */
static void sync_switch(lv_obj_t *sw, bool on)
{
    if(!sw) return;
    bool cur = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if(cur == on) return;
    g_sync_in_progress = true;
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else   lv_obj_remove_state(sw, LV_STATE_CHECKED);
    g_sync_in_progress = false;
    /* lv_obj_add/remove_state 不会自动 send VALUE_CHANGED，需要手动同步文字 */
    update_toggle_label(sw);
}

static void update_func_btns(func_btn_set_t *set, const serial_status_t *st)
{
    if(!set) return;
    /* 4 颗动作按钮：仅按当前状态做色调反馈 */
    if(set->enable) {
        btn_set_color(set->enable, st->enabled ? ui_color_accent : ui_color_accent_dim);
    }
    if(set->disable) {
        btn_set_color(set->disable, !st->enabled ? ui_color_danger : ui_color_accent_dim);
    }
    if(set->wake) {
        btn_set_color(set->wake, st->awake ? ui_color_accent : ui_color_accent_dim);
    }
    if(set->sleep) {
        btn_set_color(set->sleep, !st->awake ? ui_color_accent : ui_color_accent_dim);
    }
    /* 滑动开关：橙=ON / 灰=OFF
     *   Light：checked == light_on
     *   Door ：checked == 门打开（即 !door_closed）
     */
    sync_switch(set->light_sw, st->light_on);
    sync_switch(set->door_sw,  !st->door_closed);
}

static void refresh_cb(lv_timer_t *)
{
    serial_status_t st;
    serial_backend_get_status(&st);

    /* Motor Tab: 解析数据 */
    format_pos(g_lbl_z_pos, st.z_pos_valid, st.z_pos, "");
    format_pos(g_lbl_s_pos, st.s_pos_valid, st.s_pos, "");
    format_pos(g_lbl_d_pos, st.d_pos_valid, st.d_pos, "");

    /* Print Tab: 传感器与状态 */
    format_pos(g_lbl_zwork, st.z_work_valid, st.z_work_pos, "");
    format_float(g_lbl_o2,    st.o2_valid,       st.o2_pct,       "%.2f");
    format_float(g_lbl_press, st.chamber_valid,  st.chamber_kpa,  "%.2f");
    format_float(g_lbl_temp,  st.temp_valid,     st.temp_c,       "%.1f");
    format_float(g_lbl_hum,   st.humidity_valid, st.humidity_pct, "%.1f");
    format_float(g_lbl_pm25,  st.pm25_valid,     st.pm25_ugm3,    "%.1f");

    /* Print Tab Outputs 三开关同步 */
    sync_switch(g_sw_laser, st.laser_on);
    sync_switch(g_sw_gas,   st.wash_gas_on);
    sync_switch(g_sw_fan,   st.fan_running);

    /* Settings Tab */
    auto bucket_text = [](int8_t s) {
        return (s == 1) ? "Open" : (s == 0 ? "Closed" : "Unknown");
    };
    auto bucket_dot_color = [](int8_t s) -> lv_color_t {
        return (s == 1) ? ui_color_accent
             : (s == 0) ? ui_color_text_dim
                        : ui_color_danger;   /* 未知态用红色提醒 */
    };

    if(g_lbl_bucket1) lv_label_set_text(g_lbl_bucket1, bucket_text(st.bucket1_state));
    if(g_lbl_bucket2) lv_label_set_text(g_lbl_bucket2, bucket_text(st.bucket2_state));
    if(g_dot_bucket1) lv_obj_set_style_bg_color(g_dot_bucket1, bucket_dot_color(st.bucket1_state), 0);
    if(g_dot_bucket2) lv_obj_set_style_bg_color(g_dot_bucket2, bucket_dot_color(st.bucket2_state), 0);

    /* 功能按键状态 */
    update_func_btns(&g_func_motor, &st);
    update_func_btns(&g_func_print, &st);

    /* 串口状态栏 */
    if(g_lbl_serial_stat) {
        lv_label_set_text_fmt(g_lbl_serial_stat,
            "%s  %s @ %d  TX:%u  RX:%u  Lines:%u",
            serial_backend_is_open() ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
            serial_backend_device(), serial_backend_baud(),
            (unsigned)st.tx_bytes, (unsigned)st.rx_bytes, (unsigned)st.rx_lines);
        lv_obj_set_style_text_color(g_lbl_serial_stat,
            serial_backend_is_open() ? ui_color_accent : ui_color_danger, 0);
    }
}

/* ============================================================================
 *  入口
 * ============================================================================ */
lv_obj_t *page_serial_create(lv_obj_t *parent)
{
    /* 后端初始化（若已初始化则忽略） */
    if(!serial_backend_is_open()) {
        serial_backend_init(SERIAL_DEFAULT_DEVICE, SERIAL_DEFAULT_BAUD);
    }

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD_SM, 0);
    lv_obj_set_style_bg_color(page, ui_color_bg, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部标题 + 串口状态 */
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, UI_PAD, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_USB "  Serial");
    lv_obj_add_style(title, ui_style_title(), 0);

    g_lbl_serial_stat = lv_label_create(header);
    lv_label_set_text(g_lbl_serial_stat, "--");
    lv_obj_set_style_text_color(g_lbl_serial_stat, ui_color_text_dim, 0);
    lv_obj_set_flex_grow(g_lbl_serial_stat, 1);
    lv_label_set_long_mode(g_lbl_serial_stat, LV_LABEL_LONG_MODE_CLIP);

    /* Tabview ：根容器强制刷黑 */
    lv_obj_t *tv = lv_tabview_create(page);
    lv_obj_set_width(tv, LV_PCT(100));
    lv_obj_set_flex_grow(tv, 1);
    lv_tabview_set_tab_bar_size(tv, 64);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    paint_dark(tv);

    /* Tab bar 样式 */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(tab_bar, ui_color_sidebar, 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab_bar, ui_color_border, 0);
    lv_obj_set_style_border_width(tab_bar, 1, 0);
    lv_obj_set_style_radius(tab_bar, UI_RADIUS, 0);
    lv_obj_set_style_text_font(tab_bar, ui_font_body(), 0);
    lv_obj_set_style_text_color(tab_bar, ui_color_text, 0);

    /* Tab 内容容器（所有 tab page 的父）也强制刷黑 */
    lv_obj_t *tv_content = lv_tabview_get_content(tv);
    paint_dark(tv_content);

    lv_obj_t *tab_motor    = lv_tabview_add_tab(tv, LV_SYMBOL_DRIVE   "  Motor");
    lv_obj_t *tab_print    = lv_tabview_add_tab(tv, LV_SYMBOL_PLAY    "  Print");
    lv_obj_t *tab_settings = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS "  Settings");

    /* 每个 tab page：刷黑底，去 padding/border */
    for(lv_obj_t *t : { tab_motor, tab_print, tab_settings }) {
        paint_dark(t);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_radius(t, 0, 0);
    }

    build_motor_tab(tab_motor);
    build_print_tab(tab_print);
    build_settings_tab(tab_settings);

    /* 默认填充数值，避免空白 */
    if(g_ta_z_speed)  lv_textarea_set_text(g_ta_z_speed, "100");
    if(g_ta_s_speed)  lv_textarea_set_text(g_ta_s_speed, "100");
    if(g_ta_d_speed)  lv_textarea_set_text(g_ta_d_speed, "100");
    if(g_ta_z_inc)    lv_textarea_set_text(g_ta_z_inc,   "1.0");
    if(g_ta_s_inc)    lv_textarea_set_text(g_ta_s_inc,   "1.0");
    if(g_ta_d_inc)    lv_textarea_set_text(g_ta_d_inc,   "1.0");
    if(g_ta_fan_duty) lv_textarea_set_text(g_ta_fan_duty, "50");

    /* 周期刷新 */
    lv_timer_create(refresh_cb, 200, nullptr);

    return page;
}
