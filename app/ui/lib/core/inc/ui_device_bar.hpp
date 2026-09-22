#ifndef UI_DEVICE_BAR_H
#define UI_DEVICE_BAR_H

#include "lvgl_wrap.hpp"

/**
 * @file ui_device_bar.hpp
 * @brief 常驻设备信息条：WiFi 图标 + 一组「设备名 + 状态点 + 状态字」。
 *
 * 由 `PageManager` 持有（外壳家具，不是页面的一部分）。数据来自 `ui_device_state`，
 * 所以每个页面、每个状态看到的都是同一份活数据。对齐新稿 6230:89954。
 *
 * **上一版稿（6201:85956）是两组**（`SnowPod · Stand-by | NitroPod · Connected`），
 * 第二组的数据源一直没接通、是写死的占位。新稿把它连同中间那道竖分隔线一起去掉了，
 * 于是 `ui_device_bar_set_peer()` 与 `ui_peer_level_t` 一并退场——要再加回第二台设备
 * 就翻 git 历史，布局代码还在那儿。
 *
 * 显隐规则见 `PageManager::apply_device_bar()`：侧栏有图标的页面显示，隐藏页
 * （选文件 / 调试 / 延时摄影）与历史 / 消息 / 设置 / 监控四页不显示。
 *
 * **本条一律走浅色配色**（深色设备名 + #00c74e 状态绿）。 */

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
