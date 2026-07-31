#ifndef UI_DEVICE_STATE_H
#define UI_DEVICE_STATE_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_device_state.hpp
 * @brief 设备名 / 联网 / 运行状态 / 环境指标的唯一来源。
 *
 * 原先这份快照连同 `GLAVO_Env_Update_Ntf` 的订阅一起长在 `page_user_home.cpp`
 * 里，只有首页能读到；常驻状态栏（`ui_device_bar`）要在每个标签页显示同一份
 * 状态，就必须把它抽到页面之外。
 *
 * 为什么不让状态栏自己再订阅一次：`UiMsgProxy::register_env_update` 是**单槽**的
 * （`env_update_cb_ = std::move(cb)`），后注册的会静默顶掉前一个——那样首页的
 * 仪表盘会永远停在 "--"。所以订阅在本模块做一次，再扇出给多个观察者。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 前四项与后端 EnvSnapshotPayload.status 数值对应（见 .cpp 里的强转），
 * 后两项仅由前端在打印结果页本地置位，后端不会推送。 */
typedef enum {
    UI_DEVICE_STATUS_IDLE = 0,
    UI_DEVICE_STATUS_READY,
    UI_DEVICE_STATUS_PRINTING,
    UI_DEVICE_STATUS_ERROR,
    UI_DEVICE_STATUS_PRINT_DONE,       /**< 打印完成（状态点绿） */
    UI_DEVICE_STATUS_PRINT_TERMINATED, /**< 打印终止（状态点红） */
} ui_device_status_t;

typedef struct {
    char               device_name[32];
    ui_device_status_t status;
    bool               wifi_connected;
    int                signal_bars;
    float              pm25_indoor;
    float              pm25_cabin;
    float              temperature_c;
    float              oxygen_percent;
    float              pm10_indoor;      /**< 室内 PM10（<0/NaN=无数据） */
    float              humidity_percent; /**< 舱内湿度（<0/NaN=无数据） */
    float              pressure_kpa;     /**< 舱内气压（<0/NaN=无数据） */
} ui_device_snapshot_t;

/** 观察者：快照变化后在 UI 线程被调用（env 推送已由 Proxy 切回 UI 线程）。 */
typedef void (*ui_device_state_observer_t)(const ui_device_snapshot_t *snap);

/**
 * 装填默认值并订阅环境推送。必须在 `UiMsgProxy::init()` 之后、任何观察者注册
 * 之前调用一次（见 `ui_app.cpp`）。重复调用无副作用。
 */
void ui_device_state_init(void);

/** 当前快照；指针在进程生命周期内恒定，可长期持有。 */
const ui_device_snapshot_t *ui_device_state_get(void);

/**
 * 前端置运行状态。置成打印中/打印完成/打印终止会**锁住**状态字段，
 * 后续 env 推送不再覆盖，直到置回其它值——打印期间界面上的状态由前端说了算。
 */
void ui_device_state_set_status(ui_device_status_t s);

/** 注册观察者（上限 4 个，超出丢弃）。注册时不会立即回调。 */
void ui_device_state_add_observer(ui_device_state_observer_t cb);

const char *ui_device_status_text(ui_device_status_t s);
lv_color_t  ui_device_status_color(ui_device_status_t s);

#ifdef __cplusplus
}
#endif

#endif
