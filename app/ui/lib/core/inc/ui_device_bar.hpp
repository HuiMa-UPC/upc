#ifndef UI_DEVICE_BAR_H
#define UI_DEVICE_BAR_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_device_bar.hpp
 * @brief 常驻设备信息条：WiFi 图标 + 两组「设备名 + 状态点 + 状态字」。
 *
 * 由 `PageManager` 持有（外壳家具，不是页面的一部分）。第一组是本机打印机，
 * 数据来自 `ui_device_state`，所以每个页面、每个状态看到的都是同一份活数据；
 * 第二组见 `ui_device_bar_set_peer()`。对齐浅色稿 6201:85956。
 *
 * 显隐规则见 `PageManager::switch_to()`：侧栏有图标的页面显示，隐藏页
 * （选文件 / 调试 / 延时摄影）不显示。
 *
 * **本条一律走浅色配色**（深色设备名 + #00c74e 状态绿）。目前只有首页待机态是
 * 浅底，其余四页仍是 `user_color_bg` 的深底、首页打印/监控态底下是整屏摄像头，
 * 那几处的观感要等它们也翻成浅色才正。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 第二台设备的状态等级，只决定状态点与状态字的颜色。
 *
 *  **不复用 `ui_device_status_t`**：那是本机打印机的状态机（Idle / Printing /
 *  Paused ...），稿上第二台那个 `Connected` 不在里面，它的状态集大概率也不是同一套。 */
typedef enum {
    UI_PEER_OK = 0,   /**< 绿（稿上的 Connected） */
    UI_PEER_ERROR,    /**< 红 */
    UI_PEER_OFFLINE,  /**< 灰 */
} ui_peer_level_t;

/** 建条并接上 `ui_device_state` 的观察者；parent 通常是外壳根 `shell_`。 */
lv_obj_t *ui_device_bar_create(lv_obj_t *parent);

/**
 * 设置第二台设备（稿上的 NitroPod）那一组的显示。
 *
 * **数据源尚未接通**：`ui_device_bar_create()` 里先按稿填了占位值。等那台设备的
 * 数据到位后，在它的状态更新处改调本函数一处即可，布局与配色都不用动。
 *
 * @param name   设备名，NULL 或空串 = 整组（含分隔线）隐藏
 * @param status 状态文案，直接上屏，不做映射
 * @param level  只影响配色，见 ui_peer_level_t
 */
void ui_device_bar_set_peer(const char *name, const char *status, ui_peer_level_t level);

void ui_device_bar_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif
