#include "page_user_debug_test.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "debug_msg_proxy.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/**
 * @file page_user_debug_test.cpp
 * @brief 调试页「测试」Tab —— 自动化测试用例表（中文界面）。
 *
 * 布局对齐参考图：顶部「循环次数 / 当前循环 / 状态」信息条 + 一排操作按钮
 * （新增/删除/上移/下移/载入示例 … 开始执行/停止），下方为任务表
 * （功能 / 参数 / 延迟(ms) / 运行时间(ms) / 状态）。
 *
 * 执行模型（照搬 SnowX 串口工具的“测试”页）：任务按行顺序执行，可循环 N 次；
 * 由 lv_timer 驱动的状态机推进：StartTask →（发命令等待运行时间 / 等待类轮询到位）
 * → FinishTask → 完成后延迟 → 下一行。等待类用轴位置回读做闭环判断。
 */

namespace {

using middleware::DebugAxis;
using middleware::DebugJogMode;
using middleware::DebugMotorAction;
using middleware::DebugDeviceTarget;
using middleware::DebugSwitchTarget;

/* ---- 功能项（下拉顺序即枚举顺序，下拉 index == (int)Kind） ---- */
enum class Kind : uint8_t {
    ZInc, SInc, DInc,                                   /* 增量移动（参数=带符号位移） */
    ZJogUp, ZJogDown, SJogLeft, SJogRight, DJogCw, DJogCcw, /* 点动（参数=速度，正数） */
    WaitS0, WaitSVal, WaitZ0, WaitZVal, WaitDInc,       /* 等待到位 */
    ZHome, SHome, ZSetZero,                             /* 归零 / 设工作零点 */
    InnerOpen, InnerClose, InnerStop,                   /* 内门 */
    SieveRun, SieveReverse, SieveStop,                  /* 筛粉电机 */
    ReturnToA, ReturnToB, ReturnStop,                   /* 回粉电机（同 Other 页 Process 那三颗钮） */
    LightOn, LightOff,                                  /* 照明 */
    DoorOpen, DoorClose,                                /* 门 */
    FanOn, FanOff, FanDuty,                             /* 风机（FanDuty 参数=占空比0~100） */
    GasOn, GasOff,                                      /* 洗气 */
    BaseHeatOn, BaseHeatOff,                            /* 基板加热 */
    COUNT
};

/* 下拉显示名（index 与 Kind 对齐） */
const char *const kKindName[] = {
    "Z轴增量移动", "S轴增量移动", "D轴增量移动",
    "Z轴点动上(速度为参数)", "Z轴点动下(速度为参数)",
    "S轴点动左(速度为参数)", "S轴点动右(速度为参数)",
    "D轴点动顺时针(速度为参数)", "D轴点动逆时针(速度为参数)",
    "等待S轴位置为0", "等待S轴位置为参数",
    "等待Z轴位置为0", "等待Z轴位置为参数", "等待D轴增量到位",
    "Z轴归零", "S轴归零", "Z轴设工作零点",
    "内门开", "内门关", "内门停",
    "筛粉运行", "筛粉反转倒渣", "筛粉停止",
    "回粉到A", "回粉到B", "回粉停止(自动)",
    "照明开", "照明关",
    "门开", "门关",
    "开风机", "关风机", "风机占空比为参数",
    "洗气开", "洗气关",
    "基板加热开", "基板加热关",
};
static_assert(sizeof(kKindName) / sizeof(kKindName[0]) == static_cast<size_t>(Kind::COUNT),
              "kKindName 与 Kind 数量不一致");

/* 等待到位容差（浮点单位）：Z/S 位移 mm，D 角度 ° */
constexpr float kWaitTolZmm  = 0.02f;
constexpr float kWaitTolSmm  = 0.10f;
constexpr float kWaitTolDdeg = 0.30f;

/* ---- 一行任务的控件集 ---- */
struct TestRow {
    lv_obj_t *cont      = nullptr;
    lv_obj_t *index_lbl = nullptr;
    lv_obj_t *dd        = nullptr;
    lv_obj_t *param_ta  = nullptr;
    lv_obj_t *delay_ta  = nullptr;
    lv_obj_t *run_ta    = nullptr;
    lv_obj_t *status_lbl = nullptr;
};

/* ---- 状态机阶段 ---- */
enum class Stage : uint8_t { Idle, StartTask, FinishTask, WaitCondition };

/* ---- 模块全局 ---- */
lv_obj_t *g_tab            = nullptr;
lv_obj_t *g_loop_ta        = nullptr;
lv_obj_t *g_loop_status    = nullptr;
lv_obj_t *g_task_status    = nullptr;
lv_obj_t *g_rows_box       = nullptr;
lv_obj_t *g_btn_add        = nullptr;
lv_obj_t *g_btn_del        = nullptr;
lv_obj_t *g_btn_up         = nullptr;
lv_obj_t *g_btn_down       = nullptr;
lv_obj_t *g_btn_example    = nullptr;
lv_obj_t *g_btn_start      = nullptr;
lv_obj_t *g_btn_stop       = nullptr;

std::vector<TestRow *> g_rows;
TestRow  *g_sel = nullptr;

/* 运行态 */
lv_timer_t *g_timer        = nullptr;
Stage       g_stage        = Stage::Idle;
bool        g_running      = false;
int         g_cur_loop     = 0;
int         g_total_loops  = 0;
int         g_cur_index    = -1;
uint32_t    g_task_start_tick = 0;

/* 执行快照（开始执行时从表格采集，运行中不受编辑影响） */
struct TaskSnap {
    Kind  kind;
    float param;
    int   delay_ms;
    int   run_ms;
    int   row;      /**< 对应表格行号，用于回写状态 */
};
std::vector<TaskSnap> g_snap;

/* 实时轴位置（由 debug_test_feed_axis 更新） */
bool  g_have_axis = false;
float g_axis_z = 0.0f, g_axis_s = 0.0f, g_axis_d = 0.0f;
/* “等待D轴增量到位”的目标角度（由前一条 D 增量移动记录） */
bool  g_have_d_target = false;
float g_d_target = 0.0f;

/* ---- 小工具 ---- */
bool is_wait_kind(Kind k) {
    return k == Kind::WaitS0 || k == Kind::WaitSVal || k == Kind::WaitZ0 ||
           k == Kind::WaitZVal || k == Kind::WaitDInc;
}
bool is_jog_kind(Kind k) {
    return k == Kind::ZJogUp || k == Kind::ZJogDown || k == Kind::SJogLeft ||
           k == Kind::SJogRight || k == Kind::DJogCw || k == Kind::DJogCcw;
}
DebugAxis jog_axis(Kind k) {
    switch(k) {
        case Kind::ZJogUp: case Kind::ZJogDown: return DebugAxis::Z;
        case Kind::SJogLeft: case Kind::SJogRight: return DebugAxis::S;
        default: return DebugAxis::D;
    }
}

float read_ta_float(lv_obj_t *ta) {
    if(!ta) return 0.0f;
    const char *s = lv_textarea_get_text(ta);
    return s ? static_cast<float>(std::atof(s)) : 0.0f;
}
int read_ta_int(lv_obj_t *ta) {
    const float f = read_ta_float(ta);
    return (f < 0.0f) ? 0 : static_cast<int>(f + 0.5f);
}

void set_row_status(int row, const char *txt) {
    if(row < 0 || row >= static_cast<int>(g_rows.size())) return;
    if(g_rows[row]->status_lbl) lv_label_set_text(g_rows[row]->status_lbl, txt);
}

void set_edit_enabled(bool en) {
    auto set = [en](lv_obj_t *o) {
        if(!o) return;
        if(en) lv_obj_clear_state(o, LV_STATE_DISABLED);
        else   lv_obj_add_state(o, LV_STATE_DISABLED);
    };
    set(g_loop_ta);
    set(g_btn_add); set(g_btn_del); set(g_btn_up); set(g_btn_down); set(g_btn_example);
    set(g_btn_start);
    /* 停止按钮与运行态相反 */
    if(g_btn_stop) {
        if(en) lv_obj_add_state(g_btn_stop, LV_STATE_DISABLED);
        else   lv_obj_clear_state(g_btn_stop, LV_STATE_DISABLED);
    }
}

/* ---- 命令下发：功能项 → DebugMsgProxy ---- */
void send_task_command(Kind k, float param) {
    auto &p = ui::DebugMsgProxy::instance();
    const int dir = (param < 0.0f) ? -1 : 1;
    const float mag = std::fabs(param);
    switch(k) {
        case Kind::ZInc: p.send_axis_jog(DebugAxis::Z, DebugJogMode::Click, dir, mag); break;
        case Kind::SInc: p.send_axis_jog(DebugAxis::S, DebugJogMode::Click, dir, mag); break;
        case Kind::DInc:
            p.send_axis_jog(DebugAxis::D, DebugJogMode::Click, dir, mag);
            /* 记录本次增量目标角，供“等待D轴增量到位”判断 */
            if(g_have_axis) { g_d_target = g_axis_d + param; g_have_d_target = true; }
            else            { g_have_d_target = false; }
            break;
        /* 点动：方向按功能项固定（上/左/顺=+1，下/右/逆=-1），速度取参数绝对值 */
        case Kind::ZJogUp:    p.send_axis_jog(DebugAxis::Z, DebugJogMode::Hold, +1, mag); break;
        case Kind::ZJogDown:  p.send_axis_jog(DebugAxis::Z, DebugJogMode::Hold, -1, mag); break;
        case Kind::SJogLeft:  p.send_axis_jog(DebugAxis::S, DebugJogMode::Hold, +1, mag); break;
        case Kind::SJogRight: p.send_axis_jog(DebugAxis::S, DebugJogMode::Hold, -1, mag); break;
        case Kind::DJogCw:    p.send_axis_jog(DebugAxis::D, DebugJogMode::Hold, +1, mag); break;
        case Kind::DJogCcw:   p.send_axis_jog(DebugAxis::D, DebugJogMode::Hold, -1, mag); break;
        case Kind::ZHome:     p.send_axis_home(DebugAxis::Z); break;
        case Kind::SHome:     p.send_axis_home(DebugAxis::S); break;
        case Kind::ZSetZero:  p.send_axis_set_zero(DebugAxis::Z); break;
        case Kind::InnerOpen:  p.send_inner_door(DebugMotorAction::Forward); break;
        case Kind::InnerClose: p.send_inner_door(DebugMotorAction::Reverse); break;
        case Kind::InnerStop:  p.send_inner_door(DebugMotorAction::Stop); break;
        case Kind::SieveRun:     p.send_sieve(DebugMotorAction::Forward); break;
        case Kind::SieveReverse: p.send_sieve(DebugMotorAction::Reverse); break;
        case Kind::SieveStop:    p.send_sieve(DebugMotorAction::Stop); break;
        /* 回粉电机：送往 A 瓶 0x16 / 送往 B 瓶 0x17 / 停止并恢复自动 0x18。送 A、送 B 发出后
         * 一直送，直到执行到「回粉停止(自动)」那行；流程中途停止不补发（同筛粉） */
        case Kind::ReturnToA:  p.send_return_motor(DebugMotorAction::Reverse); break;
        case Kind::ReturnToB:  p.send_return_motor(DebugMotorAction::Forward); break;
        case Kind::ReturnStop: p.send_return_motor(DebugMotorAction::Stop); break;
        case Kind::LightOn:  p.send_device(DebugDeviceTarget::Light, true); break;
        case Kind::LightOff: p.send_device(DebugDeviceTarget::Light, false); break;
        case Kind::DoorOpen:  p.send_device(DebugDeviceTarget::Door, true); break;
        case Kind::DoorClose: p.send_device(DebugDeviceTarget::Door, false); break;
        case Kind::FanOn:   p.send_fan(true); break;
        case Kind::FanOff:  p.send_fan(false); break;
        case Kind::FanDuty: {
            float d = mag; if(d > 100.0f) d = 100.0f;
            p.send_fan_duty(d);
            break;
        }
        case Kind::GasOn:  p.send_switch(DebugSwitchTarget::GasWash, true); break;
        case Kind::GasOff: p.send_switch(DebugSwitchTarget::GasWash, false); break;
        case Kind::BaseHeatOn:  p.send_switch(DebugSwitchTarget::BaseHeat, true); break;
        case Kind::BaseHeatOff: p.send_switch(DebugSwitchTarget::BaseHeat, false); break;
        /* 等待类不发命令 */
        case Kind::WaitS0: case Kind::WaitSVal: case Kind::WaitZ0:
        case Kind::WaitZVal: case Kind::WaitDInc: case Kind::COUNT: break;
    }
}

void send_jog_stop(Kind k) {
    ui::DebugMsgProxy::instance().send_axis_hold_stop(jog_axis(k));
}

/* ---- 状态机 ---- */
void on_timer(lv_timer_t *);

void schedule_step(int delay_ms, Stage stage) {
    g_stage = stage;
    if(!g_timer) g_timer = lv_timer_create(on_timer, 10, nullptr);
    lv_timer_set_period(g_timer, delay_ms < 1 ? 1 : static_cast<uint32_t>(delay_ms));
    lv_timer_reset(g_timer);
    lv_timer_resume(g_timer);
}

void stop_flow(const char *reason) {
    if(!g_running && g_stage == Stage::Idle) return;
    /* 停止时若点动正在运行（已发启动、等待运行时间结束），补发点动停止避免电机一直转 */
    if(g_running && g_stage == Stage::FinishTask &&
       g_cur_index >= 0 && g_cur_index < static_cast<int>(g_snap.size())) {
        const Kind k = g_snap[g_cur_index].kind;
        if(is_jog_kind(k)) send_jog_stop(k);
    }
    if(g_timer) { lv_timer_delete(g_timer); g_timer = nullptr; }
    g_stage = Stage::Idle;
    g_running = false;
    set_edit_enabled(true);
    if(g_task_status) lv_label_set_text_fmt(g_task_status, "状态：%s", reason);
    if(g_cur_index >= 0 && g_cur_index < static_cast<int>(g_snap.size())) {
        const bool done = (std::strstr(reason, "完成") != nullptr);
        set_row_status(g_snap[g_cur_index].row, done ? "完成" : "停止");
    }
}

void start_next_task();

void finish_current_task() {
    if(g_cur_index < 0 || g_cur_index >= static_cast<int>(g_snap.size())) return;
    const TaskSnap &t = g_snap[g_cur_index];
    if(is_jog_kind(t.kind)) send_jog_stop(t.kind);
    set_row_status(t.row, "完成");
    schedule_step(t.delay_ms, Stage::StartTask);
}

void poll_wait_condition() {
    if(g_cur_index < 0 || g_cur_index >= static_cast<int>(g_snap.size())) return;
    const TaskSnap &t = g_snap[g_cur_index];
    bool satisfied = false;
    char wait_txt[48] = {0};

    switch(t.kind) {
        case Kind::WaitS0:
        case Kind::WaitSVal: {
            const float target = (t.kind == Kind::WaitSVal) ? t.param : 0.0f;
            satisfied = g_have_axis && std::fabs(g_axis_s - target) <= kWaitTolSmm;
            std::snprintf(wait_txt, sizeof(wait_txt), "S轴位置为%.2f", static_cast<double>(target));
            break;
        }
        case Kind::WaitZ0:
        case Kind::WaitZVal: {
            const float target = (t.kind == Kind::WaitZVal) ? t.param : 0.0f;
            satisfied = g_have_axis && std::fabs(g_axis_z - target) <= kWaitTolZmm;
            std::snprintf(wait_txt, sizeof(wait_txt), "Z轴位置为%.2f", static_cast<double>(target));
            break;
        }
        case Kind::WaitDInc: {
            if(!g_have_d_target) {
                stop_flow("等待D轴增量到位失败：前面无D增量移动记录");
                return;
            }
            satisfied = g_have_axis && std::fabs(g_axis_d - g_d_target) <= kWaitTolDdeg;
            std::snprintf(wait_txt, sizeof(wait_txt), "D轴到位(目标%.1f)", static_cast<double>(g_d_target));
            break;
        }
        default: return;
    }

    if(satisfied) { finish_current_task(); return; }

    /* runMs>0 作为等待超时时间 */
    if(t.run_ms > 0 && static_cast<int>(lv_tick_elaps(g_task_start_tick)) >= t.run_ms) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "等待%s超时", wait_txt);
        stop_flow(msg);
        return;
    }
    char st[64];
    std::snprintf(st, sizeof(st), "等待%s", wait_txt);
    set_row_status(t.row, st);
    schedule_step(100, Stage::WaitCondition);
}

void start_next_task() {
    ++g_cur_index;
    if(g_cur_index >= static_cast<int>(g_snap.size())) {
        /* 一轮结束 */
        if(g_cur_loop >= g_total_loops) { stop_flow("任务流程已完成"); return; }
        ++g_cur_loop;
        g_cur_index = -1;
        if(g_loop_status)
            lv_label_set_text_fmt(g_loop_status, "当前循环：%d/%d", g_cur_loop, g_total_loops);
        for(auto *r : g_rows) if(r->status_lbl) lv_label_set_text(r->status_lbl, "等待");
        schedule_step(0, Stage::StartTask);
        return;
    }

    const TaskSnap &t = g_snap[g_cur_index];
    set_row_status(t.row, "执行中");
    if(g_task_status)
        lv_label_set_text_fmt(g_task_status, "状态：第%d轮 第%d行 %s",
                              g_cur_loop, t.row + 1, kKindName[static_cast<int>(t.kind)]);
    g_task_start_tick = lv_tick_get();

    if(is_wait_kind(t.kind)) { schedule_step(0, Stage::WaitCondition); return; }

    send_task_command(t.kind, t.param);
    schedule_step(t.run_ms, Stage::FinishTask);
}

void on_timer(lv_timer_t *) {
    if(!g_running) return;
    switch(g_stage) {
        case Stage::StartTask:     start_next_task();     break;
        case Stage::FinishTask:    finish_current_task(); break;
        case Stage::WaitCondition: poll_wait_condition(); break;
        case Stage::Idle:          break;
    }
}

/* ---- 采集表格为执行快照（含基本校验） ---- */
bool collect_snapshot() {
    g_snap.clear();
    for(int row = 0; row < static_cast<int>(g_rows.size()); ++row) {
        TestRow *r = g_rows[row];
        const Kind kind = static_cast<Kind>(lv_dropdown_get_selected(r->dd));
        const float param = read_ta_float(r->param_ta);

        if(is_jog_kind(kind) && read_ta_int(r->run_ta) <= 0) {
            ui_toast_show("点动任务需填写>0的运行时间", user_light_notice_dot, 1800);
            return false;
        }
        g_snap.push_back(TaskSnap{ kind, param, read_ta_int(r->delay_ta),
                                   read_ta_int(r->run_ta), row });
    }
    if(g_snap.empty()) {
        ui_toast_show("任务表为空，请先新增任务", user_light_notice_dot, 1800);
        return false;
    }
    return true;
}

/* ---- 行选中高亮 ---- */
void apply_selection() {
    for(auto *r : g_rows) {
        if(!r->cont) continue;
        const bool sel = (r == g_sel);
        lv_obj_set_style_border_color(r->cont, sel ? user_light_accent : user_light_divider, 0);
        lv_obj_set_style_border_opa(r->cont, sel ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

void select_row(TestRow *r) { g_sel = r; apply_selection(); }

void renumber_rows() {
    for(int i = 0; i < static_cast<int>(g_rows.size()); ++i)
        if(g_rows[i]->index_lbl) lv_label_set_text_fmt(g_rows[i]->index_lbl, "%d", i + 1);
}

int row_index(TestRow *r) {
    for(int i = 0; i < static_cast<int>(g_rows.size()); ++i)
        if(g_rows[i] == r) return i;
    return -1;
}

/* ---- 事件：行选中 ---- */
void on_row_clicked(lv_event_t *e) {
    select_row(static_cast<TestRow *>(lv_event_get_user_data(e)));
}

/* ---- 构建：中文下拉 ---- */
void style_cjk_dropdown(lv_obj_t *dd) {
    lv_obj_set_style_text_font(dd, user_font_cjk(), 0);
    lv_obj_set_style_bg_color(dd, user_light_more_bg, 0);
    lv_obj_set_style_border_width(dd, 2, 0);
    lv_obj_set_style_border_color(dd, user_light_accent, 0);
    lv_obj_set_style_text_color(dd, user_light_text, 0);
    /* 默认下拉箭头用的是 FontAwesome 符号，子集字体没有→显示方块；
     * 改用子集字体内含的 ▼(U+25BC) 作为下拉箭头 */
    lv_dropdown_set_symbol(dd, "▼");
    /* 弹出列表在构造时即创建（关闭时仅隐藏），此处可直接设中文字体与配色 */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if(list) {
        lv_obj_set_style_text_font(list, user_font_cjk(), 0);
        lv_obj_set_style_bg_color(list, user_light_card, 0);
        lv_obj_set_style_text_color(list, user_light_text, 0);
        lv_obj_set_style_border_color(list, user_light_accent, 0);
        /* 选中项自己给强调蓝底配白字，不依赖主题默认值（主题那套是按深色档配的） */
        lv_obj_set_style_bg_color(list, user_light_accent, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(list, lv_color_white(), LV_PART_SELECTED | LV_STATE_CHECKED);
    }
}

/* ---- 构建：数字输入框 ---- */
lv_obj_t *make_num_ta(lv_obj_t *parent, const char *init, int32_t w) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, init ? init : "0");
    lv_obj_set_width(ta, w);
    lv_obj_set_height(ta, UI_INPUT_H);
    lv_obj_set_style_text_font(ta, user_font_cjk(), 0);
    lv_obj_set_style_bg_color(ta, user_light_more_bg, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, user_light_accent, 0);
    lv_obj_set_style_text_color(ta, user_light_text, 0);
    lv_obj_set_style_pad_ver(ta, USER_PAD_SM / 2, 0);
    ui_keyboard_attach(ta, LV_KEYBOARD_MODE_NUMBER);
    return ta;
}

/* 任务表列宽（px），表头与数据行共用以对齐 */
constexpr int32_t kColIndexW  = 30;
constexpr int32_t kColParamW  = 84;
constexpr int32_t kColDelayW  = 84;
constexpr int32_t kColRunW    = 100;
constexpr int32_t kColFuncGrow = 5;
constexpr int32_t kColStatGrow = 3;

/* 文字颜色按调用方传入：表头强调蓝、说明与序号 / 状态列浅灰、循环与任务状态墨色。
 * 调试页还是深色档时这里统一写死白色、忽略形参；调试页翻浅之后白字压在浅底上看不见，
 * 改回用形参（调用方给的本来就是浅色档的色）。 */
lv_obj_t *make_cjk_label(lv_obj_t *parent, const char *txt, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, user_font_cjk(), 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* 一行的通用容器 */
lv_obj_t *make_table_row_container(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(row, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* 新增一行任务（返回新行指针），selected 决定是否选中 */
TestRow *add_task_row(Kind kind, const char *param, int delay_ms, int run_ms, bool select) {
    TestRow *r = new TestRow();
    r->cont = make_table_row_container(g_rows_box);
    lv_obj_set_style_border_width(r->cont, 1, 0);
    lv_obj_set_style_border_color(r->cont, user_light_divider, 0);
    lv_obj_set_style_border_opa(r->cont, LV_OPA_40, 0);
    lv_obj_add_flag(r->cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->cont, on_row_clicked, LV_EVENT_CLICKED, r);

    r->index_lbl = make_cjk_label(r->cont, "0", user_light_icon_off);
    lv_obj_set_width(r->index_lbl, kColIndexW);

    r->dd = lv_dropdown_create(r->cont);
    lv_dropdown_set_options_static(r->dd,
        /* 与 kKindName 顺序一致的整块选项串 */
        "Z轴增量移动\n" "S轴增量移动\n" "D轴增量移动\n"
        "Z轴点动上(速度为参数)\n" "Z轴点动下(速度为参数)\n"
        "S轴点动左(速度为参数)\n" "S轴点动右(速度为参数)\n"
        "D轴点动顺时针(速度为参数)\n" "D轴点动逆时针(速度为参数)\n"
        "等待S轴位置为0\n" "等待S轴位置为参数\n"
        "等待Z轴位置为0\n" "等待Z轴位置为参数\n" "等待D轴增量到位\n"
        "Z轴归零\n" "S轴归零\n" "Z轴设工作零点\n"
        "内门开\n" "内门关\n" "内门停\n"
        "筛粉运行\n" "筛粉反转倒渣\n" "筛粉停止\n"
        "回粉到A\n" "回粉到B\n" "回粉停止(自动)\n"
        "照明开\n" "照明关\n"
        "门开\n" "门关\n"
        "开风机\n" "关风机\n" "风机占空比为参数\n"
        "洗气开\n" "洗气关\n"
        "基板加热开\n" "基板加热关");
    lv_dropdown_set_selected(r->dd, static_cast<uint16_t>(kind));
    lv_obj_set_width(r->dd, 0);
    lv_obj_set_flex_grow(r->dd, kColFuncGrow);
    lv_obj_set_height(r->dd, UI_INPUT_H);
    style_cjk_dropdown(r->dd);
    lv_obj_add_event_cb(r->dd, on_row_clicked, LV_EVENT_CLICKED, r);

    r->param_ta = make_num_ta(r->cont, param, kColParamW);
    lv_obj_add_event_cb(r->param_ta, on_row_clicked, LV_EVENT_CLICKED, r);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", delay_ms);
    r->delay_ta = make_num_ta(r->cont, buf, kColDelayW);
    lv_obj_add_event_cb(r->delay_ta, on_row_clicked, LV_EVENT_CLICKED, r);
    std::snprintf(buf, sizeof(buf), "%d", run_ms);
    r->run_ta = make_num_ta(r->cont, buf, kColRunW);
    lv_obj_add_event_cb(r->run_ta, on_row_clicked, LV_EVENT_CLICKED, r);

    r->status_lbl = make_cjk_label(r->cont, "未执行", user_light_icon_off);
    lv_obj_set_width(r->status_lbl, 0);
    lv_obj_set_flex_grow(r->status_lbl, kColStatGrow);

    g_rows.push_back(r);
    renumber_rows();
    if(select) select_row(r);
    return r;
}

/* 交换两行内容（下拉选项 + 三个输入框），选中跟随到目标行 */
void swap_rows(int a, int b) {
    if(a < 0 || b < 0 || a >= static_cast<int>(g_rows.size()) ||
       b >= static_cast<int>(g_rows.size())) return;
    TestRow *ra = g_rows[a];
    TestRow *rb = g_rows[b];
    const uint32_t ka = lv_dropdown_get_selected(ra->dd);
    const uint32_t kb = lv_dropdown_get_selected(rb->dd);
    lv_dropdown_set_selected(ra->dd, kb);
    lv_dropdown_set_selected(rb->dd, ka);
    auto swap_ta = [](lv_obj_t *x, lv_obj_t *y) {
        char tmp[24];
        std::snprintf(tmp, sizeof(tmp), "%s", lv_textarea_get_text(x));
        lv_textarea_set_text(x, lv_textarea_get_text(y));
        lv_textarea_set_text(y, tmp);
    };
    swap_ta(ra->param_ta, rb->param_ta);
    swap_ta(ra->delay_ta, rb->delay_ta);
    swap_ta(ra->run_ta, rb->run_ta);
    select_row(g_rows[b]);
}

/* ---- 按钮事件 ---- */
void on_add(lv_event_t *)     { add_task_row(Kind::ZInc, "0", 0, 0, true); }

void on_remove(lv_event_t *) {
    if(!g_sel) return;
    const int idx = row_index(g_sel);
    if(idx < 0) return;
    lv_obj_delete(g_sel->cont);
    delete g_sel;
    g_rows.erase(g_rows.begin() + idx);
    g_sel = g_rows.empty() ? nullptr
          : g_rows[idx < static_cast<int>(g_rows.size()) ? idx : g_rows.size() - 1];
    renumber_rows();
    apply_selection();
}

void on_move_up(lv_event_t *) {
    const int idx = row_index(g_sel);
    if(idx > 0) swap_rows(idx, idx - 1);
}
void on_move_down(lv_event_t *) {
    const int idx = row_index(g_sel);
    if(idx >= 0 && idx + 1 < static_cast<int>(g_rows.size())) swap_rows(idx, idx + 1);
}

void clear_all_rows() {
    for(auto *r : g_rows) { if(r->cont) lv_obj_delete(r->cont); delete r; }
    g_rows.clear();
    g_sel = nullptr;
}

void on_load_example(lv_event_t *) {
    clear_all_rows();
    add_task_row(Kind::SInc, "140", 500, 0, false);
    add_task_row(Kind::SHome, "0", 0, 0, false);
    add_task_row(Kind::WaitS0, "0", 0, 0, false);
    if(g_loop_ta) lv_textarea_set_text(g_loop_ta, "100");
    renumber_rows();
}

void on_start(lv_event_t *) {
    if(g_running) return;
    if(!collect_snapshot()) return;

    g_total_loops = read_ta_int(g_loop_ta);
    if(g_total_loops < 1) g_total_loops = 1;
    g_cur_loop = 1;
    g_cur_index = -1;
    g_running = true;
    g_have_d_target = false;
    if(g_loop_status) lv_label_set_text_fmt(g_loop_status, "当前循环：%d/%d", g_cur_loop, g_total_loops);
    if(g_task_status) lv_label_set_text(g_task_status, "状态：运行中");
    for(auto *r : g_rows) if(r->status_lbl) lv_label_set_text(r->status_lbl, "等待");
    set_edit_enabled(false);
    schedule_step(0, Stage::StartTask);
}

void on_stop(lv_event_t *) { stop_flow("任务流程已停止"); }

/* ---- 中文按钮 ---- */
/* 纯色填充按钮：常态 fill 满填，按下变半透明作点击反馈，禁用时更淡；文字白色 */
lv_obj_t *make_action_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, lv_color_t fill) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, fill, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);   /* 点击反馈 */
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_DISABLED);  /* 禁用变淡 */
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, user_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return btn;
}

lv_obj_t *make_spacer(lv_obj_t *parent) {
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_height(s, 1);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
    return s;
}

/* 表头 */
void build_table_header(lv_obj_t *parent) {
    lv_obj_t *row = make_table_row_container(parent);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_t *idx = make_cjk_label(row, "", user_light_icon_off);
    lv_obj_set_width(idx, kColIndexW);
    lv_obj_t *fn = make_cjk_label(row, "功能", user_light_accent);
    lv_obj_set_width(fn, 0); lv_obj_set_flex_grow(fn, kColFuncGrow);
    lv_obj_t *pa = make_cjk_label(row, "参数", user_light_accent);
    lv_obj_set_width(pa, kColParamW);
    lv_obj_t *de = make_cjk_label(row, "延迟(ms)", user_light_accent);
    lv_obj_set_width(de, kColDelayW);
    lv_obj_t *rn = make_cjk_label(row, "运行时间(ms)", user_light_accent);
    lv_obj_set_width(rn, kColRunW);
    lv_obj_t *stt = make_cjk_label(row, "状态", user_light_accent);
    lv_obj_set_width(stt, 0); lv_obj_set_flex_grow(stt, kColStatGrow);
}

} // namespace

namespace ui {

lv_obj_t *build_debug_test_tab(lv_obj_t *body) {
    debug_test_reset();

    g_tab = lv_obj_create(body);
    lv_obj_remove_style_all(g_tab);
    lv_obj_set_size(g_tab, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g_tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_tab, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(g_tab, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g_tab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_tab, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部说明 */
    make_cjk_label(g_tab, "任务表按行顺序执行，可设置每行运行时间和完成后的延迟时间。",
                   user_light_icon_off);

    /* 信息条：循环次数 + 当前循环 + 状态 */
    lv_obj_t *info = lv_obj_create(g_tab);
    lv_obj_remove_style_all(info);
    lv_obj_set_width(info, LV_PCT(100));
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(info, USER_PAD, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    make_cjk_label(info, "循环次数：", user_light_text);
    g_loop_ta = make_num_ta(info, "100", 90);
    g_loop_status = make_cjk_label(info, "当前循环：0/0", user_light_text);
    make_spacer(info);
    g_task_status = make_cjk_label(info, "状态：空闲", user_light_text);

    /* 操作按钮行 */
    lv_obj_t *btns = lv_obj_create(g_tab);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    const lv_color_t kBlue = user_light_accent;
    const lv_color_t kRed  = lv_palette_main(LV_PALETTE_RED);
    g_btn_add     = make_action_btn(btns, "新增任务", on_add, kBlue);
    g_btn_del     = make_action_btn(btns, "删除任务", on_remove, kBlue);
    g_btn_up      = make_action_btn(btns, "上移", on_move_up, kBlue);
    g_btn_down    = make_action_btn(btns, "下移", on_move_down, kBlue);
    g_btn_example = make_action_btn(btns, "载入示例", on_load_example, kBlue);
    make_spacer(btns);
    g_btn_start   = make_action_btn(btns, "开始执行", on_start, kBlue);
    g_btn_stop    = make_action_btn(btns, "停止", on_stop, kRed);
    lv_obj_add_state(g_btn_stop, LV_STATE_DISABLED);

    /* 表头 */
    build_table_header(g_tab);

    /* 任务行滚动区 */
    g_rows_box = lv_obj_create(g_tab);
    lv_obj_remove_style_all(g_rows_box);
    lv_obj_set_width(g_rows_box, LV_PCT(100));
    lv_obj_set_flex_grow(g_rows_box, 1);
    lv_obj_set_flex_flow(g_rows_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_rows_box, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(g_rows_box, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_rows_box, LV_DIR_VER);

    /* 默认载入示例三行 */
    add_task_row(Kind::SInc, "140", 500, 0, false);
    add_task_row(Kind::SHome, "0", 0, 0, false);
    add_task_row(Kind::WaitS0, "0", 0, 0, false);
    renumber_rows();

    return g_tab;
}

void debug_test_feed_axis(const middleware::DebugAxisStatePayload &p) {
    g_axis_z = p.z_pos_mm;
    g_axis_s = p.s_pos_mm;
    g_axis_d = p.d_pos_deg;
    g_have_axis = true;
}

void debug_test_stop_flow(void) {
    if(g_running) stop_flow("任务流程已停止");
    else if(g_timer) { lv_timer_delete(g_timer); g_timer = nullptr; }
}

void debug_test_reset(void) {
    if(g_timer) { lv_timer_delete(g_timer); g_timer = nullptr; }
    g_stage = Stage::Idle;
    g_running = false;
    g_cur_loop = g_total_loops = 0;
    g_cur_index = -1;
    g_have_axis = g_have_d_target = false;
    g_snap.clear();
    /* 仅释放 Row 堆对象，lv 对象随页面销毁由 LVGL 回收 */
    for(auto *r : g_rows) delete r;
    g_rows.clear();
    g_sel = nullptr;
    g_tab = g_loop_ta = g_loop_status = g_task_status = g_rows_box = nullptr;
    g_btn_add = g_btn_del = g_btn_up = g_btn_down = g_btn_example = nullptr;
    g_btn_start = g_btn_stop = nullptr;
}

} // namespace ui
