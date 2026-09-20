#ifndef UI_BACKLIGHT_HPP
#define UI_BACKLIGHT_HPP

/**
 * @file ui_backlight.hpp
 * @brief LCD 背光：读/写 `/sys/class/backlight/.../brightness`（Snowpod 文档 0~255）。
 *
 * UI 与 Glavo 同机，不经 IPC、**不依赖 MCU 版本**。滑条用 5~100%（下限避免关死屏幕）。
 */

namespace ui {

/** sysfs 原始最大值；节点不存在时返回 0。 */
int backlight_raw_max();

/** 当前亮度百分比 1~100；读失败返回 -1。 */
int backlight_get_percent();

/** 写入亮度百分比（内部钳到 1~100）并记住，供下次开机恢复。 */
bool backlight_set_percent(int percent);

/** 启动时若有记住的百分比则应用；否则保持内核当前值。 */
void backlight_apply_saved();

} // namespace ui

#endif
