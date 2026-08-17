#ifndef UI_MESSAGE_STORE_H
#define UI_MESSAGE_STORE_H

#include "lvgl_wrap.hpp"

#include <stdint.h>

/**
 * @file ui_message_store.hpp
 * @brief 设备消息中心的唯一数据源：一条一条的事件/告警，去重、排序、已读计数。
 *
 * 数据从两处进来：
 *   1. 后端 `GLAVO_Device_Event_Ntf`（码 + 等级 + 触发时刻），码表见
 *      `doc/设备通信协议-数据结构定义（v0.9）.md` 第五节；
 *   2. 前端自己 push 的本地消息（打印结束催吹粉、吹粉完成提醒戴口罩），
 *      码段从 `DeviceEventCode::UiWearMask` 起，后端不会发这一段。
 *
 * **文案不进线协议**：总线上只跑数值码，人话由本模块的码表（kCatalog）出。
 * 这样后端换文案不用动 IPC，多语言以后也只改这一张表。
 *
 * 消费方有两个：左下角通知浮层（`ui_notice_stack`）和侧栏红点，都靠观察者刷新。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 等级。UI 只分两种观感：INFO 蓝色可关卡片，其余红色告警卡片、不可关。 */
typedef enum {
    UI_MSG_SEV_INFO = 0,
    UI_MSG_SEV_ERROR,
    UI_MSG_SEV_CRITICAL,
    UI_MSG_SEV_EMERGENCY,
} ui_msg_severity_t;

typedef struct {
    uint16_t          code;      /**< middleware::DeviceEventCode 的数值 */
    ui_msg_severity_t severity;
    int64_t           ts_ms;     /**< 后端给的触发时刻（Unix 毫秒）；本地消息为 0=未知 */
    /** 到达序号，越大越新。排序一律用它而不是 ts_ms：后端给的是 Unix 时间、
     *  本地消息压根没有时间，两种纪元混在一起比大小会把本地消息全排到最旧。 */
    uint32_t          seq;
    const char       *title;     /**< 码表出的标题（英文），恒非空 */
    const char       *body;      /**< 码表出的副标题，可为空串 */
    bool              unread;    /**< 进过一次消息页就全清 */
    bool              dismissed; /**< 信息卡被 × 掉：只从浮层撤下，列表里还留着 */
} ui_message_t;

/**
 * 等级配色（四档阶梯，见 app/ui/UI.md「通知徽标配色」）：
 * INFO 蓝 → ERROR 黄 → CRITICAL 橙 → EMERGENCY 红。
 *
 * 消息页列表与左下通知浮层共用这一份，同一条消息在两处必须同色。相邻两档不能
 * 只靠明度区分——暗背景下暗色视觉后退，与"更严重"的直觉相反。
 */
lv_color_t ui_message_severity_color(ui_msg_severity_t sev);

/** 等级图标：INFO 出消息气泡，其余一律 alert-triangle（稿 3058:7058 / 7233）。 */
const lv_image_dsc_t *ui_message_severity_icon(ui_msg_severity_t sev);

/** 观察者：消息集合变化后在 UI 线程被调用。 */
typedef void (*ui_message_observer_t)(void);

/**
 * 订阅后端事件推送。须在 `UiMsgProxy::init()` 之后调用一次（见 `ui_app.cpp`）。
 * 重复调用无副作用。
 */
void ui_message_store_init(void);

/**
 * 记一条消息，重复 push 同一个 code 是幂等的（已在浮层上就什么也不做，
 * 被 × 掉过的会重新露面并回到未读）。满了（上限 24 条）顶掉最旧的一条已读消息。
 *
 * 等级和文案都由本模块的码表出，不由调用方给：线协议里那个 severity 字段是留给
 * 云端的，UI 侧同一个码必须恒定同一种观感，否则后端改个字段卡片就换色了。
 *
 * @param code    `middleware::DeviceEventCode` 数值；不在码表里的码直接忽略
 * @param ts_ms   后端给的触发时刻；本地消息传 0
 */
void ui_message_push(uint16_t code, int64_t ts_ms);

/**
 * 记一条**文案由调用方提供**的消息（MCU 报警走这里：severity/文字都以报文 +
 * 报警字典为准，进不了本模块那张 DeviceEventCode 码表）。
 *
 * title/body 只存指针不拷贝，调用方必须保证其在消息存续期内有效
 * （`ui_alarm_state` 用的是自己那张固定槽位表）。code 与 DeviceEventCode
 * 共用键空间，报警号从 1000 起，天然不冲突。
 *
 * @param renew  true = 视为一次新发生：顶到同级最前并回未读（EVENT 重复触发）；
 *               false = 原位更新文字/等级，不动已读与排序（CONDITION 状态迁移）
 */
void ui_message_push_ex(uint16_t code, ui_msg_severity_t severity,
                        const char *title, const char *body,
                        int64_t ts_ms, bool renew);

/**
 * 调试注入一条模拟消息（调试页 `Msg Inject`）：文案取自码表但标题加 `[SIM] `
 * 前缀，与真事件区分。撤回仍用 `ui_message_retract()`。
 *
 * 前缀文案存在本模块的固定槽位里（`ui_message_t` 只存指针），槽位数够注入表用；
 * 满了就复用最老的一格。
 */
void ui_message_debug_push(uint16_t code);

/**
 * 撤回一条消息（stateful 事件的 `active=false` 边沿，或前端态变了）。
 * code 不存在时什么也不做。
 */
void ui_message_retract(uint16_t code);

/** 把一条信息消息从浮层撤下（点 × 触发）。告警级不受理，返回值恒为撤下与否。 */
bool ui_message_dismiss(uint16_t code);

/** 浮层可见（未被 × 掉）的消息条数。 */
int ui_message_visible_count(void);

/**
 * 取第 idx 条可见消息，已按浮层顺序排好：
 * 口罩提醒钉在第一位，其余先按等级降序、同级比新。越界返回 NULL。
 */
const ui_message_t *ui_message_visible_at(int idx);

/** 在库总条数（含被 × 掉的；消息页列表用，浮层用 visible 那组）。 */
int ui_message_total_count(void);

/** 按存储序取第 idx 条（不排序、含 dismissed）；越界返回 NULL。 */
const ui_message_t *ui_message_at(int idx);

/** 未读条数（含被 × 掉的——× 只影响浮层，不代表看过了）。 */
int ui_message_unread_count(void);

/** 全部标已读（进消息页时调用），顺带熄灭侧栏红点。 */
void ui_message_mark_all_read(void);

/** 注册观察者（上限 4 个，超出丢弃）。注册时不会立即回调。 */
void ui_message_add_observer(ui_message_observer_t cb);

#ifdef __cplusplus
}
#endif

#endif
