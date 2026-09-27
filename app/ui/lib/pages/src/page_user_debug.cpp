#include "page_user_debug.hpp"
#include "page_user_debug_test.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "page_manager.hpp"
#include "debug_msg_proxy.hpp"
#include "control.hpp"
#include "ui_message_store.hpp"
#include "ui_alarm_state.hpp"
#include "ui_sys_hud.hpp"
#include "upc_sys_monitor.hpp"
#include "config/cal_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>

/**
 * @file page_user_debug.cpp
 * @brief 调试页（开发者/工厂手动控制台）—— "移动控制" Tab。
 *
 * 顶部传感器条（含 Zone4 本机 thermal）+ 八路料位红外条（A1..OUTR）+ 设备快捷行 + Tab（Move/Other/Laser/Test）。
 * Test（测试）Tab 为自动化用例表，实现见 page_user_debug_test.cpp（中文界面）。
 * 移动控制：左列 Z/S/D 三轴卡（点击移动/长按移动），右列 振镜坐标卡 + 振镜标定卡。
 * 振镜方向盘（X/-X/Y/-Y）按 Step 本地增减中心偏移；复位回上次 Save/读盘值。
 * Abs X / Abs Y + Move：按 snowx 口径发 UI_Debug_Galvo_AbsMove_Ntf，真正移动振镜。
 * Save 写 cal.csv 第 4 行并由 Glavo load_calibration；测试图/正式打印中禁用。
 * Test image 开关：红光循环打印 /userdata/test/test.gcode（经 DebugMsgProxy）。
 * Other 点位测试：按校准表显示中心 + 四角，每点 3 秒，与打印/Test image 互斥。
 * Other 页按 Process / Cabin / Print Gates / Media & Sim / Inject 分组；
 * Process 首行为回粉电机 + 筛粉电机点动（粉桶按钮已撤），Powder Clean 在 Print Gates 末行，
 * MCU 清粉结束（GLAVO_Debug_Clean_Test_Ntf）时自动复位为 Off 并显示结果；
 * Media & Sim 含 Sys HUD：开则右上角 lv_layer_sys 红字蒙版显示 CPU 温/占用/内存。
 * Zone4 温度在顶部传感器条常显（本机 thermal_zone4，不经 IPC）。
 * 左侧 Print Params + Fan + O2 Calib（参考浓度输入后点圆钮标定）。
 * 其余命令经 DebugMsgProxy → APP_GLAVO（后端待实现）；图标暂用 LV_SYMBOL/文字占位。
 */

namespace {

using middleware::DebugAxis;
using middleware::DebugJogMode;
using middleware::DebugGalvoAxis;
using middleware::DebugDeviceTarget;
using middleware::DebugMotorAction;
using middleware::DebugSwitchTarget;

/* ---- 三轴配置（英文标签，受 Montserrat 字库约束） ---- */
struct AxisCfg {
    DebugAxis   axis;
    const char *title;
    const char *unit;        /**< 距离单位（位置显示） */
    const char *step_unit;   /**< 点击移动(增量)输入单位；Z 为 um */
    float       step_scale;  /**< 点击输入 × step_scale = 契约 value（mm/°）；Z 为 um→mm */
    const char *speed_unit;  /**< 速度单位 */
    bool        has_zero;    /**< Z 轴有"工作零点" */
    bool        rotational;  /**< D 轴用旋转箭头 */
};

const AxisCfg kAxes[3] = {
    { DebugAxis::Z, "Print Plane Height (Z)", "mm", "um", 0.001f, "mm/s", true,  false },
    { DebugAxis::S, "Powder Scraper (S)",     "mm", "mm", 1.0f,   "mm/s", false, false },
    { DebugAxis::D, "Powder Bucket Rod (D)",  "\xC2\xB0", "\xC2\xB0", 1.0f, "\xC2\xB0/s", false, true },
};

/* 协议 S/D 的 INC 与三轴 JOG 为 int16 整数（mm·°/mm·°·s），Z 的 INC 为 int32 μm；步距/速度默认 0 */
constexpr float kAxisDefaultStep = 0.0f;

/* 振镜单次步长候选 */
const float kGalvoSteps[4] = { 0.01f, 0.1f, 1.0f, 10.0f };

/* ---- 页面对象/状态 ---- */
lv_obj_t *g_tab1 = nullptr;          /**< 移动控制内容 */
lv_obj_t *g_tab2 = nullptr;          /**< 其他控制内容 */
lv_obj_t *g_tab3 = nullptr;          /**< 激光器控制内容 */
lv_obj_t *g_tab4 = nullptr;          /**< 测试（自动化用例表）内容 */
lv_obj_t *g_height_calib_lbl = nullptr; /**< 高度校准进度字 */

/* Laser 标签页回读控件 */
lv_obj_t *g_laser_status_lbl[4] = { nullptr };  /**< 红光/输出/Ready/外部 状态字 */
lv_obj_t *g_laser_status_dot[4] = { nullptr };  /**< 对应状态圆点 */
lv_obj_t *g_laser_alarm_box     = nullptr;      /**< 报警列表容器 */
lv_obj_t *g_laser_temp_lbl      = nullptr;      /**< 泵浦温度读数 */
lv_obj_t *g_laser_power_lbl     = nullptr;      /**< 功率读数 */
lv_obj_t *g_laser_power_ta      = nullptr;      /**< 设置功率输入 */
lv_obj_t *g_laser_type_lbl      = nullptr;      /**< 激光器类型显示区（JFSC / AL 200W / …） */
lv_obj_t *g_laser_current_lbl   = nullptr;      /**< 电流读数 */
uint8_t   g_laser_type_code     = 0;            /**< 最近一次上报的激光器型号（0=未知 1=JFSC 2=AL） */

lv_obj_t *g_sensor_val[10] = { nullptr }; /**< [9]=Zone4（本机 thermal_zone4） */
/** 主板 8 路料位红外指示：A1/A2/A3/B1/B2/B3/OUTL/OUTR */
lv_obj_t *g_powder_ir_val[8] = { nullptr };
lv_timer_t *g_zone4_timer = nullptr;      /**< 顶部条 Zone4 约 1Hz 刷新 */
lv_obj_t *g_axis_pos[3]   = { nullptr };
lv_obj_t *g_axis_step[3]  = { nullptr };   /**< 点击距离/速度输入（按 tab 复用） */
lv_obj_t *g_axis_ctrl[3]  = { nullptr };   /**< 控制区容器（按子 tab 重建） */
uint8_t   g_axis_mode[3]  = { 0, 0, 0 };   /**< 0=点击 1=长按 */
/** 各轴 × 子 tab 的步距/速度记忆（切换子 tab 不丢值） */
float     g_axis_step_saved[3][2] = {};
lv_obj_t *g_z_zero_lbl     = nullptr;
lv_obj_t *g_galvo_x_lbl    = nullptr;
lv_obj_t *g_galvo_y_lbl    = nullptr;
lv_obj_t *g_galvo_scale_ta = nullptr;
lv_obj_t *g_galvo_abs_x_ta = nullptr;   /**< 振镜绝对坐标 X 输入框(mm) */
lv_obj_t *g_galvo_abs_y_ta = nullptr;   /**< 振镜绝对坐标 Y 输入框(mm) */
lv_obj_t *g_galvo_step_seg = nullptr;
lv_obj_t *g_test_image_seg = nullptr;
lv_obj_t *g_galvo_save_btn = nullptr;
lv_obj_t *g_galvo_point_test_btn = nullptr; /**< Other：中心点 + 四角红光测试圆钮 */
lv_obj_t *g_galvo_point_test_lbl = nullptr; /**< 点位测试状态字（Idle/Running） */
bool      g_galvo_point_test_pending = false; /**< 点击后等待 Glavo 首次状态回报 */
bool      g_galvo_center_valid = false;
double    g_galvo_center_x = 0.0;   /**< 编辑中的中心偏移（未 Save 前仅 UI） */
double    g_galvo_center_y = 0.0;
double    g_galvo_saved_x = 0.0;    /**< 上次从 cal.csv / Save 成功后的值（复位目标） */
double    g_galvo_saved_y = 0.0;
lv_obj_t *g_dev_btn[4]     = { nullptr };
lv_obj_t *g_dev_state[4]   = { nullptr };   /**< 设备快捷行状态字（已唤醒/休眠中…） */
lv_obj_t *g_fan_duty_ta      = nullptr;     /**< 其他控制：风扇占空比输入 */
lv_obj_t *g_fan_rpm_lbl      = nullptr;     /**< 风扇卡：MCU 实测转速（与 Duty 同行便于对照） */
lv_obj_t *g_fan_state_lbl    = nullptr;     /**< 风扇卡：风机状态机 / 故障名 */
lv_obj_t *g_print_layer_ta   = nullptr;     /**< 其他控制：打印层厚输入 */
lv_obj_t *g_print_balance_ta = nullptr;     /**< 其他控制：打印平衡系数×1000 输入 */
lv_obj_t *g_print_h264_btn   = nullptr;     /**< 其他控制：打印期 H264 开关按钮 */
lv_obj_t *g_print_h264_state_lbl = nullptr; /**< 其他控制：打印期 H264 状态文本 */
lv_obj_t *g_powder_clean_btn = nullptr;     /**< Print Gates：清粉开关按钮（MCU 结束回执复位） */
lv_obj_t *g_powder_clean_state_lbl = nullptr; /**< Print Gates：清粉状态文本 */
lv_obj_t *g_clean_record_btn = nullptr;     /**< 其他控制：清粉录像开关按钮 */
lv_obj_t *g_clean_record_state_lbl = nullptr; /**< 其他控制：清粉录像状态文本 */
lv_obj_t *g_env_sim_btn = nullptr;          /**< 其他控制：环境上报模拟开关按钮 */
lv_obj_t *g_env_sim_state_lbl = nullptr;    /**< 其他控制：环境上报模拟状态文本 */
lv_obj_t *g_env_sim_powder_btn = nullptr;   /**< 其他控制：环境模拟缺粉按钮 */
lv_obj_t *g_env_sim_powder_state_lbl = nullptr; /**< 其他控制：环境模拟缺粉状态文本 */
lv_obj_t *g_skip_o2_gate_btn = nullptr;     /**< 其他控制：跳过氧门+打印前洗气 */
lv_obj_t *g_skip_o2_gate_state_lbl = nullptr;
lv_obj_t *g_skip_outer_door_btn = nullptr;  /**< 其他控制：跳过外门检测+关内门套件 */
lv_obj_t *g_skip_outer_door_state_lbl = nullptr;
lv_obj_t *g_inner_door_btns[3] = {};        /**< Inner Door：关 / 停 / 开 */
lv_obj_t *g_inner_door_state_lbl = nullptr;
bool      g_inner_door_busy = false;
lv_obj_t *g_o2_calib_btn = nullptr;         /**< 其他控制：氧单点标定 */
lv_obj_t *g_o2_calib_lbl = nullptr;         /**< 显示当前 K / 标定状态 */
lv_obj_t *g_o2_calib_ref_ta = nullptr;      /**< 标定时参考氧浓度 % */
bool      g_o2_calib_pending = false;       /**< 点击后等待 Rsp */
lv_obj_t *g_first_layer_visual_btn = nullptr; /**< 其他控制：首层视觉检测开关按钮 */
lv_obj_t *g_first_layer_visual_state_lbl = nullptr; /**< 其他控制：首层视觉检测状态文本 */
lv_obj_t *g_optical_path_check_btn = nullptr; /**< 其他控制：Skip Optical Path Chk（按下=跳过光路） */
lv_obj_t *g_optical_path_check_state_lbl = nullptr; /**< 其他控制：Skip Optical Path Chk 状态字 */
lv_obj_t *g_plate_height_adj_btn = nullptr; /**< 其他控制：打印前基板测高开关按钮 */
lv_obj_t *g_plate_height_adj_state_lbl = nullptr; /**< 其他控制：打印前基板测高状态文本 */
lv_obj_t *g_inner_door_occlusion_btn = nullptr; /**< 其他控制：跳过内门视觉挡门 */
lv_obj_t *g_inner_door_occlusion_state_lbl = nullptr;
lv_obj_t *g_plate_present_btn = nullptr; /**< 其他控制：跳过基板在位读码 */
lv_obj_t *g_plate_present_state_lbl = nullptr;
lv_obj_t *g_soc_led_btn[middleware::kDebugSocLedCount] = {};
lv_obj_t *g_soc_led_state_lbl[middleware::kDebugSocLedCount] = {};
lv_obj_t *g_cabin_pm_valve_btn = nullptr;   /**< 其他控制：舱内 PM fa_valve */
lv_obj_t *g_cabin_pm_valve_state_lbl = nullptr;
/* SPI 灯板验色：三个互斥的强制色按钮（下标同 kStatusLedBtns）+ 释放接管的 Auto 键
 * + 裸 RGB 调色输入 */
lv_obj_t *g_status_led_btn[3] = {};
lv_obj_t *g_status_led_state_lbl[3] = {};
lv_obj_t *g_status_led_rgb_ta[3] = {};      /**< R/G/B 输入框（0-255） */
lv_obj_t *g_status_led_rgb_lbl = nullptr;   /**< 裸 RGB 当前生效值回显 */
lv_obj_t *g_status_led_auto_btn = nullptr;  /**< 释放强制接管，回自动推导 */
lv_obj_t *g_status_led_auto_lbl = nullptr;
lv_obj_t *g_spi_pm25_lcd_btn = nullptr;     /**< SPI 小屏测试刷屏 */
lv_obj_t *g_spi_pm25_lcd_state_lbl = nullptr;
lv_obj_t *g_local_rtsp_btn = nullptr;       /**< 其他控制：本地 RTSP Server 开关按钮 */
lv_obj_t *g_local_rtsp_state_lbl = nullptr; /**< 其他控制：本地 RTSP Server 状态文本 */
bool g_env_sim_on = false;                  /**< 缓存 Env Sim 回读状态，用于门控 Powder Sim 按钮 */

/* 六路舱灯名称（sysfs lgt*；与 Device/Light 串口舱灯无关） */
const char *const kSocLedNames[middleware::kDebugSocLedCount] = {
    "LED 1L", "LED 1R", "LED 2L", "LED 2R", "LED 3L", "LED 3R"
};

/* 设备快捷行状态字（受 Montserrat 字库约束，沿用英文） */
const char *const kDevOn[4]  = { "Awake", "Open", "Running", "On" };
const char *const kDevOff[4] = { "Sleeping", "Closed", "Sleeping", "Off" };

/* jog 按钮 user_data 编码：axis(2bit)<<2 | dirbit<<1 | mode */
inline void *jog_ud(uint8_t axis, int dir, uint8_t mode) {
    const uintptr_t v = (static_cast<uintptr_t>(axis) << 2) |
                        ((dir < 0 ? 0u : 1u) << 1) | (mode & 1u);
    return reinterpret_cast<void *>(v);
}
inline void jog_decode(void *ud, uint8_t &axis, int &dir, uint8_t &mode) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(ud);
    axis = static_cast<uint8_t>((v >> 2) & 0x3u);
    dir  = ((v >> 1) & 1u) ? 1 : -1;
    mode = static_cast<uint8_t>(v & 1u);
}

float read_ta_float(lv_obj_t *ta) {
    if(!ta) {
        return 0.0f;
    }
    const char *s = lv_textarea_get_text(ta);
    return s ? static_cast<float>(std::atof(s)) : 0.0f;
}

/**
 * 从cal.csv读取中心点偏移（第 4 行：X,<offset>,Y,<offset>）。
 * 本页仅用于调试展示，读取失败时回退为无效态（标签显示 "--"）。
 */
bool load_galvo_center_offset_from_cal(double &out_x, double &out_y)
{
    out_x = 0.0;
    out_y = 0.0;
    FILE *fp = std::fopen(upc::config::kCalDefaultPath, "r");
    if(!fp) {
        return false;
    }
    char line[512];
    bool ok = false;
    while(std::fgets(line, sizeof(line), fp)) {
        double x = 0.0;
        double y = 0.0;
        if(std::sscanf(line, " X,%lf,Y,%lf", &x, &y) == 2 ||
           std::sscanf(line, " x,%lf,y,%lf", &x, &y) == 2)
        {
            out_x = x;
            out_y = y;
            ok = true;
            break;
        }
    }
    std::fclose(fp);
    return ok;
}

void apply_galvo_center_labels()
{
    if(g_galvo_x_lbl) {
        if(g_galvo_center_valid) {
            lv_label_set_text_fmt(g_galvo_x_lbl, "%.3f", g_galvo_center_x);
        } else {
            lv_label_set_text(g_galvo_x_lbl, "--");
        }
    }
    if(g_galvo_y_lbl) {
        if(g_galvo_center_valid) {
            lv_label_set_text_fmt(g_galvo_y_lbl, "%.3f", g_galvo_center_y);
        } else {
            lv_label_set_text(g_galvo_y_lbl, "--");
        }
    }
}

/** 正式打印或任一振镜测试运行中禁止写 cal.csv */
bool galvo_save_busy()
{
    const bool test_seg_on =
        g_test_image_seg &&
        ui_user_segmented_get_selected(g_test_image_seg) == 0;
    return control_is_printing() ||
           ui::DebugMsgProxy::instance().test_image_active() ||
           ui::DebugMsgProxy::instance().galvo_point_test_active() ||
           g_galvo_point_test_pending ||
           test_seg_on;
}

void update_galvo_test_controls()
{
    const auto& proxy = ui::DebugMsgProxy::instance();
    const bool point_test_busy =
        proxy.galvo_point_test_active() || g_galvo_point_test_pending;
    const bool test_image_on =
        proxy.test_image_active() ||
        (g_test_image_seg &&
         ui_user_segmented_get_selected(g_test_image_seg) == 0);

    if(g_galvo_save_btn) {
        if(galvo_save_busy()) {
            lv_obj_add_state(g_galvo_save_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_galvo_save_btn, LV_STATE_DISABLED);
        }
    }
    if(g_test_image_seg) {
        if(point_test_busy) {
            lv_obj_add_state(g_test_image_seg, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_test_image_seg, LV_STATE_DISABLED);
        }
    }
    /* 与 set_dbg_toggle_enabled 同款门控：连 CLICKABLE 一起摘，避免灰态仍可点。 */
    if(g_galvo_point_test_btn) {
        if(control_is_printing() || test_image_on || point_test_busy) {
            lv_obj_clear_flag(g_galvo_point_test_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_state(g_galvo_point_test_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_flag(g_galvo_point_test_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(g_galvo_point_test_btn, LV_STATE_DISABLED);
        }
    }
    if(g_galvo_point_test_lbl) {
        lv_label_set_text(g_galvo_point_test_lbl,
                          point_test_busy ? "Running" : "Idle");
        lv_obj_set_style_text_color(g_galvo_point_test_lbl,
            point_test_busy ? user_light_accent : user_light_icon_off, 0);
    }
}

void update_galvo_save_enabled()
{
    update_galvo_test_controls();
}

/* 调试页按钮尺寸：在主题默认基础上放大约 1/3，便于触摸（仅本页使用，不影响其它页面）。
 * 依赖运行时 g_ui，必须在构建期（函数内）取值，不能做全局 constexpr。 */
inline int32_t dbg_axis_btn_sz()  { return USER_BACK_BTN_H * 4 / 3; }        /* 轴 +/- < > 复位 */
inline int32_t dbg_round_btn_sz() { return USER_BTN_DOOR_ICON_SZ * 4 / 3; }  /* 振镜 / 其他开关圆钮 */

/* 圆形图标按钮（jog / dpad / 设备开关复用） */
lv_obj_t *make_circle_btn(lv_obj_t *parent, int32_t sz, const char *sym,
                          lv_event_cb_t cb, void *ud, bool accent = false)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    /* 常态主色描边：标出可点击范围（浅色档） */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, user_light_accent, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    if(accent) {
        lv_obj_set_style_bg_color(btn, user_light_accent, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        /* 浅墨填充，与白卡拉开区分（对齐 Manual jog） */
        lv_obj_set_style_bg_color(btn, user_light_text, 0);
        lv_obj_set_style_bg_opa(btn, 13, 0);
    }
    /* 点击反馈：蓝底 + 蓝边 */
    lv_obj_set_style_bg_color(btn, user_light_accent, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, user_light_accent, LV_STATE_PRESSED);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, sym);
    /* 蓝底用白字，浅墨底用深字 */
    lv_obj_set_style_text_color(l, accent ? lv_color_white() : user_light_text, 0);
    lv_obj_set_style_text_font(l, user_font_text(), 0);
    lv_obj_center(l);
    return btn;
}

/** 调试页浅色白卡：替掉深色磨砂卡，与设置/首页卡片同款。 */
lv_obj_t *make_dbg_light_card(lv_obj_t *parent, bool flex_grow)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    if(flex_grow) {
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, LV_PCT(100));
    } else {
        lv_obj_set_height(card, LV_SIZE_CONTENT);
    }
    lv_obj_set_style_radius(card, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, 26, 0);
    lv_obj_set_style_shadow_width(card, 6, 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, 10, 0);
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_set_style_text_color(card, user_light_text, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void on_back_click(lv_event_t *)
{
    ui::PageManager::instance().switch_to("user_settings");
}

/* ---- jog 事件 ---- */
void on_jog_click(lv_event_t *e)
{
    uint8_t axis, mode; int dir;
    jog_decode(lv_event_get_user_data(e), axis, dir, mode);
    ui::DebugMsgProxy::instance().send_axis_jog(
        static_cast<DebugAxis>(axis), DebugJogMode::Click, dir,
        read_ta_float(g_axis_step[axis]) * kAxes[axis].step_scale);
}

void on_jog_hold(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    uint8_t axis, mode; int dir;
    jog_decode(lv_event_get_user_data(e), axis, dir, mode);
    auto &proxy = ui::DebugMsgProxy::instance();
    if(code == LV_EVENT_PRESSED) {
        proxy.send_axis_jog(static_cast<DebugAxis>(axis), DebugJogMode::Hold, dir,
                            read_ta_float(g_axis_step[axis]));
    } else { /* RELEASED / PRESS_LOST */
        proxy.send_axis_hold_stop(static_cast<DebugAxis>(axis));
    }
}

/* 归零按钮：发送轴回 Home（仅 Z/S；D 轴无此按钮） */
void on_axis_home(lv_event_t *e)
{
    const uint8_t axis = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if(axis >= 2) {
        return;
    }
    ui::DebugMsgProxy::instance().send_axis_home(static_cast<DebugAxis>(axis));
}

void on_set_zero(lv_event_t *e)
{
    const uint8_t axis = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    ui::DebugMsgProxy::instance().send_axis_set_zero(static_cast<DebugAxis>(axis));
    ui_toast_show("Work zero set", user_light_accent, 1200);
}

/* ---- 顶部传感器条 ---- */
lv_obj_t *make_sensor_cell(lv_obj_t *parent, const char *caption, int idx)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    /* 左侧分隔线 */
    if(idx > 0) {
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, user_light_divider, 0);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_LEFT, 0);
    }
    lv_obj_t *cap = lv_label_create(cell);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *val = lv_label_create(cell);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_light_text, 0);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    if(idx >= 0 && idx < 10) {
        g_sensor_val[idx] = val;
    }
    return cell;
}

void refresh_zone4_temp(lv_timer_t * /*t*/)
{
    if(!g_sensor_val[9]) {
        return;
    }
    UpcSysMonitorSample s{};
    if(!upc_sys_monitor_sample(&s) || s.zone4_temp_c < 0.f) {
        lv_label_set_text(g_sensor_val[9], "--");
        return;
    }
    lv_label_set_text_fmt(g_sensor_val[9], "%.1f",
                          static_cast<double>(s.zone4_temp_c));
}

void start_zone4_timer(void)
{
    if(!g_zone4_timer) {
        g_zone4_timer = lv_timer_create(refresh_zone4_temp, 1000, nullptr);
    }
    refresh_zone4_temp(nullptr);
}

void stop_zone4_timer(void)
{
    if(g_zone4_timer) {
        lv_timer_delete(g_zone4_timer);
        g_zone4_timer = nullptr;
    }
}

lv_obj_t *build_sensor_strip(lv_obj_t *parent)
{
    lv_obj_t *strip = make_dbg_light_card(parent, false);
    /* 本页不显示常驻状态栏，卡高与 USER_TOPBAR_H 脱钩（后者已按稿抬到屏高 16%）。 */
    lv_obj_set_height(strip, USER_SENSOR_STRIP_H);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(strip, USER_PAD_SM, 0);
    lv_obj_set_style_pad_hor(strip, 0, 0);

    const char *caps[10] = {
        "Oxygen", "Filter O2", "Pressure", "PM Out", "Humidity",
        "Cabin Temp", "Plate Temp", "PM Cabin", "Powder", "Zone4",
    };
    for(int i = 0; i < 10; i++) {
        make_sensor_cell(strip, caps[i], i);
    }
    return strip;
}

/**
 * 主板 8 路料位红外（1ST powder_ir_bits）。
 * 协议：1=受光/无粉，0=遮挡有粉。有粉用 accent 标 "Blk"，无粉用 dim 标 "Clr"。
 */
lv_obj_t *build_powder_ir_strip(lv_obj_t *parent)
{
    lv_obj_t *strip = make_dbg_light_card(parent, false);
    lv_obj_set_width(strip, LV_PCT(100));
    lv_obj_set_height(strip, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(strip, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_hor(strip, USER_PAD_SM, 0);

    lv_obj_t *title = lv_label_create(strip);
    lv_label_set_text(title, "Powder IR");
    lv_obj_set_style_text_color(title, user_light_icon_off, 0);
    lv_obj_set_style_text_font(title, user_font_caption(), 0);

    static const char *const kIrCaps[8] = {
        "A1", "A2", "A3", "B1", "B2", "B3", "L", "R",
    };
    for(int i = 0; i < 8; ++i) {
        lv_obj_t *cell = lv_obj_create(strip);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 2, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *cap = lv_label_create(cell);
        lv_label_set_text(cap, kIrCaps[i]);
        lv_obj_set_style_text_color(cap, user_light_icon_off, 0);
        lv_obj_set_style_text_font(cap, user_font_caption(), 0);

        lv_obj_t *val = lv_label_create(cell);
        lv_label_set_text(val, "--");
        lv_obj_set_style_text_color(val, user_light_text, 0);
        lv_obj_set_style_text_font(val, user_font_text(), 0);
        g_powder_ir_val[i] = val;
    }
    return strip;
}

void apply_powder_ir_bits(uint8_t bits, bool valid)
{
    for(int i = 0; i < 8; ++i) {
        lv_obj_t *lbl = g_powder_ir_val[i];
        if(!lbl) {
            continue;
        }
        if(!valid) {
            lv_label_set_text(lbl, "--");
            lv_obj_set_style_text_color(lbl, user_light_icon_off, 0);
            continue;
        }
        /* bit=0 遮挡有粉 → Blk；bit=1 受光无粉 → Clr */
        const bool clear = (bits & static_cast<uint8_t>(1u << i)) != 0;
        lv_label_set_text(lbl, clear ? "Clr" : "Blk");
        lv_obj_set_style_text_color(lbl,
            clear ? user_light_icon_off : user_light_accent, 0);
    }
}

/* ---- 设备快捷行 ---- */
void on_device_toggle(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const uint8_t target = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    const bool on = lv_obj_has_state(btn, LV_STATE_CHECKED);
    if(target < 4 && g_dev_state[target]) {
        lv_label_set_text(g_dev_state[target], on ? kDevOn[target] : kDevOff[target]);
        lv_obj_set_style_text_color(g_dev_state[target],
                                    on ? user_light_accent : user_light_icon_off, 0);
    }
    ui::DebugMsgProxy::instance().send_device(
        static_cast<DebugDeviceTarget>(target), on);
}

lv_obj_t *make_device_toggle(lv_obj_t *parent, DebugDeviceTarget target,
                             const char *name, const char *sym)
{
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    /* 左列：名称 + 状态字（纵向） */
    lv_obj_t *col = lv_obj_create(grp);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(col);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);

    const uint8_t ti = static_cast<uint8_t>(target);
    lv_obj_t *state = lv_label_create(col);
    lv_label_set_text(state, ti < 4 ? kDevOff[ti] : "");
    lv_obj_set_style_text_color(state, user_light_icon_off, 0);
    lv_obj_set_style_text_font(state, user_font_text(), 0);
    if(ti < 4) {
        g_dev_state[ti] = state;
    }

    lv_obj_t *btn = lv_button_create(grp);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_BTN_DOOR_ICON_SZ, USER_BTN_DOOR_ICON_SZ);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, user_light_accent, 0);
    lv_obj_set_style_bg_color(btn, user_light_text, 0);
    lv_obj_set_style_bg_opa(btn, 13, 0);
    lv_obj_set_style_bg_color(btn, user_light_accent, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_light_accent, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, user_light_accent, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, user_light_accent, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_device_toggle, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(
                            static_cast<uint8_t>(target))));
    if(static_cast<uint8_t>(target) < 4) {
        g_dev_btn[static_cast<uint8_t>(target)] = btn;
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, user_light_text, 0);
    lv_obj_set_style_text_font(ic, user_font_icon(), 0);
    lv_obj_center(ic);
    return grp;
}

void on_tab_changed(lv_event_t *e)
{
    lv_obj_t *seg = lv_event_get_target_obj(e);
    const uint8_t sel = ui_user_segmented_get_selected(seg);
    auto set_vis = [](lv_obj_t *o, bool show) {
        if(!o) return;
        if(show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    set_vis(g_tab1, sel == 0);
    set_vis(g_tab2, sel == 1);
    set_vis(g_tab3, sel == 2);
    set_vis(g_tab4, sel == 3);
    /* Other 含六路舱灯：切进来再 Get 一次，对齐监控页总开关改过的 mask */
    if(sel == 1) {
        ui::DebugMsgProxy::instance().request_soc_led_state();
    }
}

/*
 * 让分段控件按内容宽度显示：ui_user_make_segmented 的段默认 flex_grow=1，
 * 需父级有固定宽度才撑得开；未设固定宽（按内容摆放）时 grow 会把段压成 0 宽
 * 导致不显示。关掉各段 grow，使其按文字宽度自适应。
 */
void seg_fit_content(lv_obj_t *seg)
{
    if(!seg) {
        return;
    }
    for(uint32_t i = 0; i < lv_obj_get_child_count(seg); i++) {
        lv_obj_set_flex_grow(lv_obj_get_child(seg, i), 0);
    }
}

/*
 * 分段控件调试页专用样式：缩小段间距，每段加蓝色边框，选中段填充蓝底白字。
 * 仅作用于本页传入的分段控件，不改动共享的 ui_user_make_segmented，避免影响其它页面。
 */
void style_debug_seg(lv_obj_t *seg)
{
    if(!seg) {
        return;
    }
    /* 浅底上的轨：淡墨条，不再用深色档的近黑半透明 */
    lv_obj_set_style_bg_color(seg, user_light_text, 0);
    lv_obj_set_style_bg_opa(seg, 13, 0);
    lv_obj_set_style_pad_column(seg, 2, 0);   /* 段间距缩小 */
    for(uint32_t i = 0; i < lv_obj_get_child_count(seg); i++) {
        lv_obj_t *b = lv_obj_get_child(seg, i);
        /* 每段蓝色边框 */
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, user_light_accent, 0);
        lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        /* 未选：深字；选中：蓝底白字 */
        lv_obj_set_style_text_color(b, user_light_text, 0);
        lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(b, user_light_accent, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    }
}

lv_obj_t *build_control_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const char *tabs[4] = { "Move", "Other", "Laser", "Test" };
    lv_obj_t *seg = ui_user_make_segmented(row, tabs, 4, 0);
    lv_obj_add_event_cb(seg, on_tab_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    seg_fit_content(seg);
    style_debug_seg(seg);

    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    make_device_toggle(row, DebugDeviceTarget::Device, "Device", LV_SYMBOL_POWER);
    make_device_toggle(row, DebugDeviceTarget::Door,   "Door",   LV_SYMBOL_DRIVE);
    make_device_toggle(row, DebugDeviceTarget::Motor,  "Motor",  LV_SYMBOL_REFRESH);
    make_device_toggle(row, DebugDeviceTarget::Light,  "Light",  LV_SYMBOL_CHARGE);
    return row;
}

/* ---- 三轴卡 ---- */
/** integer=true 时按整数显示（协议 int16 / uint8 字段，如轴步距、风扇占空比） */
lv_obj_t *make_value_input(lv_obj_t *parent, float value, bool integer = false)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    char buf[16];
    std::snprintf(buf, sizeof(buf), integer ? "%.0f" : "%.2f",
                  static_cast<double>(value));
    lv_textarea_set_text(ta, buf);
    lv_obj_set_width(ta, UI_TA_SHORT_W);
    lv_obj_set_height(ta, UI_INPUT_H);
    lv_obj_set_style_text_font(ta, user_font_text(), 0);
    lv_obj_set_style_bg_color(ta, user_light_more_bg, 0);
    /* 蓝色加粗边框 */
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, user_light_accent, 0);
    lv_obj_set_style_text_color(ta, user_light_text, 0);
    ui_keyboard_attach(ta, LV_KEYBOARD_MODE_NUMBER);
    return ta;
}

void rebuild_axis_ctrl(int idx)
{
    if(idx < 0 || idx >= 3 || !g_axis_ctrl[idx]) {
        return;
    }
    const AxisCfg &cfg = kAxes[idx];
    const bool hold = (g_axis_mode[idx] == 1);
    lv_obj_t *box = g_axis_ctrl[idx];
    /* 销毁旧输入框前先关键盘，避免 s_current_ta 悬空 */
    ui_keyboard_hide();
    lv_obj_clean(box);
    g_axis_step[idx] = nullptr;

    lv_obj_t *cap = lv_label_create(box);
    lv_label_set_text(cap, hold ? "Speed" : "Step");
    lv_obj_set_style_text_color(cap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    g_axis_step[idx] = make_value_input(box, g_axis_step_saved[idx][g_axis_mode[idx]], true);
    /* 输入框收窄，把 Speed/Step 一组整体压向左侧，给右侧放大后的 +/- < > 复位按钮留出空间 */
    lv_obj_set_width(g_axis_step[idx], UI_TA_SHORT_W * 2 / 3);

    lv_obj_t *unit = lv_label_create(box);
    lv_label_set_text(unit, hold ? cfg.speed_unit : cfg.step_unit);
    lv_obj_set_style_text_color(unit, user_light_icon_off, 0);
    lv_obj_set_style_text_font(unit, user_font_caption(), 0);

    lv_obj_t *spacer = lv_obj_create(box);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    const int32_t bsz = dbg_axis_btn_sz();
    const uint8_t a = static_cast<uint8_t>(cfg.axis);
    if(hold) {
        const char *lsym = LV_SYMBOL_LEFT, *rsym = LV_SYMBOL_RIGHT;
        lv_obj_t *bl = make_circle_btn(box, bsz, lsym, nullptr, nullptr);
        lv_obj_add_event_cb(bl, on_jog_hold, LV_EVENT_PRESSED, jog_ud(a, -1, 1));
        lv_obj_add_event_cb(bl, on_jog_hold, LV_EVENT_RELEASED, jog_ud(a, -1, 1));
        lv_obj_add_event_cb(bl, on_jog_hold, LV_EVENT_PRESS_LOST, jog_ud(a, -1, 1));
        lv_obj_t *br = make_circle_btn(box, bsz, rsym, nullptr, nullptr);
        lv_obj_add_event_cb(br, on_jog_hold, LV_EVENT_PRESSED, jog_ud(a, +1, 1));
        lv_obj_add_event_cb(br, on_jog_hold, LV_EVENT_RELEASED, jog_ud(a, +1, 1));
        lv_obj_add_event_cb(br, on_jog_hold, LV_EVENT_PRESS_LOST, jog_ud(a, +1, 1));
    } else {
        const char *lsym = cfg.rotational ? LV_SYMBOL_LEFT  : LV_SYMBOL_MINUS;
        const char *rsym = cfg.rotational ? LV_SYMBOL_RIGHT : LV_SYMBOL_PLUS;
        make_circle_btn(box, bsz, lsym, on_jog_click, jog_ud(a, -1, 0));
        make_circle_btn(box, bsz, rsym, on_jog_click, jog_ud(a, +1, 0));
    }
    if(a < 2) {   /* 仅 Z/S 卡保留归零（回 Home）按钮；D 轴删除 */
        make_circle_btn(box, bsz, LV_SYMBOL_REFRESH, on_axis_home,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(a)));
    }
}

void on_axis_subtab_changed(lv_event_t *e)
{
    lv_obj_t *seg = lv_event_get_target_obj(e);
    const int idx = static_cast<int>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if(idx < 0 || idx >= 3) {
        return;
    }
    const uint8_t new_mode = ui_user_segmented_get_selected(seg);
    if(new_mode == g_axis_mode[idx]) {
        return;
    }
    /* 切换子 tab 前保存当前模式下的输入值 */
    if(g_axis_step[idx]) {
        g_axis_step_saved[idx][g_axis_mode[idx]] =
            read_ta_float(g_axis_step[idx]);
    }
    ui_keyboard_hide();
    g_axis_mode[idx] = new_mode;
    rebuild_axis_ctrl(idx);
}

lv_obj_t *build_axis_card(lv_obj_t *parent, int idx)
{
    const AxisCfg &cfg = kAxes[idx];
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, USER_PAD_SM, 0);

    /* 头部：标题 + 位置读数 */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);

    lv_obj_t *title = lv_label_create(head);
    lv_label_set_text(title, cfg.title);
    lv_obj_set_style_text_color(title, user_light_accent, 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);

    lv_obj_t *pos = lv_label_create(head);
    lv_label_set_text_fmt(pos, "Pos 0.000 %s", cfg.unit);
    lv_obj_set_style_text_color(pos, user_light_text, 0);
    lv_obj_set_style_text_font(pos, user_font_text(), 0);
    g_axis_pos[idx] = pos;

    /* Z 轴：工作零点 */
    if(cfg.has_zero) {
        lv_obj_t *zrow = lv_obj_create(card);
        lv_obj_remove_style_all(zrow);
        lv_obj_set_width(zrow, LV_PCT(100));
        lv_obj_set_height(zrow, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(zrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(zrow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(zrow, LV_OPA_TRANSP, 0);

        g_z_zero_lbl = lv_label_create(zrow);
        lv_label_set_text(g_z_zero_lbl, "Work zero 0.000 mm");
        lv_obj_set_style_text_color(g_z_zero_lbl, user_light_icon_off, 0);
        lv_obj_set_style_text_font(g_z_zero_lbl, user_font_caption(), 0);

        lv_obj_t *zbtn = lv_button_create(zrow);
        lv_obj_remove_style_all(zbtn);
        lv_obj_set_width(zbtn, LV_SIZE_CONTENT);
        lv_obj_set_height(zbtn, USER_BTN_PRIMARY_H);   /* 调高按钮高度 */
        lv_obj_set_style_radius(zbtn, USER_RADIUS, 0);
        lv_obj_set_style_pad_hor(zbtn, USER_PAD, 0);
        /* 底色蓝，按下反馈红 */
        lv_obj_set_style_bg_color(zbtn, user_light_accent, 0);
        lv_obj_set_style_bg_opa(zbtn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(zbtn, ui_color_danger, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(zbtn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(zbtn, user_light_text, 0);
        lv_obj_add_event_cb(zbtn, on_set_zero, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(
                                static_cast<uint8_t>(cfg.axis))));
        lv_obj_t *zl = lv_label_create(zbtn);
        lv_label_set_text(zl, "Set work zero");
        lv_obj_set_style_text_font(zl, user_font_text(), 0);
        lv_obj_center(zl);
    }

    /* 子 tab：点击移动 / 长按移动 */
    const char *subs[2] = { "Click move", "Long-press" };
    lv_obj_t *seg = ui_user_make_segmented(card, subs, 2, 0);
    lv_obj_set_width(seg, LV_PCT(100));
    lv_obj_add_event_cb(seg, on_axis_subtab_changed, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(idx)));
    style_debug_seg(seg);

    /* 控制区（按子 tab 重建） */
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, USER_PAD_SM / 2, 0);   /* 缩小列间距，给按钮更多横向空间 */
    lv_obj_set_style_bg_color(box, user_light_more_bg, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(box, USER_PAD_SM, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    g_axis_ctrl[idx] = box;
    g_axis_mode[idx] = 0;
    rebuild_axis_ctrl(idx);

    return card;
}

/* ---- 振镜坐标卡 ---- */
float galvo_step()
{
    const uint8_t s = g_galvo_step_seg ? ui_user_segmented_get_selected(g_galvo_step_seg) : 0;
    return kGalvoSteps[s < 4 ? s : 0];
}

void on_galvo_dir(lv_event_t *e)
{
    const uintptr_t v = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    const auto axis = static_cast<DebugGalvoAxis>((v >> 1) & 1u);
    const int dir = (v & 1u) ? 1 : -1;
    const double delta = static_cast<double>(galvo_step()) * static_cast<double>(dir);
    if(!g_galvo_center_valid) {
        g_galvo_center_x = 0.0;
        g_galvo_center_y = 0.0;
        g_galvo_center_valid = true;
    }
    if(axis == DebugGalvoAxis::X) {
        g_galvo_center_x += delta;
    } else {
        g_galvo_center_y += delta;
    }
    apply_galvo_center_labels();
}

void on_galvo_home(lv_event_t *)
{
    /* 复位到上次从 cal 读入 / Save 成功后的偏移，不是清零 */
    g_galvo_center_x = g_galvo_saved_x;
    g_galvo_center_y = g_galvo_saved_y;
    g_galvo_center_valid = true;
    apply_galvo_center_labels();
}

void on_red_mode_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_galvo_red_light(true);
}

void on_laser_mode_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_galvo_red_light(false);
}

void on_red_always_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_galvo_red_always();
}

void on_galvo_save(lv_event_t *)
{
    if(galvo_save_busy()) {
        ui_toast_show("Cannot save while printing", user_light_notice_dot, 1800);
        update_galvo_save_enabled();
        return;
    }
    if(!g_galvo_center_valid) {
        ui_toast_show("No center offset loaded", user_light_notice_dot, 1500);
        return;
    }
    ui::DebugMsgProxy::instance().send_galvo_param_save(
        read_ta_float(g_galvo_scale_ta), galvo_step(),
        static_cast<float>(g_galvo_center_x),
        static_cast<float>(g_galvo_center_y));
    /* Save 成功后以当前编辑值为“已保存”，供复位键回退 */
    g_galvo_saved_x = g_galvo_center_x;
    g_galvo_saved_y = g_galvo_center_y;
    ui_toast_show("Saved", user_light_accent, 1200);
}

void on_test_image_changed(lv_event_t *e)
{
    lv_obj_t *seg = lv_event_get_target_obj(e);
    const bool on = (ui_user_segmented_get_selected(seg) == 0);
    ui::DebugMsgProxy::instance().send_test_image_set(on);
    /* 乐观禁用：实际态以 GLAVO_Debug_TestImage_Ntf 为准再校正 */
    update_galvo_save_enabled();
}

void on_calib_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_galvo_calib();
    ui_toast_show("Calibration started", user_light_accent, 1500);
}

void on_height_calib_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_height_calib_start(
        -5.0f, 12.0f, 1.0f, "/userdata/visual/height_lut.txt");
    if(g_height_calib_lbl) {
        lv_label_set_text(g_height_calib_lbl, "Starting...");
    }
    ui_toast_show("Height calibration started", user_light_accent, 1500);
}

/** @brief 振镜绝对坐标移动：读 Abs X/Y，发 UI_Debug_Galvo_AbsMove_Ntf。
 *  laser_on=1（运动包 LD=01 00）：Red_On 常亮时途中不关红光。 */
void on_galvo_abs_move(lv_event_t *)
{
    const float x = read_ta_float(g_galvo_abs_x_ta);
    const float y = read_ta_float(g_galvo_abs_y_ta);
    ui::DebugMsgProxy::instance().send_galvo_abs_move(x, y, 0.0f, 1);
    ui_toast_show("Galvo move sent", user_light_accent, 1200);
}

void on_galvo_point_test_click(lv_event_t *)
{
    const auto& proxy = ui::DebugMsgProxy::instance();
    const bool test_image_on =
        proxy.test_image_active() ||
        (g_test_image_seg &&
         ui_user_segmented_get_selected(g_test_image_seg) == 0);
    if(control_is_printing() || test_image_on ||
       proxy.galvo_point_test_active() || g_galvo_point_test_pending) {
        ui_toast_show("Print or test mode active",
                      user_light_notice_dot, 1800);
        return;
    }

    g_galvo_point_test_pending = true;
    update_galvo_test_controls();
    ui::DebugMsgProxy::instance().start_galvo_point_test();
    ui_toast_show("Galvo point test started", user_light_accent, 1500);
}

const char *o2_calib_err_text(uint8_t err)
{
    using E = middleware::DebugO2CalibErr;
    switch(static_cast<E>(err)) {
    case E::Ok:          return "OK";
    case E::NotReady:    return "O2 not ready";
    case E::IioFail:     return "IIO/sample fail";
    case E::IOutOfRange: return "I out of range";
    case E::KOutOfRange: return "K out of range";
    case E::Printing:    return "Print/test busy";
    case E::WriteFail:   return "Write fail";
    case E::Busy:        return "Busy";
    case E::BadRef:      return "Bad ref O2";
    default:             return "Calib fail";
    }
}

void apply_o2_calib_label(const middleware::DebugO2CalibRspPayload& p, bool from_rsp)
{
    if(!g_o2_calib_lbl) {
        return;
    }
    char buf[48];
    if(p.k > 0.f) {
        std::snprintf(buf, sizeof(buf), "K=%.1f", static_cast<double>(p.k));
    } else {
        std::snprintf(buf, sizeof(buf), "K=--");
    }
    lv_label_set_text(g_o2_calib_lbl, buf);
    lv_obj_set_style_text_color(g_o2_calib_lbl,
                                from_rsp && p.ok ? user_light_accent
                                                 : user_light_icon_off, 0);
}

void on_o2_calib_click(lv_event_t *)
{
    if(g_o2_calib_pending) {
        return;
    }
    if(control_is_printing()) {
        ui_toast_show("Cannot calib while printing",
                      user_light_notice_dot, 1800);
        return;
    }
    float ref = g_o2_calib_ref_ta ? read_ta_float(g_o2_calib_ref_ta) : 20.9f;
    if(!(ref > 0.f && ref < 100.f)) {
        ui_toast_show("Ref O2 must be 0–100%",
                      user_light_notice_dot, 1800);
        return;
    }
    g_o2_calib_pending = true;
    if(g_o2_calib_lbl) {
        lv_label_set_text(g_o2_calib_lbl, "Calibrating...");
        lv_obj_set_style_text_color(g_o2_calib_lbl, user_light_accent, 0);
    }
    if(g_o2_calib_btn) {
        lv_obj_clear_flag(g_o2_calib_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(g_o2_calib_btn, LV_STATE_DISABLED);
    }
    ui::DebugMsgProxy::instance().send_o2_calib(ref);
    ui_toast_show("O2 calib started", user_light_accent, 1200);
}

lv_obj_t *make_galvo_readout(lv_obj_t *parent, const char *axis_name, lv_obj_t **out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);

    lv_obj_t *n = lv_label_create(row);
    lv_label_set_text(n, axis_name);
    lv_obj_set_style_text_color(n, user_light_icon_off, 0);
    lv_obj_set_style_text_font(n, user_font_text(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "0.000");
    lv_obj_set_style_text_color(v, user_light_text, 0);
    lv_obj_set_style_text_font(v, user_font_text(), 0);
    if(out) *out = v;
    return row;
}

lv_obj_t *build_dpad(lv_obj_t *parent)
{
    const int32_t bsz = dbg_round_btn_sz();
    lv_obj_t *pad = lv_obj_create(parent);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(pad, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    auto make_dir = [&](lv_obj_t *p, const char *sym, uint8_t axis, int dir) {
        const uintptr_t v = (static_cast<uintptr_t>(axis) << 1) | (dir > 0 ? 1u : 0u);
        return make_circle_btn(p, bsz, sym, on_galvo_dir, reinterpret_cast<void *>(v));
    };
    auto make_mid_row = [&]() {
        lv_obj_t *r = lv_obj_create(pad);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(r, USER_PAD_SM, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        return r;
    };

    /* 上 +Y：本地增减中心偏移，Save 后才写 cal.csv */
    make_dir(pad, "Y", static_cast<uint8_t>(DebugGalvoAxis::Y), +1);
    /* 中：-X 复位(回上次保存) +X */
    lv_obj_t *mid = make_mid_row();
    make_dir(mid, "-X", static_cast<uint8_t>(DebugGalvoAxis::X), -1);
    make_circle_btn(mid, bsz, LV_SYMBOL_REFRESH, on_galvo_home, nullptr);
    make_dir(mid, "X", static_cast<uint8_t>(DebugGalvoAxis::X), +1);
    /* 下 -Y */
    make_dir(pad, "-Y", static_cast<uint8_t>(DebugGalvoAxis::Y), -1);
    return pad;
}

lv_obj_t *build_galvo_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, USER_PAD_SM, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Galvo Coordinate");
    lv_obj_set_style_text_color(title, user_light_accent, 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);

    /* 主行：左信息 + 右方向盘 */
    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = lv_obj_create(body);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    make_galvo_readout(left, "X", &g_galvo_x_lbl);
    make_galvo_readout(left, "Y", &g_galvo_y_lbl);
    apply_galvo_center_labels();

    build_dpad(body);

    /* Scale / Test image：整行（卡片直接子，避免窄列裁剪） */
    lv_obj_t *scale_row = lv_obj_create(card);
    lv_obj_remove_style_all(scale_row);
    lv_obj_set_width(scale_row, LV_PCT(100));
    lv_obj_set_height(scale_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(scale_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scale_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(scale_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(scale_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scale_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *scap = lv_label_create(scale_row);
    lv_label_set_text(scap, "Scale");
    lv_obj_set_style_text_color(scap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(scap, user_font_caption(), 0);
    g_galvo_scale_ta = make_value_input(scale_row, 0.01f);

    lv_obj_t *rl_row = lv_obj_create(card);
    lv_obj_remove_style_all(rl_row);
    lv_obj_set_width(rl_row, LV_PCT(100));
    lv_obj_set_height(rl_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rl_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rl_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(rl_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *rlcap = lv_label_create(rl_row);
    lv_label_set_text(rlcap, "Test image");
    lv_obj_set_style_text_color(rlcap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(rlcap, user_font_caption(), 0);
    const char *onoff[2] = { "On", "Off" };
    g_test_image_seg = ui_user_make_segmented(rl_row, onoff, 2, 1);
    lv_obj_add_event_cb(g_test_image_seg, on_test_image_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    seg_fit_content(g_test_image_seg);
    style_debug_seg(g_test_image_seg);

    /* 绝对坐标移动行：X / Y 输入框 + Move（snowx 手动振镜，与测高同一条 TCP 包） */
    lv_obj_t *abs_row = lv_obj_create(card);
    lv_obj_remove_style_all(abs_row);
    lv_obj_set_width(abs_row, LV_PCT(100));
    lv_obj_set_height(abs_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(abs_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(abs_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(abs_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(abs_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(abs_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *axcap = lv_label_create(abs_row);
    lv_label_set_text(axcap, "Abs X");
    lv_obj_set_style_text_color(axcap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(axcap, user_font_caption(), 0);
    g_galvo_abs_x_ta = make_value_input(abs_row, 0.0f);
    lv_obj_t *aycap = lv_label_create(abs_row);
    lv_label_set_text(aycap, "Y");
    lv_obj_set_style_text_color(aycap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(aycap, user_font_caption(), 0);
    g_galvo_abs_y_ta = make_value_input(abs_row, 0.0f);
    lv_obj_t *abs_spacer = lv_obj_create(abs_row);
    lv_obj_remove_style_all(abs_spacer);
    lv_obj_set_height(abs_spacer, 1);
    lv_obj_set_flex_grow(abs_spacer, 1);
    lv_obj_set_style_bg_opa(abs_spacer, LV_OPA_TRANSP, 0);
    lv_obj_t *move_btn = lv_button_create(abs_row);
    lv_obj_add_style(move_btn, user_style_btn_primary(), 0);
    lv_obj_set_size(move_btn, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
    lv_obj_add_event_cb(move_btn, on_galvo_abs_move, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ml = lv_label_create(move_btn);
    lv_label_set_text(ml, "Move");
    lv_obj_center(ml);

    /* 底行：步长 + 保存 */
    lv_obj_t *foot = lv_obj_create(card);
    lv_obj_remove_style_all(foot);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);

    lv_obj_t *stepcap = lv_label_create(foot);
    lv_label_set_text(stepcap, "Step(mm)");
    lv_obj_set_style_text_color(stepcap, user_light_icon_off, 0);
    lv_obj_set_style_text_font(stepcap, user_font_caption(), 0);
    const char *steps[4] = { "0.01", "0.1", "1", "10" };
    g_galvo_step_seg = ui_user_make_segmented(foot, steps, 4, 2);
    seg_fit_content(g_galvo_step_seg);
    style_debug_seg(g_galvo_step_seg);

    lv_obj_t *spacer = lv_obj_create(foot);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    lv_obj_t *save = lv_button_create(foot);
    lv_obj_add_style(save, user_style_btn_primary(), 0);
    lv_obj_set_size(save, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
    lv_obj_add_event_cb(save, on_galvo_save, LV_EVENT_CLICKED, nullptr);
    g_galvo_save_btn = save;
    update_galvo_save_enabled();
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save");
    lv_obj_center(sl);
    return card;
}

lv_obj_t *build_calib_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *info = lv_obj_create(card);
    lv_obj_remove_style_all(info);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);

    lv_obj_t *title = lv_label_create(info);
    lv_label_set_text(title, "Galvo Calibration");
    lv_obj_set_style_text_color(title, user_light_accent, 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);
    lv_obj_t *hint = lv_label_create(info);
    lv_label_set_text(hint, "Insert photo paper and close the door first");
    lv_obj_set_style_text_color(hint, user_light_icon_off, 0);
    lv_obj_set_style_text_font(hint, user_font_caption(), 0);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    lv_obj_set_style_bg_color(btn, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_add_event_cb(btn, on_calib_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Start calibration");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);
    return card;
}

lv_obj_t *build_height_calib_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *info = lv_obj_create(card);
    lv_obj_remove_style_all(info);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);

    lv_obj_t *title = lv_label_create(info);
    lv_label_set_text(title, "Height Calibration");
    lv_obj_set_style_text_color(title, user_light_accent, 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);
    g_height_calib_lbl = lv_label_create(info);
    lv_label_set_text(g_height_calib_lbl, "Idle");
    lv_obj_set_style_text_color(g_height_calib_lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(g_height_calib_lbl, user_font_caption(), 0);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    lv_obj_set_style_bg_color(btn, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_add_event_cb(btn, on_height_calib_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Start");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);
    return card;
}

lv_obj_t *make_scroll_col(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    return col;
}

/* ============================ Tab2 其他控制 ============================ */

/** 风扇占空比读数（限幅 0–100） */
float read_fan_duty()
{
    float d = g_fan_duty_ta ? read_ta_float(g_fan_duty_ta) : 0.0f;
    if(d < 0.0f)   d = 0.0f;
    if(d > 100.0f) d = 100.0f;
    return d;
}

/* 开关控件上下文：点击即本地翻转状态字 + 发命令（后端待实现，无害发送） */
struct DbgToggleCtx {
    lv_obj_t   *state_lbl;
    const char *on_txt;
    const char *off_txt;
    uint8_t     kind;   /**< 0 粉桶 / 1 推粉 / 2 风扇 / 3 其他开关 / 4 清粉 / 5 打印H264 / 6 清粉录像 / 7 环境模拟 / 8 环境模拟缺粉 / 9 转运盒流程 / 10 首层视觉检测 / 11 消息注入 / 12 报警注入 / 13 本地RTSP / 14 光路检测 / 15 基板测高 / 16 SOC舱灯 / 17 舱内PM阀 / 18 跳过氧门 / 19 Sys HUD / 20 SPI小屏测试 / 21 跳过外门 / 22 跳过内门视觉挡门 / 23 跳过基板在位读码 / 24 跳过放版预确认 */
    uint8_t     arg;    /**< 粉桶号 / DebugSwitchTarget / SOC LED channel */
};

/** 消息注入按钮造的那几条：一条 critical + 两条 info + 一条本地调试码。 */
const uint16_t kDbgInjectCodes[] = {
    (uint16_t)middleware::DeviceEventCode::O2High,
    (uint16_t)middleware::DeviceEventCode::PowderLow,
    (uint16_t)middleware::DeviceEventCode::FilterDue,
    (uint16_t)middleware::DeviceEventCode::UiDebugSample,
};

/** 报警注入按钮造的那几条 MCU 报警，覆盖各条显示路径：
 *  1124 阻断 CONDITION（error/ACTIVE/RESET/LOCAL）→ 打印入口拦截 + Reset 三态；
 *       LOCAL 类在 ACTIVE 时该置灰，与下面 1003 对照着看；
 *  1104 RTN_LATCHED（warning/RESET）→ 消息页 RESOLVED 药丸 + 可复位；
 *  1101 自动清除 CONDITION（warning/ACTIVE/AUTO）→ 无 Reset 钮的红卡；
 *  2000 EVENT（info/NONE）→ 蓝卡 5s 自动从浮层撤下、历史留档。
 *  1003 critical CONDITION（ACTIVE/RESET/OWNER_TASK）→ 凑齐四档等级配色（深红
 *       那档），同时验 ACTIVE + OWNER_TASK 在抽屉里可点 Reset。
 *  1999 **字典外**的未知码（error/ACTIVE/RESET）→ 验固件比字典新时的降级路径：
 *       标题降级成 "Unknown alarm #1999"，且 Reset 仍**可点**
 *       （ui_alarm_reset_allowed 查不到字典按放行处理）。刻意选 1999 是因为它在
 *       快照管辖范围（1000..1395）之外，30s 兜底轮询的反向清理不会把它抹掉。
 *  等级不进字典、以报文为准，所以四色能不能看全全看造什么数据。
 *  关掉开关注入 NORMAL 帧模拟解除（EVENT 无需解除）。 */
struct DbgAlarmInject {
    uint16_t code;
    uint8_t  severity;
    uint8_t  state_mask;
    uint8_t  clear_mode;
};
const DbgAlarmInject kDbgInjectAlarms[] = {
    { 1124, 3, middleware::kAlarmStateActive | middleware::kAlarmStateLatched,
      (uint8_t)middleware::AlarmClearMode::Reset },
    { 1104, 2, middleware::kAlarmStateLatched,
      (uint8_t)middleware::AlarmClearMode::Reset },
    { 1101, 2, middleware::kAlarmStateActive,
      (uint8_t)middleware::AlarmClearMode::Auto },
    /* EVENT 按报警表恒为 NONE；造 AUTO 会踩中 apply_report_async 里那条
     * "固件行为与表不符" 的告警日志，联调时是干扰项 */
    { 2000, 1, middleware::kAlarmStateEvent,
      (uint8_t)middleware::AlarmClearMode::None },
    { 1003, 4, middleware::kAlarmStateActive,
      (uint8_t)middleware::AlarmClearMode::Reset },
    { 1999, 3, middleware::kAlarmStateActive,
      (uint8_t)middleware::AlarmClearMode::Reset },
};

void set_dbg_toggle_enabled(lv_obj_t *btn, bool enabled)
{
    if(!btn) {
        return;
    }
    if(enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

void on_dbg_toggle_delete(lv_event_t *e)
{
    delete static_cast<DbgToggleCtx *>(lv_event_get_user_data(e));
}

void on_dbg_toggle(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    auto *c = static_cast<DbgToggleCtx *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state(btn, LV_STATE_CHECKED);
    if(c->state_lbl) {
        lv_label_set_text(c->state_lbl, on ? c->on_txt : c->off_txt);
        lv_obj_set_style_text_color(c->state_lbl,
                                    on ? user_light_accent : user_light_icon_off, 0);
    }
    auto &proxy = ui::DebugMsgProxy::instance();
    switch(c->kind) {
        //case 0: proxy.send_powder_bucket(c->arg, on); break;  // 粉桶1/2功能暂时屏蔽
        case 2: proxy.send_fan(on); break;   /* 风扇开/关；占空比由 Send 按钮单独下发 */
        case 3: proxy.send_switch(static_cast<DebugSwitchTarget>(c->arg), on); break;
        case 4: if(on) proxy.send_clean_test_start();          /* 开：启动清粉测试 */
                else   proxy.send_clean_test_close();          /* 关：中止清粉 A2/42 TLV 0x02 */
                break;
        case 5: proxy.send_print_h264_record_set(on);          /* 打印期连续 H264 录像总开关（仅本次运行） */
                break;
        case 6: proxy.send_clean_record_set(on);               /* 清粉录像总开关（仅本次运行） */
                break;
        case 7: proxy.send_env_sim_set(on);                    /* 环境上报模拟（仅本次运行，供 MQTT 联调） */
                break;
        case 8: proxy.send_env_sim_powder_set(on);             /* 环境模拟缺粉（延迟 5s 生效） */
                break;
        /* 取件流程是否插入转运盒三步。纯 UI 本地标志，不下发任何命令（仅本次运行）。 */
        case 9: control_set_transfer_box_flow(on);
                break;
        /* 打印入口跳过「是否已放入打印版」预确认，直接进选文件页。纯 UI 本地标志（仅本次运行）。 */
        case 24: control_set_skip_plate_prompt(on);
                 break;
        case 10: proxy.send_first_layer_visual_check_set(on);  /* 首层视觉检测（仅本次运行） */
                 break;
        case 14: proxy.send_optical_path_check_set(!on);      /* Skip Optical Path Chk：按下跳过光路；载荷 on=1 仍表示执行（默认执行） */
                 break;
        case 15: proxy.send_plate_height_adj_set(on);          /* 打印前基板测高调 Z（仅本次运行，默认开） */
                 break;
        case 22: proxy.send_inner_door_occlusion_set(!on);    /* Skip Inner Door Occl：按下跳过挡门融合；载荷 on=1 仍表示执行（默认执行） */
                 break;
        case 23: proxy.send_plate_present_set(!on);            /* Skip Plate Present：按下跳过读码；载荷 on=1 仍表示执行（默认执行） */
                 break;
        /* SOC 六路舱灯 / 舱内 PM 阀：sysfs 直控，不看 MCU 版本 */
        case 16: proxy.send_soc_led_set(c->arg, on);
                 break;
        case 17: proxy.send_cabin_pm_valve_set(on);
                 break;
        case 18: proxy.send_skip_o2_gate_set(on);             /* 跳过氧门+打印前洗气（仅本次运行，默认关） */
                 break;
        case 21: proxy.send_skip_outer_door_set(on);          /* 跳过外门检测+关内门套件（仅本次运行，默认关=要做） */
                 break;
        /* 造几条假消息喂通知浮层。Glavo 侧还没实现 GLAVO_Device_Event_Ntf，
         * 没有这个入口就只有打印流程那两条能看，「查看剩余 N 条」那条路径根本走不到。
         * 四条 = 两张卡 + 一颗「剩余 2 条」按钮，顺带把红/蓝两种卡和排序都覆盖上。 */
        case 11: for(uint16_t code : kDbgInjectCodes) {
                     /* 走 debug_push：文案取自码表但带 [SIM] 前缀，与真事件区分 */
                     if(on) ui_message_debug_push(code);
                     else   ui_message_retract(code);
                 }
                 break;
        /* 造几条假 MCU 报警走真实上报路径（ui_alarm_state），MCU 不发真报警时
         * 验消息页/浮层/打印拦截整条链路。关掉注入 NORMAL 帧模拟解除。
         * 同时经 Glavo 只向 MQTT 发 GLAVO_Alarm_Report_Ntf，供云端 alarm.single 联调；
         * 不上 UI，避免与本地 [SIM] 双弹。 */
        case 12: for(const auto &a : kDbgInjectAlarms) {
                     middleware::AlarmReportPayload p{};
                     p.alarm_code = a.code;
                     p.severity   = a.severity;
                     p.clear_mode = a.clear_mode;
                     if(a.state_mask & middleware::kAlarmStateEvent) {
                         if(on) {
                             ui_alarm_debug_inject(a.code, a.severity,
                                                   a.state_mask, a.clear_mode, 0);
                             p.state_mask = a.state_mask;
                             proxy.send_alarm_inject(p);
                         }
                     } else {
                         ui_alarm_debug_inject(a.code, a.severity,
                                               on ? a.state_mask : 0,
                                               a.clear_mode, 0);
                         p.state_mask = on ? a.state_mask : 0;
                         proxy.send_alarm_inject(p);
                     }
                 }
                 break;
        /* 本地局域网 RTSP Server（→Media，仅本次运行；上电默认开启）。相机流未就绪时
         * Media 会回 Off，register_local_rtsp 的回调据此把按钮弹回。 */
        case 13: proxy.send_local_rtsp_set(on);
                 break;
        /* 右上角红字 Host 资源蒙版（本机 /proc，纯 UI，仅本次运行）。 */
        case 19: ui_sys_hud_set_enabled(on);
                 break;
        case 20: proxy.send_spi_pm25_lcd_test_set(on); /* SPI 小屏周期刷测试色 */
                 break;
        default: break;
    }
}

/** 名称+状态字 + 圆形开关按钮（CHECKABLE）；vertical=true 竖排(标签在上)，false 横排(标签在左)。
 * btn_sz<=0 用默认放大尺寸 dbg_round_btn_sz()；需要更小的按钮（如 Fan 那种横排紧凑行）可显式传入。 */
lv_obj_t *make_dbg_toggle(lv_obj_t *parent, const char *name,
                          const char *on_txt, const char *off_txt,
                          const char *sym, bool initial_on,
                          uint8_t kind, uint8_t arg, bool vertical = true,
                          int32_t btn_sz = 0,
                          lv_obj_t **out_btn = nullptr,
                          lv_obj_t **out_state = nullptr)
{
    const int32_t toggle_btn_sz = (btn_sz > 0) ? btn_sz : dbg_round_btn_sz();
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, vertical ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if(vertical) {
        lv_obj_set_style_pad_row(grp, USER_PAD_SM / 2, 0);
    } else {
        lv_obj_set_style_pad_column(grp, USER_PAD_SM, 0);
    }
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    /* 竖排：名称/状态 直接进 grp；横排：套一个纵向 col 放名称/状态，按钮在右 */
    lv_obj_t *lblbox = grp;
    if(!vertical) {
        lblbox = lv_obj_create(grp);
        lv_obj_remove_style_all(lblbox);
        lv_obj_set_size(lblbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(lblbox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(lblbox, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_bg_opa(lblbox, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(lblbox, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *lbl = lv_label_create(lblbox);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);

    lv_obj_t *state = lv_label_create(lblbox);
    lv_label_set_text(state, initial_on ? on_txt : off_txt);
    lv_obj_set_style_text_color(state,
                                initial_on ? user_light_accent : user_light_icon_off, 0);
    lv_obj_set_style_text_font(state, user_font_text(), 0);

    lv_obj_t *btn = lv_button_create(grp);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, toggle_btn_sz, toggle_btn_sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, user_light_accent, 0);
    lv_obj_set_style_bg_color(btn, user_light_text, 0);
    lv_obj_set_style_bg_opa(btn, 13, 0);
    lv_obj_set_style_bg_color(btn, user_light_accent, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_light_accent, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, user_light_accent, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, user_light_accent, LV_STATE_PRESSED);
    if(vertical) {
        lv_obj_set_style_margin_top(btn, USER_PAD_SM, 0);   /* 竖排时按钮与标签拉开间距 */
    }
    if(initial_on) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    auto *ctx = new DbgToggleCtx{ state, on_txt, off_txt, kind, arg };
    lv_obj_add_event_cb(btn, on_dbg_toggle, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(btn, on_dbg_toggle_delete, LV_EVENT_DELETE, ctx);
    if(out_btn) {
        *out_btn = btn;
    }
    if(out_state) {
        *out_state = state;
    }

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, sym);
    /* 选中蓝底时偏白更清晰；未选中浅墨底用深字。LVGL 子控件不继承父 CHECKED，
     * 这里在 VALUE_CHANGED 里同步太重，统一用深字——蓝底上仍可读。 */
    lv_obj_set_style_text_color(ic, user_light_text, 0);
    lv_obj_set_style_text_font(ic, user_font_icon(), 0);
    lv_obj_center(ic);
    return grp;
}

/* 点动电机类控件（筛粉电机 / 内门 / 回粉电机）：3 个点动键 [< 停止▶ >] + 共享状态字。
 * device：0=筛粉电机 1=内门 2=回粉电机。action 与 DebugMotorAction 对齐：
 * 0=停止 1=正转/开/送往 B 瓶 2=反转/关/送往 A 瓶。 */
struct MomentBtnCtx {
    lv_obj_t *state_lbl;
    uint8_t   device;
    uint8_t   action;
};

void on_moment_btn_delete(lv_event_t *e)
{
    delete static_cast<MomentBtnCtx *>(lv_event_get_user_data(e));
}

void apply_inner_door_debug_busy(bool busy)
{
    g_inner_door_busy = busy;
    for(lv_obj_t *b : g_inner_door_btns) {
        set_dbg_toggle_enabled(b, !busy);
    }
    if(g_inner_door_state_lbl) {
        if(busy) {
            lv_label_set_text(g_inner_door_state_lbl, "Moving");
            lv_obj_set_style_text_color(g_inner_door_state_lbl, user_light_accent, 0);
        } else {
            lv_label_set_text(g_inner_door_state_lbl, "Idle");
            lv_obj_set_style_text_color(g_inner_door_state_lbl, user_light_icon_off, 0);
        }
    }
}

/* 点动：按下即发对应帧，并把该块状态字更新为运行/停止文案 */
void on_moment_btn(lv_event_t *e)
{
    auto *c = static_cast<MomentBtnCtx *>(lv_event_get_user_data(e));
    if(!c) {
        return;
    }
    const auto act = static_cast<DebugMotorAction>(c->action);
    const char *txt;
    if(c->device == 0) {          /* 筛粉电机：正转0x13 / 停止0x15 / 反转0x14 */
        ui::DebugMsgProxy::instance().send_sieve(act);
        txt = (act == DebugMotorAction::Forward) ? "Running"
            : (act == DebugMotorAction::Reverse) ? "Reversing" : "Stopped";
    } else if(c->device == 2) {   /* 回粉电机：送往 B 瓶0x17 / 停止并恢复自动0x18 / 送往 A 瓶0x16 */
        ui::DebugMsgProxy::instance().send_return_motor(act);
        txt = (act == DebugMotorAction::Forward) ? "To B"
            : (act == DebugMotorAction::Reverse) ? "To A" : "Auto";
    } else {                      /* 内门：开0x0C / 停止0x0E / 关0x0D */
        if(g_inner_door_busy) {
            return;
        }
        if(act == DebugMotorAction::Stop) {
            ui::DebugMsgProxy::instance().send_inner_door(act);
            txt = "Stopped";
        } else {
            apply_inner_door_debug_busy(true);
            ui::DebugMsgProxy::instance().send_inner_door(act);
            txt = "Moving";
        }
    }
    if(c->state_lbl) {
        lv_label_set_text(c->state_lbl, txt);
        lv_obj_set_style_text_color(c->state_lbl,
            (act != DebugMotorAction::Stop) ? user_light_accent : user_light_icon_off, 0);
    }
}

/** 点动键：sym 图标，device+action 编码进 ctx；点动即发帧并刷新状态字 */
lv_obj_t *make_moment_btn(lv_obj_t *parent, const char *sym,
                          uint8_t device, DebugMotorAction action, lv_obj_t *state_lbl)
{
    auto *ctx = new MomentBtnCtx{ state_lbl, device, static_cast<uint8_t>(action) };
    lv_obj_t *b = make_circle_btn(parent, dbg_round_btn_sz(), sym, on_moment_btn, ctx);
    lv_obj_add_event_cb(b, on_moment_btn_delete, LV_EVENT_DELETE, ctx);
    return b;
}

/* 点动电机单元：名称+状态在上，[< 停止▶ >] 三键在下一行（与标签留出间距）。
 * 左 < = 反转/关/送往 A 瓶，中 ▶ = 停止，右 > = 正转/开/送往 B 瓶。
 * 回粉电机(device=2) 初始状态为 Auto，并在三键下方加 To A / Stop·Auto / To B 小字说明。 */
void build_moment_motor_controls(lv_obj_t *parent, const char *name, uint8_t device)
{
    const int32_t btn_sz = dbg_round_btn_sz();
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grp, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(grp);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, user_light_icon_off, 0);
    lv_obj_set_style_text_font(nm, user_font_caption(), 0);
    lv_obj_t *st = lv_label_create(grp);
    lv_label_set_text(st, (device == 2) ? "Auto" : "Stopped");
    lv_obj_set_style_text_color(st, user_light_icon_off, 0);
    lv_obj_set_style_text_font(st, user_font_text(), 0);

    lv_obj_t *btnrow = lv_obj_create(grp);
    lv_obj_remove_style_all(btnrow);
    lv_obj_set_size(btnrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btnrow, USER_PAD_SM, 0);
    lv_obj_set_style_margin_top(btnrow, USER_PAD_SM, 0);   /* 与标签拉开间距 */
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btnrow, LV_OBJ_FLAG_SCROLLABLE);

    /* 左 < 反转/关，中 ▶ 停止，右 > 正转/开 */
    lv_obj_t *b_rev = make_moment_btn(btnrow, LV_SYMBOL_LEFT,  device, DebugMotorAction::Reverse, st);
    lv_obj_t *b_stop = make_moment_btn(btnrow, LV_SYMBOL_PLAY,  device, DebugMotorAction::Stop,    st);
    lv_obj_t *b_fwd = make_moment_btn(btnrow, LV_SYMBOL_RIGHT, device, DebugMotorAction::Forward, st);
    if(device == 1) {
        g_inner_door_btns[0] = b_rev;
        g_inner_door_btns[1] = b_stop;
        g_inner_door_btns[2] = b_fwd;
        g_inner_door_state_lbl = st;
    }

    if(device == 2) {
        /* 每格与圆钮同宽、同列距，保证小字正对各自按钮 */
        lv_obj_t *caprow = lv_obj_create(grp);
        lv_obj_remove_style_all(caprow);
        lv_obj_set_size(caprow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(caprow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(caprow, USER_PAD_SM, 0);
        lv_obj_set_style_bg_opa(caprow, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(caprow, LV_OBJ_FLAG_SCROLLABLE);
        const char *const kCaps[3] = { "To A", "Stop/Auto", "To B" };
        for(const char *cap : kCaps) {
            lv_obj_t *l = lv_label_create(caprow);
            lv_label_set_text(l, cap);
            lv_obj_set_width(l, btn_sz);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(l, user_light_icon_off, 0);
            lv_obj_set_style_text_font(l, user_font_caption(), 0);
        }
    }
}

/** 一行卡片容器（横向 flex，等高） */
lv_obj_t *make_card_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, USER_PAD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* ---- 小工具：卡片标题 / 暗色小标签 / 文本框整数读数 ---- */
void add_card_title(lv_obj_t *card, const char *title)
{
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, user_light_accent, 0);
    lv_obj_set_style_text_font(t, user_font_text(), 0);
}

void add_caption(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, user_light_icon_off, 0);
    lv_obj_set_style_text_font(l, user_font_caption(), 0);
}

uint16_t read_u16_ta(lv_obj_t *ta)
{
    float f = read_ta_float(ta);
    if(f < 0.0f)      f = 0.0f;
    if(f > 65535.0f)  f = 65535.0f;
    return static_cast<uint16_t>(f + 0.5f);
}

/* ---- Fan：Send 只发占空比；开关只发风机开/关 ---- */
void on_fan_send_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_fan_duty(read_fan_duty());
}

/* ---- Print Params：Send 下发层厚 + 平衡系数×1000 ---- */
void on_print_param_send(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_print_layer_param(
        read_u16_ta(g_print_layer_ta), read_u16_ta(g_print_balance_ta));
}

/** 整宽主按钮（圆角矩形，蓝底） */
lv_obj_t *make_send_button(lv_obj_t *parent, lv_event_cb_t cb, bool full_width)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, user_style_btn_primary(), 0);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    if(full_width) {
        lv_obj_set_width(btn, LV_PCT(100));
    } else {
        lv_obj_set_width(btn, USER_BTN_PRIMARY_W);
    }
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, "Send");
    lv_obj_center(l);
    return btn;
}

/* 打印参数卡：层厚 + 平衡系数 两个整数输入 + 整宽 Send */
void build_print_params_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);   /* 按内容高度，避免 Send 被裁 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    add_card_title(card, "Print Params");

    /* 标签与输入框同一行，压缩纵向高度 */
    auto make_labeled_input = [&](const char *cap_txt, float val) -> lv_obj_t * {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        add_caption(row, cap_txt);
        lv_obj_t *ta = make_value_input(row, val, true);
        lv_obj_set_flex_grow(ta, 1);
        return ta;
    };
    g_print_layer_ta   = make_labeled_input("Layer (um)", 50.0f);
    g_print_balance_ta = make_labeled_input("Balance", 1000.0f);

    make_send_button(card, on_print_param_send, true);
}

/* 风机故障码 / 状态机文字，取自 doc/主控板串口通信协议 - 传感器上报命令.csv FANBOARD */
const char *const kFanFaultNames[10] = {
    "OK", "Hall Invalid", "Hall Disorder", "Stalled",
    "Overcurrent", "Undervoltage", "Overvoltage", "Overspeed",
    "Board Overheat", "NTC Fault" };
const char *const kFanStateNames[6] = {
    "Stopped", "Starting", "Running", "Retry Wait", "Fault Lock", "Precharge" };

/**
 * 刷风扇实测：转速与 Duty 同行（便于对照下发值），状态/故障单独一行。
 * 链路异常(valid=0)时 MCU 把转速填 0，必须显示 "--"，否则 0 会被当成"风机停了"。
 * 有故障码时两个 label 一起标红——只红一个看着像单独某项出了问题。
 */
void apply_fan_state(const middleware::DebugFanStatePayload &p)
{
    if(!g_fan_rpm_lbl || !g_fan_state_lbl) {
        return;
    }
    if(p.valid == 0) {
        lv_label_set_text(g_fan_rpm_lbl, "--");
        lv_label_set_text(g_fan_state_lbl, "--");
        lv_obj_set_style_text_color(g_fan_rpm_lbl, user_light_icon_off, 0);
        lv_obj_set_style_text_color(g_fan_state_lbl, user_light_icon_off, 0);
        return;
    }
    const bool bad = (p.fault != 0);
    char buf[8];    /* rpm 为 uint16_t，最大 65535 共 5 位 */
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(p.rpm));
    lv_label_set_text(g_fan_rpm_lbl, buf);
    lv_label_set_text(g_fan_state_lbl,
                      bad ? ((p.fault < 10) ? kFanFaultNames[p.fault] : "Unknown")
                          : ((p.state < 6) ? kFanStateNames[p.state] : "Unknown"));
    const lv_color_t c = bad ? user_light_notice_dot : user_light_text;
    lv_obj_set_style_text_color(g_fan_rpm_lbl, c, 0);
    lv_obj_set_style_text_color(g_fan_state_lbl, c, 0);
}

/* 风扇卡：Duty+实测 RPM 行 + 风机状态行 + Send(发占空比) 与 开关，上下排列 */
void build_fan_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);   /* 按内容高度，避免 Send/开关被裁 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    add_card_title(card, "Fan");

    /* Duty 行 */
    lv_obj_t *drow = lv_obj_create(card);
    lv_obj_remove_style_all(drow);
    lv_obj_set_width(drow, LV_PCT(100));
    lv_obj_set_height(drow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(drow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(drow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(drow, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(drow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(drow, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(drow, "Duty");
    g_fan_duty_ta = make_value_input(drow, 0.0f, true);
    add_caption(drow, "%");
    /* 实测转速紧跟下发占空比：两个数并排才看得出"发了 60% 却只有 200 转" */
    add_caption(drow, "RPM");
    g_fan_rpm_lbl = lv_label_create(drow);
    lv_label_set_text(g_fan_rpm_lbl, "--");
    lv_obj_set_style_text_color(g_fan_rpm_lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(g_fan_rpm_lbl, user_font_text(), 0);

    /* 状态行（只读）：MCU V4 EXT 帧约 1s 一次；旧固件不发该帧，恒 "--" */
    lv_obj_t *rrow = lv_obj_create(card);
    lv_obj_remove_style_all(rrow);
    lv_obj_set_width(rrow, LV_PCT(100));
    lv_obj_set_height(rrow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rrow, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(rrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rrow, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(rrow, "State");
    g_fan_state_lbl = lv_label_create(rrow);
    lv_label_set_text(g_fan_state_lbl, "--");
    lv_obj_set_style_text_color(g_fan_state_lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(g_fan_state_lbl, user_font_text(), 0);

    /* Send（发占空比）与 开关 并排：Send 左，开关右 */
    lv_obj_t *frow = lv_obj_create(card);
    lv_obj_remove_style_all(frow);
    lv_obj_set_width(frow, LV_PCT(100));
    lv_obj_set_height(frow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(frow, USER_PAD, 0);
    lv_obj_set_style_bg_opa(frow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(frow, LV_OBJ_FLAG_SCROLLABLE);
    make_send_button(frow, on_fan_send_click, false);
    /* Fan 开关与 Send 同一横排，空间紧张：用原始（较小）尺寸避免显示不全 */
    make_dbg_toggle(frow, "Fan", "On", "Off", LV_SYMBOL_POWER, false, 2, 0, false,
                    USER_BTN_DOOR_ICON_SZ);
}

/* 网格行：横向 flex、均匀分布。高度按内容自适应（不再 flex_grow 撑满），
 * 便于所在容器在纵向放不下时滚动显示。 */
lv_obj_t *make_switch_grid_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/** 分类小卡：标题 + 内容列；高度随内容，由外层滚动。 */
lv_obj_t *make_section_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    add_card_title(card, title);
    return card;
}

/** 三个粉料弹窗的手动触发，档位由 user_data 带过来。见 build_other_left 的 Inject 组。 */
void on_supply_dlg_click(lv_event_t *e)
{
    const auto kind = static_cast<control_supply_dlg_t>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    control_show_supply_dialog(kind);
}

/** 一次性动作：名称 + 状态字 + 圆钮（非 CHECKABLE）。
 *  ud 透传给点击回调，供同一回调服务多个按钮时区分来源。 */
lv_obj_t *make_one_shot_action(lv_obj_t *parent, const char *name,
                               const char *idle_txt, const char *sym,
                               lv_event_cb_t cb, lv_obj_t **out_btn,
                               lv_obj_t **out_lbl, void *ud = nullptr)
{
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grp, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(grp);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, user_light_icon_off, 0);
    lv_obj_set_style_text_font(nm, user_font_caption(), 0);

    lv_obj_t *st = lv_label_create(grp);
    lv_label_set_text(st, idle_txt);
    lv_obj_set_style_text_color(st, user_light_icon_off, 0);
    lv_obj_set_style_text_font(st, user_font_text(), 0);

    lv_obj_t *btn = make_circle_btn(grp, dbg_round_btn_sz(), sym, cb, ud);
    lv_obj_set_style_margin_top(btn, USER_PAD_SM, 0);
    if(out_btn) {
        *out_btn = btn;
    }
    if(out_lbl) {
        *out_lbl = st;
    }
    return grp;
}

void build_o2_calib_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    add_card_title(card, "O2 Calib");

    lv_obj_t *rrow = lv_obj_create(card);
    lv_obj_remove_style_all(rrow);
    lv_obj_set_width(rrow, LV_PCT(100));
    lv_obj_set_height(rrow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rrow, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(rrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(rrow, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(rrow, "Ref %");
    g_o2_calib_ref_ta = make_value_input(rrow, 20.9f, false);
    lv_obj_set_flex_grow(g_o2_calib_ref_ta, 1);

    lv_obj_t *krow = lv_obj_create(card);
    lv_obj_remove_style_all(krow);
    lv_obj_set_width(krow, LV_PCT(100));
    lv_obj_set_height(krow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(krow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(krow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(krow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(krow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kcol = lv_obj_create(krow);
    lv_obj_remove_style_all(kcol);
    lv_obj_set_size(kcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(kcol, LV_OPA_TRANSP, 0);
    add_caption(kcol, "Current K");
    g_o2_calib_lbl = lv_label_create(kcol);
    lv_label_set_text(g_o2_calib_lbl, "K=--");
    lv_obj_set_style_text_color(g_o2_calib_lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(g_o2_calib_lbl, user_font_text(), 0);

    g_o2_calib_btn = make_circle_btn(krow, dbg_round_btn_sz(),
                                     LV_SYMBOL_REFRESH,
                                     on_o2_calib_click, nullptr);
}

/* ===================== SPI 灯板验色（Status LED） ====================== */

/* 三个验色按钮对应的强制色，下标与 g_status_led_btn / g_status_led_state_lbl 一致 */
const middleware::DebugStatusLedColor kStatusLedBtns[3] = {
    middleware::DebugStatusLedColor::Blue,
    middleware::DebugStatusLedColor::Orange,
    middleware::DebugStatusLedColor::Red,
};

/* 最近一次 GLAVO_Debug_Status_Led_Ntf 回读；三色按钮据此判断"再点一次=释放" */
bool    g_status_led_force = false;
uint8_t g_status_led_color = 0;

/** 按回读状态刷三个按钮的状态字：被强制那颗显示 Forced，其余 Auto。 */
void apply_status_led_labels()
{
    for(uint8_t i = 0; i < 3; ++i) {
        if(!g_status_led_state_lbl[i]) {
            continue;
        }
        const bool forced = g_status_led_force &&
                            g_status_led_color == static_cast<uint8_t>(kStatusLedBtns[i]);
        lv_label_set_text(g_status_led_state_lbl[i], forced ? "Forced" : "Auto");
        lv_obj_set_style_text_color(g_status_led_state_lbl[i],
                                    forced ? user_light_accent : user_light_icon_off, 0);
    }
    if(g_status_led_rgb_lbl) {
        const bool raw = g_status_led_force &&
                         g_status_led_color ==
                             static_cast<uint8_t>(middleware::DebugStatusLedColor::Raw);
        lv_label_set_text(g_status_led_rgb_lbl, raw ? "Forced" : "Auto");
        lv_obj_set_style_text_color(g_status_led_rgb_lbl,
                                    raw ? user_light_accent : user_light_icon_off, 0);
    }
    /* Auto 键与三色键相反：没有任何强制时它才是生效的那个。这里不能写 Forced，
     * 否则会被读成"Auto 被强制了"。 */
    if(g_status_led_auto_lbl) {
        lv_label_set_text(g_status_led_auto_lbl, g_status_led_force ? "Idle" : "Active");
        lv_obj_set_style_text_color(g_status_led_auto_lbl,
                                    g_status_led_force ? user_light_icon_off
                                                       : user_light_accent, 0);
    }
}

/* 三色互斥；点当前已强制那色即释放回自动，与 Auto 键是等价的两条路径（都发
 * force=0），操作员不必先找哪颗亮着。
 * 真实状态以 GLAVO_Debug_Status_Led_Ntf 回读为准，这里不做乐观更新。 */
void on_status_led_color(lv_event_t *e)
{
    const uint8_t idx = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if(idx >= 3) {
        return;
    }
    const uint8_t color = static_cast<uint8_t>(kStatusLedBtns[idx]);
    const bool release = g_status_led_force && g_status_led_color == color;
    ui::DebugMsgProxy::instance().send_status_led_set(!release, color);
}

/* 显式释放接管：灯板回到 报警红 > 打印橙 > 空闲蓝 的自动推导 */
void on_status_led_auto(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_status_led_set(false, 0);
}

/* 裸 RGB 直发：用于现场调 kBlue/kOrange/kRed 的取值，定稿后回写 spi_status_led.cpp */
void on_status_led_rgb_send(lv_event_t *)
{
    uint8_t rgb[3] = {0, 0, 0};
    for(uint8_t i = 0; i < 3; ++i) {
        float v = g_status_led_rgb_ta[i] ? read_ta_float(g_status_led_rgb_ta[i]) : 0.f;
        if(v < 0.f)        v = 0.f;
        else if(v > 255.f) v = 255.f;
        rgb[i] = static_cast<uint8_t>(v + 0.5f);
    }
    ui::DebugMsgProxy::instance().send_status_led_set(
        true, static_cast<uint8_t>(middleware::DebugStatusLedColor::Raw),
        rgb[0], rgb[1], rgb[2]);
}

/** 裸 RGB 调色卡：三个 0-255 输入框 + Send，直发到灯板（不经三色常量）。 */
void build_status_led_rgb_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    add_card_title(card, "Status LED RGB");

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 默认填当前 kBlue(30,30,30)，与 spi_status_led.cpp 的常量对齐 */
    const char *const names[3] = { "R", "G", "B" };
    for(uint8_t i = 0; i < 3; ++i) {
        add_caption(row, names[i]);
        g_status_led_rgb_ta[i] = make_value_input(row, 30.f, true);
        lv_obj_set_flex_grow(g_status_led_rgb_ta[i], 1);
    }

    lv_obj_t *srow = lv_obj_create(card);
    lv_obj_remove_style_all(srow);
    lv_obj_set_width(srow, LV_PCT(100));
    lv_obj_set_height(srow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(srow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);

    g_status_led_rgb_lbl = lv_label_create(srow);
    lv_label_set_text(g_status_led_rgb_lbl, "Auto");
    lv_obj_set_style_text_color(g_status_led_rgb_lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(g_status_led_rgb_lbl, user_font_text(), 0);

    make_send_button(srow, on_status_led_rgb_send, false);
}

void build_other_switches_column(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, 1);
    lv_obj_set_flex_grow(col, 8);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);

    /* ---- 机构 / 工艺 ---- */
    lv_obj_t *proc = make_section_card(col, "Process");
    lv_obj_t *rowA = make_switch_grid_row(proc);
    build_moment_motor_controls(rowA, "Return Motor", 2);
    build_moment_motor_controls(rowA, "Sieve Motor", 0);

    lv_obj_t *rowB = make_switch_grid_row(proc);
    make_dbg_toggle(rowB, "Laser", "On", "Off", LV_SYMBOL_POWER, false, 3,
                    static_cast<uint8_t>(DebugSwitchTarget::Laser));
    make_dbg_toggle(rowB, "Gas Wash", "On", "Off", LV_SYMBOL_POWER, false, 3,
                    static_cast<uint8_t>(DebugSwitchTarget::GasWash));
    make_dbg_toggle(rowB, "Base Heat", "On", "Off", LV_SYMBOL_POWER, false, 3,
                    static_cast<uint8_t>(DebugSwitchTarget::BaseHeat));
    build_moment_motor_controls(rowB, "Inner Door", 1);

    /* ---- 舱内灯 / 阀 ---- */
    lv_obj_t *cabin = make_section_card(col, "Cabin");
    lv_obj_t *rowLed = make_switch_grid_row(cabin);
    for(uint8_t i = 0; i < middleware::kDebugSocLedCount; ++i) {
        make_dbg_toggle(rowLed, kSocLedNames[i], "On", "Off", LV_SYMBOL_CHARGE, false,
                        16, i, true, 0, &g_soc_led_btn[i], &g_soc_led_state_lbl[i]);
    }
    lv_obj_t *rowCab = make_switch_grid_row(cabin);
    make_dbg_toggle(rowCab, "Cabin PM Valve", "On", "Off", LV_SYMBOL_POWER, false,
                    17, 0, true, 0, &g_cabin_pm_valve_btn, &g_cabin_pm_valve_state_lbl);

    /* ---- SPI 灯板验色：强制 kBlue / kOrange / kRed ---- */
    lv_obj_t *sled = make_section_card(col, "Status LED");
    lv_obj_t *rowSled = make_switch_grid_row(sled);
    const char *const kStatusLedNames[3] = { "LED Blue", "LED Orange", "LED Red" };
    for(uint8_t i = 0; i < 3; ++i) {
        make_one_shot_action(rowSled, kStatusLedNames[i], "Auto", LV_SYMBOL_CHARGE,
                             on_status_led_color, &g_status_led_btn[i],
                             &g_status_led_state_lbl[i],
                             reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
    make_one_shot_action(rowSled, "LED Auto", "Active", LV_SYMBOL_REFRESH,
                         on_status_led_auto, &g_status_led_auto_btn,
                         &g_status_led_auto_lbl);
    lv_obj_t *rowSled2 = make_switch_grid_row(sled);
    make_dbg_toggle(rowSled2, "SPI LCD Test", "On", "Off", LV_SYMBOL_REFRESH, false,
                    20, 0, true, 0, &g_spi_pm25_lcd_btn, &g_spi_pm25_lcd_state_lbl);

    /* ---- 打印门控 / 视觉 ---- */
    lv_obj_t *gate = make_section_card(col, "Print Gates");
    lv_obj_t *rowG1 = make_switch_grid_row(gate);
    make_dbg_toggle(rowG1, "Transfer Box", "On", "Off", LV_SYMBOL_DOWNLOAD,
                    control_transfer_box_flow_enabled(), 9, 0);
    make_dbg_toggle(rowG1, "1st Layer Visual", "On", "Off", LV_SYMBOL_PLAY, false,
                    10, 0, true, 0, &g_first_layer_visual_btn, &g_first_layer_visual_state_lbl);
    make_dbg_toggle(rowG1, "Plate Height Adj", "On", "Off", LV_SYMBOL_UP, true,
                    15, 0, true, 0, &g_plate_height_adj_btn, &g_plate_height_adj_state_lbl);
    lv_obj_t *rowG2 = make_switch_grid_row(gate);
    make_dbg_toggle(rowG2, "Skip Optical Path Chk", "On", "Off", LV_SYMBOL_EYE_OPEN, false,
                    14, 0, true, 0, &g_optical_path_check_btn, &g_optical_path_check_state_lbl);
    make_dbg_toggle(rowG2, "Skip Inner Door Occl", "On", "Off", LV_SYMBOL_EYE_CLOSE, false,
                    22, 0, true, 0, &g_inner_door_occlusion_btn, &g_inner_door_occlusion_state_lbl);
    make_dbg_toggle(rowG2, "Skip Plate Present", "On", "Off", LV_SYMBOL_OK, false,
                    23, 0, true, 0, &g_plate_present_btn, &g_plate_present_state_lbl);
    make_dbg_toggle(rowG2, "Skip Plate Prompt", "On", "Off", LV_SYMBOL_NEXT,
                    control_skip_plate_prompt_enabled(), 24, 0);
    lv_obj_t *rowG3 = make_switch_grid_row(gate);
    make_dbg_toggle(rowG3, "Skip O2 Gate", "On", "Off", LV_SYMBOL_OK, false,
                    18, 0, true, 0, &g_skip_o2_gate_btn, &g_skip_o2_gate_state_lbl);
    make_dbg_toggle(rowG3, "Skip Outer Door", "On", "Off", LV_SYMBOL_OK, false,
                    21, 0, true, 0, &g_skip_outer_door_btn, &g_skip_outer_door_state_lbl);
    make_one_shot_action(rowG3, "Galvo Point Test", "Idle", LV_SYMBOL_PLAY,
                         on_galvo_point_test_click,
                         &g_galvo_point_test_btn, &g_galvo_point_test_lbl);
    make_dbg_toggle(rowG3, "Powder Clean", "On", "Off", LV_SYMBOL_POWER, false,
                    4, 0, true, 0, &g_powder_clean_btn, &g_powder_clean_state_lbl);
    update_galvo_test_controls();

    /* ---- 录像 / 模拟 ---- */
    lv_obj_t *media = make_section_card(col, "Media & Sim");
    lv_obj_t *rowM = make_switch_grid_row(media);
    make_dbg_toggle(rowM, "Print H264 Rec", "On", "Off", LV_SYMBOL_PLAY, false,
                    5, 0, true, 0, &g_print_h264_btn, &g_print_h264_state_lbl);
    make_dbg_toggle(rowM, "Clean Video Rec", "On", "Off", LV_SYMBOL_PLAY, true,
                    6, 0, true, 0, &g_clean_record_btn, &g_clean_record_state_lbl);
    make_dbg_toggle(rowM, "Env Sim", "On", "Off", LV_SYMBOL_REFRESH, false,
                    7, 0, true, 0, &g_env_sim_btn, &g_env_sim_state_lbl);
    make_dbg_toggle(rowM, "Powder Sim", "Low", "Full", LV_SYMBOL_WARNING, false,
                    8, 0, true, 0, &g_env_sim_powder_btn, &g_env_sim_powder_state_lbl);
    set_dbg_toggle_enabled(g_env_sim_powder_btn, false);
    lv_obj_t *rowM2 = make_switch_grid_row(media);
    make_dbg_toggle(rowM2, "Local RTSP", "On", "Off", LV_SYMBOL_VIDEO, true,
                    13, 0, true, 0, &g_local_rtsp_btn, &g_local_rtsp_state_lbl);
    make_dbg_toggle(rowM2, "Sys HUD", "On", "Off", LV_SYMBOL_LIST,
                    ui_sys_hud_enabled(), 19, 0);

    /* ---- 注入联调 ---- */
    lv_obj_t *inj = make_section_card(col, "Inject");
    lv_obj_t *rowI = make_switch_grid_row(inj);
    make_dbg_toggle(rowI, "Msg Inject", "On", "Off", LV_SYMBOL_BELL, false, 11, 0);
    make_dbg_toggle(rowI, "Alarm Inject", "On", "Off", LV_SYMBOL_WARNING, false, 12, 0);

    /* 三个粉料阻塞弹窗的手动触发（稿 6345:31129 / 6343:30738 / 6343:30854）。
     * 「粉舱已空」平时由 APP_Print_Powder_Low_Ntf 自动弹，这里只是方便单看版式；
     * **另两档目前没有协议**，只能从这儿打——协议补上之后这两颗可以撤。
     * 三张都挂在打印预览区域内，所以要先进打印态（或至少让监控页建出来）才看得到。 */
    lv_obj_t *rowI2 = make_switch_grid_row(inj);
    make_one_shot_action(rowI2, "Powder Empty", "Dialog", LV_SYMBOL_WARNING,
                         on_supply_dlg_click, nullptr, nullptr,
                         reinterpret_cast<void *>(
                             static_cast<intptr_t>(CONTROL_SUPPLY_DLG_POWDER_EMPTY)));
    make_one_shot_action(rowI2, "Recycle Full", "Dialog", LV_SYMBOL_WARNING,
                         on_supply_dlg_click, nullptr, nullptr,
                         reinterpret_cast<void *>(
                             static_cast<intptr_t>(CONTROL_SUPPLY_DLG_RECYCLE_FULL)));
    make_one_shot_action(rowI2, "Waste Full", "Dialog", LV_SYMBOL_WARNING,
                         on_supply_dlg_click, nullptr, nullptr,
                         reinterpret_cast<void *>(
                             static_cast<intptr_t>(CONTROL_SUPPLY_DLG_WASTE_FULL)));
}

lv_obj_t *build_other_tab(lv_obj_t *body)
{
    lv_obj_t *tab = lv_obj_create(body);
    lv_obj_remove_style_all(tab);
    lv_obj_set_size(tab, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, USER_PAD, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = make_card_row(tab);

    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_width(left, 1);
    lv_obj_set_flex_grow(left, 4);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(left, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(left, LV_SCROLLBAR_MODE_AUTO);

    build_print_params_card(left);
    build_fan_card(left);
    build_o2_calib_card(left);
    build_status_led_rgb_card(left);

    build_other_switches_column(row);

    return tab;
}

/* ============================ Laser 激光器 ============================= */

/* JFSC 状态位文字（bit0 红光 / bit1 激光输出 / bit2 Ready / bit3 外部输出有效） */
const char *const kLaserStatusOn[4]  = {
    "Red Light On", "Laser Emitting", "Ready", "Ext Output Valid" };
const char *const kLaserStatusOff[4] = {
    "Red Light Off", "Laser Idle", "Not Ready", "Ext Output Invalid" };

/* AL 200W 状态位文字（bit0 红光 / bit1 激光使能 / bit2 内外控；bit3 未定义→隐藏该行） */
const char *const kAlLaserStatusOn[4]  = {
    "Red Light On", "Laser On", "Internal Ctrl", nullptr };
const char *const kAlLaserStatusOff[4] = {
    "Red Light Off", "Laser Off", "External Ctrl", nullptr };

/* 激光器类型编码（后续新增机型只需在此枚举与 laser_type_name() 各追加一行） */
enum : uint8_t {
    kLaserTypeUnknown = 0,
    kLaserTypeJFSC    = 1,
    kLaserTypeAL200W  = 2,
};

/** 类型编码 → 显示名（未知编码回落到 "Unknown"） */
const char *laser_type_name(uint8_t code)
{
    switch(code) {
    case kLaserTypeJFSC:   return "JFSC";
    case kLaserTypeAL200W: return "AL 200W";
    default:               return "Unknown";
    }
}

/** 刷新激光器类型显示区（由 register_laser 回调按上报的 laser_type 调用）。 */
void set_laser_type_display(uint8_t code)
{
    if(g_laser_type_lbl) {
        lv_label_set_text(g_laser_type_lbl, laser_type_name(code));
    }
}

/* laser_alarm 位 → 名称 */
struct LaserAlarmBit { uint8_t bit; const char *name; };

/* JFSC：低字节来自寄存器 0x0067，高字节来自 0x0081 */
const LaserAlarmBit kLaserAlarms[] = {
    { 0,  "PD1 Alarm" },   { 1,  "PD2 Alarm" },   { 2,  "Interlock" },
    { 3,  "DB25 E-Stop" }, { 4,  "QCW Pulse" },   { 5,  "QCW Duty" },
    { 8,  "MCU Temp" },    { 9,  "Pump OC1" },    { 10, "Pump OC2" },
    { 11, "QBH Install" }, { 12, "Pump Voltage" },{ 13, "QBH Temp" },
    { 14, "Voltage" },
};

/* AL 200W：输入寄存器 000CH..0015H 打包（bit0 起含义与 JFSC 完全不同） */
const LaserAlarmBit kAlLaserAlarms[] = {
    { 0, "ALD Current" },     { 1, "ALD Voltage" },  { 2, "ALD Temp" },
    { 3, "Fiber Tray Temp" }, { 4, "PCB Temp" },     { 5, "Condensation" },
    { 6, "Cladding PD" },     { 7, "Fiber Break" },  { 8, "E-Stop" },
    { 9, "Service Due" },
};

/** 依据 laser_alarm 位与型号刷新报警列表；alarm=0 则清空 */
void update_laser_alarms(uint16_t alarm, uint8_t type)
{
    if(!g_laser_alarm_box) {
        return;
    }
    lv_obj_clean(g_laser_alarm_box);

    const LaserAlarmBit *tbl = kLaserAlarms;
    size_t count = sizeof(kLaserAlarms) / sizeof(kLaserAlarms[0]);
    if(type == kLaserTypeAL200W) {
        tbl   = kAlLaserAlarms;
        count = sizeof(kAlLaserAlarms) / sizeof(kAlLaserAlarms[0]);
    }
    for(size_t i = 0; i < count; i++) {
        if(alarm & (1u << tbl[i].bit)) {
            lv_obj_t *l = lv_label_create(g_laser_alarm_box);
            lv_label_set_text(l, tbl[i].name);
            lv_obj_set_style_text_color(l, ui_color_danger, 0);
            lv_obj_set_style_text_font(l, user_font_text(), 0);
        }
    }
}

void on_laser_power_send(lv_event_t *)
{
    uint16_t v = read_u16_ta(g_laser_power_ta);
    if(v > 100) {
        v = 100;
    }
    ui::DebugMsgProxy::instance().send_laser_power_set(static_cast<uint8_t>(v));
}

void on_laser_power_cancel(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_laser_power_reset();
    ui_toast_show("Power to external", user_light_accent, 1200);
}

void on_laser_alarm_reset(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_laser_alarm_reset();
    update_laser_alarms(0, g_laser_type_code);   /* 本地立即清空 Alarm 列表，后续以 MCU 上报为准 */
    ui_toast_show("Alarm reset sent", user_light_accent, 1200);
}

/* 值行：左标题 + 弹性留白 + 右白值 */
lv_obj_t *make_value_row(lv_obj_t *parent, const char *cap, lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(row, cap);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, user_light_text, 0);
    lv_obj_set_style_text_font(v, user_font_text(), 0);
    if(out_val) {
        *out_val = v;
    }
    return row;
}

/* 状态行：圆点 + 文字 */
void make_laser_status_row(lv_obj_t *parent, int idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, USER_STATUS_DOT_SZ, USER_STATUS_DOT_SZ);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, user_light_icon_off, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, kLaserStatusOff[idx]);
    lv_obj_set_style_text_color(lbl, user_light_icon_off, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);

    g_laser_status_dot[idx] = dot;
    g_laser_status_lbl[idx] = lbl;
}

lv_obj_t *make_ghost_button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, user_style_btn_ghost(), 0);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return btn;
}

/* 一行：左标题 + 右按钮（SPACE_BETWEEN） */
lv_obj_t *make_action_row(lv_obj_t *parent, const char *cap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(row, cap);
    return row;
}

/* 列容器内的 frosted 卡（内容高度、纵向） */
lv_obj_t *make_laser_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = make_dbg_light_card(parent, false);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    add_card_title(card, title);
    return card;
}

lv_obj_t *build_laser_tab(lv_obj_t *body)
{
    lv_obj_t *tab = lv_obj_create(body);
    lv_obj_remove_style_all(tab);
    lv_obj_set_size(tab, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(tab, USER_PAD, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：Alarm */
    lv_obj_t *alarm_card = make_dbg_light_card(tab, false);
    lv_obj_set_width(alarm_card, 1);
    lv_obj_set_flex_grow(alarm_card, 3);
    lv_obj_set_height(alarm_card, LV_PCT(100));
    lv_obj_set_flex_flow(alarm_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(alarm_card, USER_PAD_SM, 0);
    add_card_title(alarm_card, "Alarm");
    g_laser_alarm_box = lv_obj_create(alarm_card);
    lv_obj_remove_style_all(g_laser_alarm_box);
    lv_obj_set_width(g_laser_alarm_box, LV_PCT(100));
    lv_obj_set_flex_grow(g_laser_alarm_box, 1);
    lv_obj_set_flex_flow(g_laser_alarm_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_laser_alarm_box, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(g_laser_alarm_box, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_laser_alarm_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_laser_alarm_box, LV_SCROLLBAR_MODE_OFF);

    /* 中：竖排容器（Status 在上、Laser Type 在下） */
    lv_obj_t *mid = lv_obj_create(tab);
    lv_obj_remove_style_all(mid);
    lv_obj_set_width(mid, 1);
    lv_obj_set_flex_grow(mid, 3);
    lv_obj_set_height(mid, LV_PCT(100));
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    /* Status：高度改为按内容收缩，下边界上移，给 Laser Type 让出空间 */
    lv_obj_t *status_card = make_dbg_light_card(mid, false);
    lv_obj_set_width(status_card, LV_PCT(100));
    lv_obj_set_height(status_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status_card, USER_PAD_SM, 0);
    add_card_title(status_card, "Status");
    for(int i = 0; i < 4; i++) {
        make_laser_status_row(status_card, i);
    }

    /* Laser Type：新显示区卡*/
    lv_obj_t *type_card = make_laser_card(mid, "Laser Type");
    lv_obj_set_flex_grow(type_card, 1);
    g_laser_type_lbl = lv_label_create(type_card);
    lv_label_set_text(g_laser_type_lbl, "--");
    lv_obj_set_style_text_color(g_laser_type_lbl, user_light_text, 0);
    lv_obj_set_style_text_font(g_laser_type_lbl, user_font_value(), 0);

    /* 右：Laser Data + Power Control + Alarm Ops */
    lv_obj_t *right = make_scroll_col(tab);
    lv_obj_set_width(right, 1);
    lv_obj_set_flex_grow(right, 4);

    lv_obj_t *data_card = make_laser_card(right, "Laser Data");
    make_value_row(data_card, "Temperature", &g_laser_temp_lbl);
    make_value_row(data_card, "Power", &g_laser_power_lbl);
    make_value_row(data_card, "Current", &g_laser_current_lbl);

    lv_obj_t *pc_card = make_laser_card(right, "Power Control");
    lv_obj_t *setrow = lv_obj_create(pc_card);
    lv_obj_remove_style_all(setrow);
    lv_obj_set_width(setrow, LV_PCT(100));
    lv_obj_set_height(setrow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(setrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(setrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(setrow, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(setrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(setrow, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(setrow, "Set Power (%)");
    g_laser_power_ta = make_value_input(setrow, 0.0f, true);
    
    lv_obj_t *pwr_send = make_send_button(setrow, on_laser_power_send, false);
    lv_obj_set_width(pwr_send, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(pwr_send, USER_PAD, 0);

    lv_obj_t *cancelrow = make_action_row(pc_card, "Cancel Power");
    make_ghost_button(cancelrow, "Cancel", on_laser_power_cancel);

    /* 红光/激光/常亮：三个独立按钮，按一下即下发一次对应模式指令。
     * 不做状态回读，界面上无“当前模式”概念，纯测试下发用。
     * 三键并排时「Red Laser Mode」标题 + 按钮会在 1024 档挤爆，故改成
     * 标题独占一行、按钮组下一行右对齐。 */
    lv_obj_t *redblk = lv_obj_create(pc_card);
    lv_obj_remove_style_all(redblk);
    lv_obj_set_width(redblk, LV_PCT(100));
    lv_obj_set_height(redblk, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(redblk, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(redblk, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(redblk, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(redblk, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(redblk, LV_OBJ_FLAG_SCROLLABLE);
    add_caption(redblk, "Red Laser Mode");

    lv_obj_t *redbtns = lv_obj_create(redblk);
    lv_obj_remove_style_all(redbtns);
    lv_obj_set_width(redbtns, LV_PCT(100));
    lv_obj_set_height(redbtns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(redbtns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(redbtns, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(redbtns, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(redbtns, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(redbtns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *btn_red = make_ghost_button(redbtns, "Red", on_red_mode_click);
    lv_obj_t *btn_laser = make_ghost_button(redbtns, "Laser", on_laser_mode_click);
    lv_obj_t *btn_always = make_ghost_button(redbtns, "Red_On", on_red_always_click);
    /* 三键略收紧左右内距，1024 右栏更稳 */
    lv_obj_set_style_pad_hor(btn_red, USER_PAD_SM, 0);
    lv_obj_set_style_pad_hor(btn_laser, USER_PAD_SM, 0);
    lv_obj_set_style_pad_hor(btn_always, USER_PAD_SM, 0);

    lv_obj_t *ops_card = make_laser_card(right, "Alarm Ops");
    lv_obj_t *opsrow = make_action_row(ops_card, "Laser Alarm Reset");
    lv_obj_t *rst = lv_button_create(opsrow);
    lv_obj_add_style(rst, user_style_btn_primary(), 0);
    lv_obj_set_size(rst, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
    lv_obj_add_event_cb(rst, on_laser_alarm_reset, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rst_lbl = lv_label_create(rst);
    lv_label_set_text(rst_lbl, "Reset");
    lv_obj_center(rst_lbl);

    return tab;
}

void bind_debug_callbacks()
{
    auto &proxy = ui::DebugMsgProxy::instance();

    proxy.register_sensor([](const middleware::DebugSensorPayload &p) {
        /* <0 / NaN 与首页一致视为无数据 → "--"（Env Sim 关闭清缓存时用） */
        auto set_metric = [](lv_obj_t *lbl, float v, const char *fmt_with_unit) {
            if(!lbl) {
                return;
            }
            if(std::isnan(v) || v < 0.0f) {
                lv_label_set_text(lbl, "--");
                return;
            }
            lv_label_set_text_fmt(lbl, fmt_with_unit, static_cast<double>(v));
        };
        set_metric(g_sensor_val[0], p.oxygen_percent, "%.1f%%");
        set_metric(g_sensor_val[1], p.filter_oxygen_percent, "%.1f%%");
        set_metric(g_sensor_val[2], p.pressure_kpa, "%.1f");
        set_metric(g_sensor_val[3], p.pm25, "%.1f");
        set_metric(g_sensor_val[4], p.humidity_pct, "%.1f%%");
        set_metric(g_sensor_val[5], p.cabin_temp_c, "%.1f");
        set_metric(g_sensor_val[6], p.baseplate_temp_c, "%.1f");
        set_metric(g_sensor_val[7], p.pm25_cabin, "%.1f");
        if(g_sensor_val[8]) {
            if(p.powder_status == 0xFF) {
                lv_label_set_text(g_sensor_val[8], "--");
            } else {
                const char *txt = (p.powder_status == 0) ? "OK"
                                : (p.powder_status == 1) ? "Busy" : "Fault";
                lv_label_set_text(g_sensor_val[8], txt);
            }
        }
        apply_powder_ir_bits(p.powder_ir_bits, p.powder_ir_valid != 0);
    });

    proxy.register_axis([](const middleware::DebugAxisStatePayload &p) {
        if(g_axis_pos[0]) lv_label_set_text_fmt(g_axis_pos[0], "Pos %.3f mm", static_cast<double>(p.z_pos_mm));
        if(g_axis_pos[1]) lv_label_set_text_fmt(g_axis_pos[1], "Pos %.3f mm", static_cast<double>(p.s_pos_mm));
        if(g_axis_pos[2]) lv_label_set_text_fmt(g_axis_pos[2], "Pos %.3f \xC2\xB0", static_cast<double>(p.d_pos_deg));
        if(g_z_zero_lbl)  lv_label_set_text_fmt(g_z_zero_lbl, "Work zero %.3f mm", static_cast<double>(p.z_work_zero_mm));
        /* 转发给「测试」Tab，供“等待到位”闭环判断使用（proxy 只有单一 axis 订阅槽） */
        ui::debug_test_feed_axis(p);
    });

    proxy.register_galvo([](const middleware::DebugGalvoStatePayload &p) {
        (void)p;
        /* Galvo Coordinate 区域此处展示中心点偏移（来自 cal.csv），不显示运行态坐标。 */
        apply_galvo_center_labels();
    });

    proxy.register_device([](const middleware::DebugDeviceStatePayload &p) {
        const uint8_t on[4] = { p.device_awake, p.door_open, p.motor_on, p.light_on };
        for(int i = 0; i < 4; i++) {
            if(g_dev_btn[i]) {
                if(on[i]) lv_obj_add_state(g_dev_btn[i], LV_STATE_CHECKED);
                else      lv_obj_clear_state(g_dev_btn[i], LV_STATE_CHECKED);
            }
            if(g_dev_state[i]) {
                lv_label_set_text(g_dev_state[i], on[i] ? kDevOn[i] : kDevOff[i]);
                lv_obj_set_style_text_color(g_dev_state[i],
                    on[i] ? user_light_accent : user_light_icon_off, 0);
            }
        }
    });

    proxy.register_laser([](const middleware::DebugLaserStatePayload &p) {
        /* 型号来自 3ST 上报；若偶发未知则沿用上次已知型号，避免界面抖动。 */
        if(p.laser_type != kLaserTypeUnknown) {
            g_laser_type_code = p.laser_type;
        }
        const uint8_t type = g_laser_type_code;
        const bool    is_al = (type == kLaserTypeAL200W);

        set_laser_type_display(type);

        /* 状态：按型号选表；某位未定义（AL bit3）则隐藏该行 */
        const char *const *on_txt  = is_al ? kAlLaserStatusOn  : kLaserStatusOn;
        const char *const *off_txt = is_al ? kAlLaserStatusOff : kLaserStatusOff;
        for(int i = 0; i < 4; i++) {
            lv_obj_t *lbl = g_laser_status_lbl[i];
            if(!lbl) {
                continue;
            }
            lv_obj_t *row = lv_obj_get_parent(lbl);
            if(on_txt[i] == nullptr) {          /* 该型号未定义此状态位 → 隐藏整行 */
                if(row) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            if(row) lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
            const bool on = (p.status & (1u << i)) != 0;
            lv_label_set_text(lbl, on ? on_txt[i] : off_txt[i]);
            lv_obj_set_style_text_color(lbl,
                on ? user_light_accent : user_light_icon_off, 0);
            if(g_laser_status_dot[i]) {
                lv_obj_set_style_bg_color(g_laser_status_dot[i],
                    on ? user_light_ok : user_light_icon_off, 0);
            }
        }
        /* 温度：JFSC 原始整数℃；AL 单位 0.1℃ */
        if(g_laser_temp_lbl) {
            if(is_al) {
                lv_label_set_text_fmt(g_laser_temp_lbl, "%u.%u \xC2\xB0""C",
                    static_cast<unsigned>(p.pump_temp_raw / 10),
                    static_cast<unsigned>(p.pump_temp_raw % 10));
            } else {
                lv_label_set_text_fmt(g_laser_temp_lbl, "%u \xC2\xB0""C",
                    static_cast<unsigned>(p.pump_temp_raw));
            }
        }

        /* 功率：JFSC 原始 0..255→百分比；AL 单位 0.1% */
        if(g_laser_power_lbl) {
            const unsigned pct = is_al
                ? static_cast<unsigned>(p.power_raw / 10)
                : static_cast<unsigned>(p.power_raw * 100u / 255u);
            lv_label_set_text_fmt(g_laser_power_lbl, "%u%%", pct);
        }

        /* 电流：JFSC 单位 0.01A；AL 单位 0.1A（3ST 上报） */
        if(g_laser_current_lbl) {
            if(is_al) {
                lv_label_set_text_fmt(g_laser_current_lbl, "%u.%u A",
                    static_cast<unsigned>(p.current_raw / 10),
                    static_cast<unsigned>(p.current_raw % 10));
            } else {
                lv_label_set_text_fmt(g_laser_current_lbl, "%u.%02u A",
                    static_cast<unsigned>(p.current_raw / 100),
                    static_cast<unsigned>(p.current_raw % 100));
            }
        }

        update_laser_alarms(p.alarm, type);
    });

    proxy.register_print_h264_record([](bool on) {
        if(g_print_h264_btn) {
            if(on) {
                lv_obj_add_state(g_print_h264_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_print_h264_btn, LV_STATE_CHECKED);
            }
        }
        if(g_print_h264_state_lbl) {
            lv_label_set_text(g_print_h264_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_print_h264_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    /* MCU 清粉结束：按钮弹回 Off，状态字显示结果；下次点击重新开始 */
    proxy.register_clean_test_result([](uint8_t result) {
        if(g_powder_clean_btn) {
            lv_obj_clear_state(g_powder_clean_btn, LV_STATE_CHECKED);
        }
        if(g_powder_clean_state_lbl) {
            const auto r = static_cast<middleware::DebugCleanTestResult>(result);
            const char *txt = (r == middleware::DebugCleanTestResult::Done)    ? "Done"
                            : (r == middleware::DebugCleanTestResult::Failed)  ? "Failed"
                            : (r == middleware::DebugCleanTestResult::Aborted) ? "Aborted"
                                                                               : "Off";
            lv_label_set_text(g_powder_clean_state_lbl, txt);
            lv_obj_set_style_text_color(g_powder_clean_state_lbl, user_light_icon_off, 0);
        }
    });

    proxy.register_clean_record([](bool on) {
        if(g_clean_record_btn) {
            if(on) {
                lv_obj_add_state(g_clean_record_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_clean_record_btn, LV_STATE_CHECKED);
            }
        }
        if(g_clean_record_state_lbl) {
            lv_label_set_text(g_clean_record_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_clean_record_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    proxy.register_env_sim([](bool on) {
        g_env_sim_on = on;
        if(g_env_sim_btn) {
            if(on) {
                lv_obj_add_state(g_env_sim_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_env_sim_btn, LV_STATE_CHECKED);
            }
        }
        if(g_env_sim_state_lbl) {
            lv_label_set_text(g_env_sim_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_env_sim_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
        set_dbg_toggle_enabled(g_env_sim_powder_btn, on);
    });

    proxy.register_env_sim_powder([](bool on) {
        if(g_env_sim_powder_btn) {
            if(on) {
                lv_obj_add_state(g_env_sim_powder_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_env_sim_powder_btn, LV_STATE_CHECKED);
            }
        }
        if(g_env_sim_powder_state_lbl) {
            lv_label_set_text(g_env_sim_powder_state_lbl, on ? "Low" : "Full");
            lv_obj_set_style_text_color(g_env_sim_powder_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
        set_dbg_toggle_enabled(g_env_sim_powder_btn, g_env_sim_on);
    });

    proxy.register_skip_o2_gate([](bool on) {
        if(g_skip_o2_gate_btn) {
            if(on) {
                lv_obj_add_state(g_skip_o2_gate_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_skip_o2_gate_btn, LV_STATE_CHECKED);
            }
        }
        if(g_skip_o2_gate_state_lbl) {
            lv_label_set_text(g_skip_o2_gate_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_skip_o2_gate_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    proxy.register_skip_outer_door([](bool on) {
        if(g_skip_outer_door_btn) {
            if(on) {
                lv_obj_add_state(g_skip_outer_door_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_skip_outer_door_btn, LV_STATE_CHECKED);
            }
        }
        if(g_skip_outer_door_state_lbl) {
            lv_label_set_text(g_skip_outer_door_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_skip_outer_door_state_lbl,
                on ? user_light_accent : user_light_icon_off, 0);
        }
    });
    proxy.register_inner_door_busy([](bool busy) {
        apply_inner_door_debug_busy(busy);
    });

    proxy.register_o2_calib([](const middleware::DebugO2CalibRspPayload& p) {
        /* Get 回读 i_ua=0：标定等待中忽略，避免提前清 pending / 盖掉 Calibrating */
        if(g_o2_calib_pending && p.ok && p.i_ua <= 0.f) {
            return;
        }
        const bool was_pending = g_o2_calib_pending;
        g_o2_calib_pending = false;
        if(g_o2_calib_btn) {
            lv_obj_add_flag(g_o2_calib_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(g_o2_calib_btn, LV_STATE_DISABLED);
        }
        apply_o2_calib_label(p, was_pending);
        if(g_o2_calib_ref_ta && p.ok && p.ref_o2_percent > 0.f &&
           p.ref_o2_percent < 100.f &&
           !lv_obj_has_state(g_o2_calib_ref_ta, LV_STATE_FOCUSED)) {
            char rbuf[16];
            std::snprintf(rbuf, sizeof(rbuf), "%.2f",
                          static_cast<double>(p.ref_o2_percent));
            lv_textarea_set_text(g_o2_calib_ref_ta, rbuf);
        }
        if(was_pending) {
            if(p.ok) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "O2 calib OK K=%.1f",
                              static_cast<double>(p.k));
                ui_toast_show(buf, user_light_accent, 1800);
            } else {
                ui_toast_show(o2_calib_err_text(p.err),
                              user_light_notice_dot, 2000);
            }
        }
    });

    proxy.register_first_layer_visual_check([](bool on) {
        if(g_first_layer_visual_btn) {
            if(on) {
                lv_obj_add_state(g_first_layer_visual_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_first_layer_visual_btn, LV_STATE_CHECKED);
            }
        }
        if(g_first_layer_visual_state_lbl) {
            lv_label_set_text(g_first_layer_visual_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_first_layer_visual_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    proxy.register_height_calib_progress([](const middleware::HeightCalibProgressPayload& p) {
        if(!g_height_calib_lbl) {
            return;
        }
        char buf[64];
        if(p.stage == 0) {
            std::snprintf(buf, sizeof(buf), "Starting %u pts", p.total_count);
        } else if(p.stage == 1) {
            std::snprintf(buf, sizeof(buf), "h=%.2f (%u/%u)",
                          static_cast<double>(p.current_height_mm),
                          p.collected_count, p.total_count);
        } else if(p.stage == 2) {
            std::snprintf(buf, sizeof(buf), "Done");
        } else {
            std::snprintf(buf, sizeof(buf), "Failed");
        }
        lv_label_set_text(g_height_calib_lbl, buf);
    });

    /* 载荷 on=1 表示执行光路。按钮是 Skip，回读时取反。 */
    proxy.register_optical_path_check([](bool detect_on) {
        const bool skip = !detect_on;
        if(g_optical_path_check_btn) {
            if(skip) {
                lv_obj_add_state(g_optical_path_check_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_optical_path_check_btn, LV_STATE_CHECKED);
            }
        }
        if(g_optical_path_check_state_lbl) {
            lv_label_set_text(g_optical_path_check_state_lbl, skip ? "On" : "Off");
            lv_obj_set_style_text_color(g_optical_path_check_state_lbl,
                                        skip ? user_light_accent : user_light_icon_off, 0);
        }
    });

    proxy.register_plate_height_adj([](bool on) {
        if(g_plate_height_adj_btn) {
            if(on) {
                lv_obj_add_state(g_plate_height_adj_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_plate_height_adj_btn, LV_STATE_CHECKED);
            }
        }
        if(g_plate_height_adj_state_lbl) {
            lv_label_set_text(g_plate_height_adj_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_plate_height_adj_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    /* 载荷 on=1 表示做挡门融合。按钮是 Skip，回读时取反。 */
    proxy.register_inner_door_occlusion([](bool detect_on) {
        const bool skip = !detect_on;
        if(g_inner_door_occlusion_btn) {
            if(skip) {
                lv_obj_add_state(g_inner_door_occlusion_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_inner_door_occlusion_btn, LV_STATE_CHECKED);
            }
        }
        if(g_inner_door_occlusion_state_lbl) {
            lv_label_set_text(g_inner_door_occlusion_state_lbl, skip ? "On" : "Off");
            lv_obj_set_style_text_color(g_inner_door_occlusion_state_lbl,
                                        skip ? user_light_accent : user_light_icon_off, 0);
        }
    });

    /* 载荷 on=1 表示执行读码。按钮是 Skip，回读时取反。 */
    proxy.register_plate_present([](bool detect_on) {
        const bool skip = !detect_on;
        if(g_plate_present_btn) {
            if(skip) {
                lv_obj_add_state(g_plate_present_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_plate_present_btn, LV_STATE_CHECKED);
            }
        }
        if(g_plate_present_state_lbl) {
            lv_label_set_text(g_plate_present_state_lbl, skip ? "On" : "Off");
            lv_obj_set_style_text_color(g_plate_present_state_lbl,
                                        skip ? user_light_accent : user_light_icon_off, 0);
        }
    });

    proxy.register_soc_led([](uint8_t mask) {
        for(uint8_t i = 0; i < middleware::kDebugSocLedCount; ++i) {
            const bool on = (mask & static_cast<uint8_t>(1u << i)) != 0;
            if(g_soc_led_btn[i]) {
                if(on) {
                    lv_obj_add_state(g_soc_led_btn[i], LV_STATE_CHECKED);
                } else {
                    lv_obj_clear_state(g_soc_led_btn[i], LV_STATE_CHECKED);
                }
            }
            if(g_soc_led_state_lbl[i]) {
                lv_label_set_text(g_soc_led_state_lbl[i], on ? "On" : "Off");
                lv_obj_set_style_text_color(g_soc_led_state_lbl[i],
                                            on ? user_light_accent : user_light_icon_off, 0);
            }
        }
    });

    proxy.register_cabin_pm_valve([](bool on) {
        if(g_cabin_pm_valve_btn) {
            if(on) {
                lv_obj_add_state(g_cabin_pm_valve_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_cabin_pm_valve_btn, LV_STATE_CHECKED);
            }
        }
        if(g_cabin_pm_valve_state_lbl) {
            lv_label_set_text(g_cabin_pm_valve_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_cabin_pm_valve_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    /* 灯板强制态以 Glavo 回读为准；三色按钮的"再点一次=释放"也据此判断 */
    proxy.register_status_led([](const middleware::DebugStatusLedPayload &p) {
        g_status_led_force = (p.force != 0);
        g_status_led_color = p.color;
        apply_status_led_labels();
        /* 裸 RGB 生效值回填输入框，避免退页重进后显示与灯板不一致；
         * 正在编辑的框不动，否则会把用户刚输入的值冲掉 */
        if(g_status_led_force &&
           p.color == static_cast<uint8_t>(middleware::DebugStatusLedColor::Raw)) {
            const uint8_t rgb[3] = { p.r, p.g, p.b };
            for(uint8_t i = 0; i < 3; ++i) {
                if(!g_status_led_rgb_ta[i] ||
                   lv_obj_has_state(g_status_led_rgb_ta[i], LV_STATE_FOCUSED)) {
                    continue;
                }
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(rgb[i]));
                lv_textarea_set_text(g_status_led_rgb_ta[i], buf);
            }
        }
    });

    proxy.register_spi_pm25_lcd_test([](bool on) {
        if(g_spi_pm25_lcd_btn) {
            if(on) {
                lv_obj_add_state(g_spi_pm25_lcd_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_spi_pm25_lcd_btn, LV_STATE_CHECKED);
            }
        }
        if(g_spi_pm25_lcd_state_lbl) {
            lv_label_set_text(g_spi_pm25_lcd_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_spi_pm25_lcd_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    /* 风机实测：MCU V4 EXT 帧驱动（约 1s），旧固件不发该帧，行保持 "--" */
    proxy.register_fan_state([](const middleware::DebugFanStatePayload &p) {
        apply_fan_state(p);
    });

    /* Media 回报的是实际状态：相机未就绪 / 编码器打不开时会回 Off，按钮随之弹回。 */
    proxy.register_local_rtsp([](bool on) {
        if(g_local_rtsp_btn) {
            if(on) {
                lv_obj_add_state(g_local_rtsp_btn, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(g_local_rtsp_btn, LV_STATE_CHECKED);
            }
        }
        if(g_local_rtsp_state_lbl) {
            lv_label_set_text(g_local_rtsp_state_lbl, on ? "On" : "Off");
            lv_obj_set_style_text_color(g_local_rtsp_state_lbl,
                                        on ? user_light_accent : user_light_icon_off, 0);
        }
    });

    /* Glavo 回报实际状态：文件缺失 / 正式打印互斥失败时回 Off，开关弹回。 */
    proxy.register_test_image([](bool on) {
        if(g_test_image_seg) {
            const uint8_t want = on ? 0u : 1u; /* 0=On 1=Off */
            if(ui_user_segmented_get_selected(g_test_image_seg) != want) {
                ui_user_segmented_set_selected(g_test_image_seg, want);
                if(!on) {
                    ui_toast_show("Test file missing or busy",
                                  user_light_notice_dot, 1800);
                }
            }
        }
        update_galvo_save_enabled();
    });

    proxy.register_galvo_point_test([](bool on) {
        const bool was_pending = g_galvo_point_test_pending;
        g_galvo_point_test_pending = false;
        update_galvo_test_controls();
        if(!on && was_pending) {
            ui_toast_show("Point test unavailable",
                          user_light_notice_dot, 1800);
        }
    });
}

void reset_globals()
{
    stop_zone4_timer();
    ui::debug_test_reset();
    g_tab1 = g_tab2 = g_tab3 = g_tab4 = nullptr;
    g_laser_alarm_box = g_laser_temp_lbl = g_laser_power_lbl = g_laser_power_ta = nullptr;
    g_laser_type_lbl = g_laser_current_lbl = nullptr;
    g_laser_type_code = 0;
    for(int i = 0; i < 4; i++) {
        g_laser_status_lbl[i] = nullptr;
        g_laser_status_dot[i] = nullptr;
    }
    g_z_zero_lbl = g_galvo_x_lbl = g_galvo_y_lbl = nullptr;
    g_galvo_scale_ta = g_galvo_step_seg = g_test_image_seg = g_galvo_save_btn = nullptr;
    g_galvo_abs_x_ta = g_galvo_abs_y_ta = nullptr;
    g_galvo_point_test_btn = g_galvo_point_test_lbl = nullptr;
    g_galvo_point_test_pending = false;
    g_galvo_center_valid = false;
    g_galvo_center_x = 0.0;
    g_galvo_center_y = 0.0;
    g_galvo_saved_x = 0.0;
    g_galvo_saved_y = 0.0;
    for(int i = 0; i < 10; i++) g_sensor_val[i] = nullptr;
    for(int i = 0; i < 8; i++) g_powder_ir_val[i] = nullptr;
    g_zone4_timer = nullptr;
    for(int i = 0; i < 3; i++) {
        g_axis_pos[i] = g_axis_step[i] = g_axis_ctrl[i] = nullptr;
        g_axis_mode[i] = 0;
        g_axis_step_saved[i][0] = kAxisDefaultStep;
        g_axis_step_saved[i][1] = kAxisDefaultStep;
    }
    for(int i = 0; i < 4; i++) { g_dev_btn[i] = nullptr; g_dev_state[i] = nullptr; }
    g_fan_duty_ta = nullptr;
    g_fan_rpm_lbl = nullptr;
    g_fan_state_lbl = nullptr;
    g_print_layer_ta = nullptr;
    g_print_balance_ta = nullptr;
    g_print_h264_btn = nullptr;
    g_print_h264_state_lbl = nullptr;
    g_powder_clean_btn = nullptr;
    g_powder_clean_state_lbl = nullptr;
    g_clean_record_btn = nullptr;
    g_clean_record_state_lbl = nullptr;
    g_env_sim_btn = nullptr;
    g_env_sim_state_lbl = nullptr;
    g_env_sim_powder_btn = nullptr;
    g_env_sim_powder_state_lbl = nullptr;
    g_skip_o2_gate_btn = nullptr;
    g_skip_o2_gate_state_lbl = nullptr;
    g_skip_outer_door_btn = nullptr;
    g_skip_outer_door_state_lbl = nullptr;
    g_o2_calib_btn = nullptr;
    g_o2_calib_lbl = nullptr;
    g_o2_calib_ref_ta = nullptr;
    g_o2_calib_pending = false;
    g_first_layer_visual_btn = nullptr;
    g_first_layer_visual_state_lbl = nullptr;
    g_optical_path_check_btn = nullptr;
    g_optical_path_check_state_lbl = nullptr;
    g_plate_height_adj_btn = nullptr;
    g_plate_height_adj_state_lbl = nullptr;
    g_inner_door_occlusion_btn = nullptr;
    g_inner_door_occlusion_state_lbl = nullptr;
    g_plate_present_btn = nullptr;
    g_plate_present_state_lbl = nullptr;
    for(uint8_t i = 0; i < middleware::kDebugSocLedCount; ++i) {
        g_soc_led_btn[i] = nullptr;
        g_soc_led_state_lbl[i] = nullptr;
    }
    g_cabin_pm_valve_btn = nullptr;
    g_cabin_pm_valve_state_lbl = nullptr;
    for(uint8_t i = 0; i < 3; ++i) {
        g_status_led_btn[i] = nullptr;
        g_status_led_state_lbl[i] = nullptr;
        g_status_led_rgb_ta[i] = nullptr;
    }
    g_status_led_rgb_lbl = nullptr;
    g_status_led_auto_btn = nullptr;
    g_status_led_auto_lbl = nullptr;
    g_spi_pm25_lcd_btn = nullptr;
    g_spi_pm25_lcd_state_lbl = nullptr;
    g_status_led_force = false;
    g_status_led_color = 0;
    g_height_calib_lbl = nullptr;
    g_local_rtsp_btn = nullptr;
    g_local_rtsp_state_lbl = nullptr;
    g_env_sim_on = false;
}

} /* namespace */

void page_user_debug_refresh(bool /*force*/)
{
    g_galvo_center_valid = load_galvo_center_offset_from_cal(g_galvo_center_x, g_galvo_center_y);
    g_galvo_saved_x = g_galvo_center_x;
    g_galvo_saved_y = g_galvo_center_y;
    apply_galvo_center_labels();
    /* 读数由 DebugMsgProxy 回调驱动。页面对象由 PageManager 缓存（on_create 仅首次执行），
     * 而 page_user_debug_exit 退页时会清空回调；on_enter 每次都会调本函数，故必须在此
     * 重新挂载回调，否则「返回后再进入」将因回调为空而不再刷新读数。g_* 指针在缓存的
     * 页面对象上始终有效，可安全重挂。 */
    bind_debug_callbacks();
    start_zone4_timer();
    update_galvo_save_enabled();
    ui::DebugMsgProxy::instance().request_print_h264_record_state();
    ui::DebugMsgProxy::instance().request_clean_record_state();
    ui::DebugMsgProxy::instance().request_env_sim_state();
    ui::DebugMsgProxy::instance().request_env_sim_powder_state();
    ui::DebugMsgProxy::instance().request_skip_o2_gate_state();
    ui::DebugMsgProxy::instance().request_skip_outer_door_state();
    ui::DebugMsgProxy::instance().request_inner_door_busy();
    ui::DebugMsgProxy::instance().request_o2_calib_state();
    ui::DebugMsgProxy::instance().request_first_layer_visual_check_state();
    ui::DebugMsgProxy::instance().request_optical_path_check_state();
    ui::DebugMsgProxy::instance().request_plate_height_adj_state();
    ui::DebugMsgProxy::instance().request_inner_door_occlusion_state();
    ui::DebugMsgProxy::instance().request_plate_present_state();
    ui::DebugMsgProxy::instance().request_soc_led_state();
    ui::DebugMsgProxy::instance().request_cabin_pm_valve_state();
    ui::DebugMsgProxy::instance().request_status_led_state();
    ui::DebugMsgProxy::instance().request_spi_pm25_lcd_test_state();
    ui::DebugMsgProxy::instance().request_test_image_state();
    ui::DebugMsgProxy::instance().request_galvo_point_test_state();
    ui::DebugMsgProxy::instance().request_local_rtsp_state();
}

void page_user_debug_exit(void)
{
    ui_keyboard_hide();
    stop_zone4_timer();
    ui::debug_test_stop_flow();   /* 退页停止正在运行的测试流程（含点动补停） */
    auto &proxy = ui::DebugMsgProxy::instance();
    /* 无条件释放灯板接管：否则退页后灯板卡在验色色上，真报警的红灯就看不到了 */
    proxy.send_status_led_set(false, 0);
    proxy.register_sensor(nullptr);
    proxy.register_axis(nullptr);
    proxy.register_galvo(nullptr);
    proxy.register_device(nullptr);
    proxy.register_laser(nullptr);
    proxy.register_print_h264_record(nullptr);
    proxy.register_clean_record(nullptr);
    proxy.register_clean_test_result(nullptr);
    proxy.register_env_sim(nullptr);
    proxy.register_env_sim_powder(nullptr);
    proxy.register_skip_o2_gate(nullptr);
    proxy.register_skip_outer_door(nullptr);
    proxy.register_inner_door_busy(nullptr);
    proxy.register_o2_calib(nullptr);
    proxy.register_first_layer_visual_check(nullptr);
    proxy.register_optical_path_check(nullptr);
    proxy.register_plate_height_adj(nullptr);
    proxy.register_inner_door_occlusion(nullptr);
    proxy.register_plate_present(nullptr);
    proxy.register_soc_led(nullptr);
    proxy.register_cabin_pm_valve(nullptr);
    proxy.register_status_led(nullptr);
    proxy.register_spi_pm25_lcd_test(nullptr);
    proxy.register_fan_state(nullptr);
    proxy.register_height_calib_progress(nullptr);
    proxy.register_test_image(nullptr);
    proxy.register_galvo_point_test(nullptr);
    proxy.register_local_rtsp(nullptr);
}

lv_obj_t *page_user_debug_create(lv_obj_t *parent)
{
    reset_globals();

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, USER_PAD, 0);
    lv_obj_set_style_pad_row(page, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶行：返回按钮 + 传感器条 */
    lv_obj_t *toprow = lv_obj_create(page);
    lv_obj_remove_style_all(toprow);
    lv_obj_set_width(toprow, LV_PCT(100));
    lv_obj_set_height(toprow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(toprow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toprow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(toprow, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(toprow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(toprow, LV_OBJ_FLAG_SCROLLABLE);

    make_circle_btn(toprow, USER_BACK_BTN_H, LV_SYMBOL_LEFT, on_back_click, nullptr);
    lv_obj_t *strip = build_sensor_strip(toprow);
    lv_obj_set_flex_grow(strip, 1);   /* 占满返回钮右侧剩余宽度，避免 100% 宽越界裁剪 */

    /* 主板 8 路料位红外：看哪一路被遮挡（有粉） */
    build_powder_ir_strip(page);

    /* Tab + 设备行 */
    build_control_row(page);

    /* 内容区 */
    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* Tab1 移动控制：左 Z/S/D 三轴，右 振镜 + 标定 */
    g_tab1 = lv_obj_create(body);
    lv_obj_remove_style_all(g_tab1);
    lv_obj_set_size(g_tab1, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g_tab1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_tab1, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(g_tab1, USER_PAD, 0);
    lv_obj_set_style_bg_opa(g_tab1, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_tab1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = make_scroll_col(g_tab1);
    build_axis_card(left, 0);
    build_axis_card(left, 1);
    build_axis_card(left, 2);

    lv_obj_t *right = make_scroll_col(g_tab1);
    build_galvo_card(right);
    build_calib_card(right);
    build_height_calib_card(right);

    /* Tab2 其他控制：左 Print/Fan/O2 Calib，右 Process/Cabin/Print Gates/Media/Inject */
    g_tab2 = build_other_tab(body);

    /* Tab3 激光器 */
    g_tab3 = build_laser_tab(body);

    /* Tab4 测试（自动化用例表，中文界面） */
    g_tab4 = ui::build_debug_test_tab(body);

    bind_debug_callbacks();
    return page;
}
