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
 * 消费方有三个，都靠观察者刷新：右上角的通知三态（`ui_notice_stack` 的 toast 与
 * 铃铛面板）、侧栏红点、消息中心页的列表。
 *
 * **自动收起也在本模块**：每条消息进入 SHOWN 时记一个 tick，一支扫描定时器按等级
 * 到点把它收进铃铛（INFO 5s、告警级 15s，见 .cpp 的 kCollapseMs）。放在这里而不是
 * 显示层，是因为待机态的新三态与打印态的旧浮层共用同一份数据，时序必须一致。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 等级。INFO 的 toast 右上带一颗关闭钮，其余三档不带、只能等自动收起。 */
typedef enum {
    UI_MSG_SEV_INFO = 0,
    UI_MSG_SEV_ERROR,
    UI_MSG_SEV_CRITICAL,
    UI_MSG_SEV_EMERGENCY,
} ui_msg_severity_t;

/**
 * 这条消息此刻在 toast 上的去留，以及**为什么**离开的。
 *
 * 两种离开必须分开记，不能合并成一个 bool：`ui_message_sync()` 判断「要不要当作
 * 又发生了一次」时，只认用户亲手关掉的那种。自动收起是系统行为、不代表用户处理过，
 * 所以打印流程那种每秒好几次的无脑 sync、以及 30s 快照对齐，收起之后都不该把
 * toast 重新顶出来——合并成 bool 的话它们会变成周期性刷屏。
 */
typedef enum {
    UI_MSG_SHOWN = 0,      /**< 正挂在 toast 上 */
    UI_MSG_CLOSED_BY_USER, /**< 用户点了 × （仅 INFO 可关） */
    UI_MSG_AUTO_COLLAPSED, /**< 停留到时限，自动收进铃铛 */
} ui_msg_visual_t;

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
    ui_msg_visual_t   visual;    /**< 在不在 toast 上、以及离开的原因 */
    uint32_t          shown_tick;/**< 进入 SHOWN 的 lv_tick，自动收起的计时起点 */
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

/* ---- 两种写入语义 ----
 *
 * 分家的理由是调用方本来就分两类，而它们对「又发生了一次」的期待正好相反：
 *
 *   sync —— 状态同步。本地没有就新建并弹一次，已经有了就原位更新文字/等级，
 *           **不碰 visual、不动已读与排序、不重弹**。给那些「反复宣告同一个状态
 *           还在」的调用方用：打印流程每次态刷新都无脑推的那两条本地提示、30s 报警
 *           快照对齐、CONDITION 同一轮内的状态迁移。
 *
 *   emit —— 又发生了一次。无条件把 visual 拉回 SHOWN、回未读、序号顶新、重弹。
 *           给真事件用：MCU 的 EVENT 每帧、CONDITION 从 RTN_LATCHED 回到 ACTIVE
 *           那个上升沿、后端设备事件的 active=true。
 *
 * 两者各有「码表出文案」和「调用方给文案」两个版本，后者带 _ex 后缀。
 */

/**
 * 状态同步（码表版）：本地没有就新建并弹一次，已有就静默更新。
 *
 * 等级和文案都由本模块的码表出，不由调用方给：线协议里那个 severity 字段是留给
 * 云端的，UI 侧同一个码必须恒定同一种观感，否则后端改个字段卡片就换色了。
 * 满了（上限 24 条）顶掉最旧的一条已读消息。
 *
 * @param code    `middleware::DeviceEventCode` 数值；不在码表里的码直接忽略
 * @param ts_ms   后端给的触发时刻；本地消息传 0
 */
void ui_message_sync(uint16_t code, int64_t ts_ms);

/** 又发生了一次（码表版）：重新露面、回未读、序号顶新。参数同 `ui_message_sync()`。 */
void ui_message_emit(uint16_t code, int64_t ts_ms);

/**
 * 状态同步（外部文案版）。MCU 报警走这一支：severity/文字都以报文 + 报警字典为准，
 * 进不了本模块那张 DeviceEventCode 码表。
 *
 * title/body 只存指针不拷贝，调用方必须保证其在消息存续期内有效
 * （`ui_alarm_state` 用的是自己那张固定槽位表）。code 与 DeviceEventCode
 * 共用键空间，报警号从 1000 起，天然不冲突。
 */
void ui_message_sync_ex(uint16_t code, ui_msg_severity_t severity,
                        const char *title, const char *body, int64_t ts_ms);

/** 又发生了一次（外部文案版）。参数同 `ui_message_sync_ex()`。 */
void ui_message_emit_ex(uint16_t code, ui_msg_severity_t severity,
                        const char *title, const char *body, int64_t ts_ms);

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

/**
 * 用户点 × 把一条消息从 toast 上关掉（置 `UI_MSG_CLOSED_BY_USER`）。
 * **只受理 INFO**——稿上只有信息卡带关闭钮，告警卡靠自动收起离场。
 * @return 是否真的关掉了
 */
bool ui_message_dismiss(uint16_t code);

/**
 * 停留到时限，把一条消息收进铃铛（置 `UI_MSG_AUTO_COLLAPSED`）。**不看等级**。
 * 正常由本模块的扫描定时器自己调，暴露出来是给调试页造时序用。
 * @return 是否真的收起了
 */
bool ui_message_collapse(uint16_t code);

/** toast 上还挂着的（visual == SHOWN）消息条数。 */
int ui_message_visible_count(void);

/**
 * 取第 idx 条 toast 可见消息，已按浮层顺序排好：
 * 口罩提醒钉在第一位，其余先按等级降序、同级比新。越界返回 NULL。
 */
const ui_message_t *ui_message_visible_at(int idx);

/* ---- 「还需要用户留意」这一组 ----
 *
 * 判据是「没看过 **或** 是还没解除的告警级」。后半句不用额外判断解除与否：
 * CONDITION 解除时 `ui_alarm_state` 会 `ui_message_retract()` 把条目删掉，所以
 * 还在表里就等于还没解除。
 *
 * 铃铛与侧栏那两颗红点、以及铃铛面板的列表共用这一组，所以「红点亮」与「面板里
 * 有东西」恒等价，不会出现点开是空的。 */

/** 需要留意的条数。> 0 即两颗红点都亮。 */
int ui_message_attention_count(void);

/** 取第 idx 条需要留意的消息，按 seq 降序（新的在前）。越界返回 NULL。 */
const ui_message_t *ui_message_attention_at(int idx);

/** 红点谓词，等价于 `ui_message_attention_count() > 0`。 */
bool ui_message_needs_attention(void);

/** 在库总条数（含已离开 toast 的；消息页列表用）。 */
int ui_message_total_count(void);

/** 按存储序取第 idx 条（不排序、含已离开 toast 的）；越界返回 NULL。 */
const ui_message_t *ui_message_at(int idx);

/** 未读条数（与在不在 toast 上无关——离开 toast 不代表看过了）。 */
int ui_message_unread_count(void);

/** 全部标已读（进消息页时调用），顺带熄灭侧栏红点。 */
void ui_message_mark_all_read(void);

/** 注册观察者（上限 4 个，超出丢弃）。注册时不会立即回调。 */
void ui_message_add_observer(ui_message_observer_t cb);

#ifdef __cplusplus
}
#endif

#endif
