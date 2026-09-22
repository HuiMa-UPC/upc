#ifndef UI_ALARM_STATE_HPP
#define UI_ALARM_STATE_HPP

#include "ui_message_store.hpp"

#include <stdbool.h>
#include <stdint.h>

/**
 * @file ui_alarm_state.hpp
 * @brief MCU 报警的 UI 侧唯一数据源：生命周期缓存 + 复位在途 + 历史落盘。
 *
 * 数据入口是 `GLAVO_Alarm_Report_Ntf`（0xA3/0x20 单条上报原样透传），本模块
 * 按 state_mask 推导生命周期（见 doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md）：
 *
 *   - EVENT（bit2）：一次性消息。记历史、上浮层（蓝卡，5s 后自动从浮层撤下）；
 *   - CONDITION：ACTIVE / RTN_LATCHED 期间挂在"当前报警"表并同步到
 *     `ui_message_store`（红卡）；回到 NORMAL 时撤卡、转入历史。
 *
 * 文案由 `ui_alarm_catalog`（字典）+ 报文 detail 组合；查不到的 code/detail
 * 降级显示原始编号，**绝不丢弃**（固件可能比字典新）。severity/clear_mode
 * 一律以报文为准，字典只出文字。
 *
 * 历史落盘 `/userdata/messages/alarm_history.jsonl`（一行一条，只存原始字段，文案
 * 加载时从字典重新生成），上限 UI_ALARM_HISTORY_CAP 条，满了丢最旧。
 *
 * 线程约定：总线回调先 lv_async_call 切回 UI 线程（同 ui_message_store），
 * 所有读写接口只能在 UI 线程调用。
 */

#ifdef __cplusplus
extern "C" {
#endif

#define UI_ALARM_HISTORY_CAP  200
#define UI_ALARM_TITLE_LEN    64
#define UI_ALARM_BODY_LEN     160

/** CONDITION 生命周期（NORMAL 即离开当前表，不需要枚举值）。 */
typedef enum {
    UI_ALARM_STAGE_ACTIVE = 0,   /**< 故障条件当前存在 */
    UI_ALARM_STAGE_RTN_LATCHED,  /**< 条件已恢复，等待复位确认 */
} ui_alarm_stage_t;

/** 一条当前（未解除）CONDITION 报警。 */
typedef struct {
    uint16_t         code;
    uint8_t          severity;      /**< 报文原值：1=information 2=warning 3=error 4=critical */
    uint8_t          clear_mode;    /**< 报文原值：middleware::AlarmClearMode */
    uint32_t         detail;
    ui_alarm_stage_t stage;
    bool             reset_pending; /**< 复位已下发、还没等到 MCU 的下一帧上报 */
    bool             simulated;     /**< 调试注入的假报警：标题带 [SIM]、不落盘 */
    int64_t          ts_ms;         /**< 首次触发墙钟毫秒 */
    char             title[UI_ALARM_TITLE_LEN];
    char             body[UI_ALARM_BODY_LEN];
} ui_alarm_entry_t;

/** 一条历史记录：已解除的 CONDITION 或已发生的 EVENT。 */
typedef struct {
    uint16_t code;
    uint8_t  severity;
    uint8_t  is_event;
    uint32_t detail;
    int64_t  ts_ms;           /**< 触发时刻（墙钟毫秒） */
    int64_t  resolved_ts_ms;  /**< CONDITION 解除时刻；EVENT 恒 0 */
    bool     simulated;       /**< 调试注入：标题带 [SIM]，不写进历史文件 */
    char     title[UI_ALARM_TITLE_LEN];
    char     body[UI_ALARM_BODY_LEN];
} ui_alarm_history_entry_t;

/** 观察者：当前表或历史变化后在 UI 线程被调用。 */
typedef void (*ui_alarm_observer_t)(void);

/**
 * 订阅报警上报 + 加载历史。须在 `UiMsgProxy::init()` 与
 * `ui_message_store_init()` 之后调用一次（见 ui_app.cpp）。重复调用无副作用。
 */
void ui_alarm_state_init(void);

/**
 * 报文 severity（1 information / 2 warning / 3 fault / 4 trip）→ 消息中心等级。
 * 未知值按不低于 warning 处理。配色再经 `ui_message_severity_color()`，报警列表
 * 与浮层因此同色。
 */
ui_msg_severity_t ui_alarm_severity_to_msg(uint8_t severity);

/* ---- 当前（未解除）CONDITION ---- */

int ui_alarm_current_count(void);

/** 按显示序取：severity 降序，同级新的在前。越界返回 NULL。 */
const ui_alarm_entry_t *ui_alarm_current_at(int idx);

/** 按报警号查当前表；不在表中（已解除/从未触发）返回 NULL。 */
const ui_alarm_entry_t *ui_alarm_current_find(uint16_t code);

/**
 * 下发复位请求（UI_Alarm_Reset_Ntf）。仅 clear_mode=RESET 且不在
 * reset_pending 的条目受理；受理后置 reset_pending，等 MCU 下一帧上报翻转
 * 状态。解除 pending 的三条路径：该 code 的新上报、复位失败回执、10s 超时兜底。
 * @return 是否已下发
 */
bool ui_alarm_request_reset(uint16_t code);

/**
 * 这条报警此刻能不能点 Reset —— **三个复位入口共用同一判据**（消息页详情抽屉、
 * 首页终止结果条、打印入口拦截弹窗），别各写各的。
 *
 * 规则（依据 doc/主控板串口通信协议 - 信息报警命令.csv 与 报警map.csv 的
 * reset_policy 列；旧版 SOC_TLV_ALARM_SINGLE_REPORT…§5 那条
 * `show_reset = latched && clear_mode==RESET` 伪代码已作废）：
 *  - 非 RESET 清除方式 / 复位在途 → 不可点；
 *  - RTN_LATCHED → 可点；
 *  - ACTIVE 时看 reset_policy：OWNER_TASK 可点（MCU 转给业务任务做硬件恢复，
 *    激光器 1301、各轴故障都属此类，不点它们永远好不了）；LOCAL 不可点
 *    （条件仍在时 MCU 直接拒绝，如 1124/1125）。
 *  - 字典查不到该 code（固件比字典新）→ **可点**。查不到是信息缺失而非"已知
 *    不可复位"，现表 30 条 RESET 里 28 条是 OWNER_TASK；拦下来新报警就永远
 *    复不了位，而误发一帧只是白等一轮 pending。
 */
bool ui_alarm_reset_allowed(const ui_alarm_entry_t *e);

/**
 * 请求一轮报警快照（UI_Alarm_Snapshot_Ntf）。MCU **不会主动推**，不请求就补不齐
 * 重启/重连前就亮着的报警。内部带节流（上一轮在途、或距上次请求不足 2s 则跳过），
 * 调用方不必自己防抖。`reason` 只进日志，便于排查是哪个触发点发的。
 *
 * 已有的触发点：init（本模块）、串口重连（Glavo 侧自行发）、30s 轮询（本模块）、
 * 进消息中心、开始打印前。
 */
void ui_alarm_request_snapshot(const char *reason);

/**
 * 是否存在阻断新打印的未复位报警（1004/1124/1125/1126/1127/1128/1129，MCU 会
 * 拒绝新打印请求，只回一条给不出原因的 EVENT 2003）。打印入口的预检用；返回
 * 第一条阻断条目，无则 NULL。名单是按报警语义硬编码的（报文与字典都没有这个
 * 字段），MCU 新增停机类报警要同步补，见实现处注释。
 */
const ui_alarm_entry_t *ui_alarm_print_blocker(void);

/* ---- 历史（已解除 CONDITION + EVENT，新→旧） ---- */

int ui_alarm_history_count(void);
const ui_alarm_history_entry_t *ui_alarm_history_at(int idx);

/** 删除第 idx 条历史（消息页左滑删除），随手落盘。 */
void ui_alarm_history_delete_at(int idx);

/** 注册观察者（上限 4 个，重复注册幂等）。 */
void ui_alarm_add_observer(ui_alarm_observer_t cb);

/**
 * 调试注入：绕过总线直接走上报处理路径（等价于收到一帧
 * `GLAVO_Alarm_Report_Ntf`），供调试页在 MCU 不发真报警时验证整条 UI 链路。
 * 云端 `alarm.single` 不走本函数，由调试页另发 `UI_Debug_Alarm_Inject_Ntf`。
 * 仅 UI 线程可调；state_mask/clear_mode 语义同 `middleware::kAlarmState*` /
 * `middleware::AlarmClearMode`。注入 state_mask=0（NORMAL）即模拟解除。
 */
void ui_alarm_debug_inject(uint16_t code, uint8_t severity, uint8_t state_mask,
                           uint8_t clear_mode, uint32_t detail);

#ifdef __cplusplus
}
#endif

#endif
