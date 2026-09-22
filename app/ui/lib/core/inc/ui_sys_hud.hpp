#ifndef UI_SYS_HUD_HPP
#define UI_SYS_HUD_HPP

/**
 * @file ui_sys_hud.hpp
 * @brief 调试用 Host 资源蒙版：红字盖在 `lv_layer_sys()` 右上角。
 *
 * 不进业务布局、不可点、不挡触摸；开/关仅本次运行，由调试页开关驱动。
 * 数据来自 `upc_sys_monitor_sample()`（本机 /proc，不经 IPC）。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 打开或关闭右上角红字蒙版。重复调用幂等。 */
void ui_sys_hud_set_enabled(bool enabled);

bool ui_sys_hud_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_SYS_HUD_HPP */
