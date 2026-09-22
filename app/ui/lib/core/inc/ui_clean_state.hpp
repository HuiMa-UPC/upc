#ifndef UI_CLEAN_STATE_H
#define UI_CLEAN_STATE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_clean_state.hpp
 * @brief 清粉（clean_test）的发起与回执在 UI 侧的唯一来源。
 *
 * `UiMsgProxy::register_clean_test_start_rsp` / `register_clean_test_done` 都是
 * **单槽**的（`xxx_cb_ = std::move(cb)`），谁后注册谁把前一个静默顶掉。而清粉有
 * 两个发起方——监控页打印结果态的吹粉（带清粉录像）和 Manual 页的手动吹粉（不录像）
 * ——各自注册必然互相顶掉。所以订阅在本模块做一次（`ui_clean_state_init()`，由
 * `ui_app.cpp` 调），再按「谁发起的」扇出给观察者。形态同 `ui_device_state`。
 *
 * 发送也收在本模块（`ui_clean_state_request()`），不留 `send_clean_test_start()` 的
 * 旁路：owner 归属必须与实际发出的那一帧绑定，否则绕过去发一帧就会让后面的回执认错人。
 *
 * MCU 同时只跑一路清粉（Glavo 侧 `clean_test_active_` 单例互斥），所以「正在跑的
 * 发起方」只有一个；期间别人发起会收到 `UI_CLEAN_EVT_REJECTED`。
 */

/** 清粉发起方。 */
typedef enum {
    UI_CLEAN_OWNER_NONE = 0,
    UI_CLEAN_OWNER_PRINT_RESULT, /**< 监控页打印结果态的吹粉：带清粉录像、进打印历史 */
    /** 监控页 Manual 的手动吹粉：不录像、不落盘、不进历史。
     *  改版前这一档叫 `CONTROL`（控制页的手动清粉），控制页退役后语义原样搬过来。 */
    UI_CLEAN_OWNER_MANUAL,
} ui_clean_owner_t;

/** 回执类型。 */
typedef enum {
    UI_CLEAN_EVT_ACCEPTED = 0, /**< 后端受理，MCU 已开始清粉 */
    UI_CLEAN_EVT_REJECTED,     /**< 后端拒绝：已有清粉在跑 */
    UI_CLEAN_EVT_DONE,         /**< MCU 清粉结束（result_code 00/01/02 一律算结束） */
} ui_clean_event_t;

/**
 * 观察者。**保证在 UI 线程被调用**——本模块内部已做过 `lv_async_call` 回跳，所以
 * 回调里可以直接建/毁 LVGL 对象，不必自己再跳一次。
 *
 * `owner` 是该事件对应的发起方：`REJECTED` 给的是被拒的那次请求的发起方，
 * `ACCEPTED`/`DONE` 给的是正在跑的那一路。**不是自己发起的事件请直接忽略。**
 */
typedef void (*ui_clean_state_observer_t)(ui_clean_event_t ev, ui_clean_owner_t owner);

/** 接管那两个单槽回调。须在 `UiMsgProxy::init()` 之后调用一次；重复调用无副作用。 */
void ui_clean_state_init(void);

/** 注册观察者（上限 4 个，幂等；注册时不会立即回调）。 */
void ui_clean_state_add_observer(ui_clean_state_observer_t cb);

/**
 * 发起清粉。`owner` 同时决定是否录像：`PRINT_RESULT` 录，`CONTROL` 不录。
 * `job_name` 只在录像那一档有意义（决定 `/data/media/video/{job_name}/`），
 * 不录像时后端根本不读它，传 nullptr 即可。
 *
 * 本函数**不做本地忙判**，一律发出去让后端裁决：本地标志与后端
 * `clean_test_active_` 一旦drift（比如 DONE 丢了）就会把自己锁死，代价比多发一帧大。
 */
void ui_clean_state_request(ui_clean_owner_t owner, const char *job_name);

/** 正在跑的发起方；`UI_CLEAN_OWNER_NONE` 表示当前空闲。 */
ui_clean_owner_t ui_clean_state_active_owner(void);

#ifdef __cplusplus
}
#endif

#endif
