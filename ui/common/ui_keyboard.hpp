#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化全局键盘（在 lv_scr_act() 上创建）
 * @param parent 父对象，通常为 lv_scr_act()
 */
void ui_keyboard_init(lv_obj_t *parent);

/**
 * @brief 将键盘附加到 textarea
 * @param ta       文本区域对象
 * @param mode     键盘模式（LV_KEYBOARD_MODE_NUMBER / LV_KEYBOARD_MODE_TEXT_LOWER）
 */
void ui_keyboard_attach(lv_obj_t *ta, lv_keyboard_mode_t mode);

/**
 * @brief 强制隐藏键盘（页面切换时调用）
 */
void ui_keyboard_hide(void);

/** 若 @p ta 正绑定键盘，则隐藏并取消其聚焦（定时器锁定某输入框时调用） */
void ui_keyboard_release_if_focused(lv_obj_t *ta);

/** @p ta 是否为当前绑定且已显示的键盘目标 */
bool ui_keyboard_is_bound(const lv_obj_t *ta);

/** 键盘是否正在显示（任意输入框） */
bool ui_keyboard_is_open(void);

#ifdef __cplusplus
}
#endif

#endif
