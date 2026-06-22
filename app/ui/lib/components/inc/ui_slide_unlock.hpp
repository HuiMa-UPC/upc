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
 * 行为：
 *   - 一条胶囊形轨道（深灰底）。
 *   - 左侧一个橙色圆形 knob，用户按住拖动到最右端时触发 unlock 回调。
 *   - 中部一段提示文字（如 "滑动开门 >>>"），随 knob 推进逐字变橙。
 *   - 中途释放则 knob 用动画弹回左端。
 *   - 解锁后控件自动 disable（防止双触发），由调用方决定是否销毁。
 *
 * 不依赖任何业务模块，可被多个弹窗复用。
 */
typedef void (*ui_slide_unlock_cb_t)(void *user_data);

typedef struct ui_slide_unlock_t ui_slide_unlock_t;

typedef struct {
    const char           *hint_text;   /**< 中间提示文字，NULL = "Slide to Open" */
    ui_slide_unlock_cb_t  on_unlocked; /**< 解锁回调（在 LVGL 主线程） */
    void                 *user_data;
    int32_t               track_w;     /**< 轨道宽，<=0 用 USER_SLIDE_W */
    int32_t               track_h;     /**< 轨道高，<=0 用 USER_SLIDE_H */
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
