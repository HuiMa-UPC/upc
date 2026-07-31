#ifndef UI_DEVICE_BAR_H
#define UI_DEVICE_BAR_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_device_bar.hpp
 * @brief 常驻设备信息条：WiFi 图标 + 设备名 + 状态点 + 状态字。
 *
 * 由 `PageManager` 持有（外壳家具，不是页面的一部分），数据来自
 * `ui_device_state`，所以每个页面、每个状态看到的都是同一份活数据。
 * 对齐稿 3540:14473 的左半边；右半边留给页面自己的动作按钮（首页的
 * 摄像头圆钮 / Start Print 仍建在首页那条透明行里，与本条同高对齐）。
 *
 * 显隐规则见 `PageManager::switch_to()`：侧栏有图标的页面显示，隐藏页
 * （选文件 / 调试 / 延时摄影）不显示。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 建条并接上 `ui_device_state` 的观察者；parent 通常是外壳根 `shell_`。 */
lv_obj_t *ui_device_bar_create(lv_obj_t *parent);

void ui_device_bar_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif
