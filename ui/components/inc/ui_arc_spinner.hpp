#ifndef UI_ARC_SPINNER_H
#define UI_ARC_SPINNER_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_arc_spinner.hpp
 * @brief 橙色未封闭旋转加载环（D2/D4 用）
 *
 * 视觉：一段约 270° 的橙色圆弧 + 灰色底环，整体匀速旋转。
 * 实现：基于 lv_arc + lv_anim 控制 rotation。
 */
typedef struct ui_arc_spinner_t {
    lv_obj_t *root;
    lv_anim_t anim;
} ui_arc_spinner_t;

/** 创建并启动旋转。size <= 0 表示使用 USER_SPINNER_SZ */
ui_arc_spinner_t *ui_arc_spinner_create(lv_obj_t *parent, int32_t size);

/** 销毁（停止动画 + 删 root） */
void ui_arc_spinner_destroy(ui_arc_spinner_t *self);

#ifdef __cplusplus
}
#endif

#endif
