#ifndef UI_SLIDE_UNLOCK_H
#define UI_SLIDE_UNLOCK_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_slide_unlock.hpp
 * @brief 滑动解锁控件（用于 D1 "滑动解锁开门"）
 *
 * 行为（对齐 Figma「滑动控件示意」）：
 *   - 一条半透明玻璃胶囊轨道。
 *   - 左端一段红色玻璃胶囊，按住向右拖动时"生长"，前沿圆头带开门图标与高光。
 *   - 静止态显示提示文字 + 3 个 chevron 箭头；拖动中隐藏；到底变 "RELEASE TO OPEN"。
 *   - 滑到底不自动触发，需在末端「松手」才触发 unlock 回调；中途松手则动画回弹。
 *   - 解锁后控件自动 disable（防止双触发），由调用方决定是否销毁/复位。
 *
 * 不依赖任何业务模块，可被多个弹窗复用。
 */
typedef void (*ui_slide_unlock_cb_t)(void *user_data);

typedef struct ui_slide_unlock_t ui_slide_unlock_t;

typedef struct {
    const char           *hint_text;   /**< 静止态提示文字，NULL = "SWIPE TO OPEN" */
    ui_slide_unlock_cb_t  on_unlocked; /**< 解锁回调（在 LVGL 主线程） */
    void                 *user_data;
    int32_t               track_w;     /**< 轨道宽，<=0 用 USER_SLIDE_W */
    int32_t               track_h;     /**< 轨道高，<=0 用 USER_SLIDE_H */
    bool                  custom_accent; /**< true 时用 accent_color 替代默认橙色 */
    lv_color_t            accent_color;  /**< knob/解锁文字配色（custom_accent=true 生效） */
} ui_slide_unlock_cfg_t;

/** 在 parent 上创建一个滑动解锁条；返回句柄供后续销毁/查询用 */
ui_slide_unlock_t *ui_slide_unlock_create(lv_obj_t *parent,
                                          const ui_slide_unlock_cfg_t *cfg);

/** 取得控件根节点（用于布局对齐：lv_obj_align 等） */
lv_obj_t *ui_slide_unlock_get_root(ui_slide_unlock_t *self);

/** 复位：knob 弹回起点、重新可滑（用于可重复触发的场景，如首页开门） */
void ui_slide_unlock_reset(ui_slide_unlock_t *self);

/** 销毁控件（root 会被 lv_obj_delete） */
void ui_slide_unlock_destroy(ui_slide_unlock_t *self);

#ifdef __cplusplus
}
#endif

#endif
