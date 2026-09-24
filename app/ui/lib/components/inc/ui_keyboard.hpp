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

/**
 * @brief 键盘显隐通知回调
 *
 * @param shown       true=键盘已显示，false=已收起
 * @param obstructed  键盘遮住的屏幕区域；shown=false 时为 NULL。
 *                    两种模式遮的方向不同——文本键盘是**屏幕下半**的横条，
 *                    数字键盘是**屏幕右侧**的整列竖板，所以给的是完整矩形而不是
 *                    一个高度，让调用方自己决定往上让还是往左让。
 * @param user_data   注册时透传
 */
typedef void (*ui_keyboard_visibility_cb_t)(bool shown, const lv_area_t *obstructed,
                                            void *user_data);

/**
 * @brief 注册键盘显隐回调（**单槽**，后注册的顶掉先注册的；传 NULL 注销）
 *
 * 给「弹窗被键盘挡住」这类需要让位的调用方用。弹窗关闭时务必注销，否则回调会打到
 * 已释放的对象上。目前只有设置页那两个密码弹窗在用。
 */
void ui_keyboard_set_visibility_cb(ui_keyboard_visibility_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
