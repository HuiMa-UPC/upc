#ifndef UI_BOARD_ANIM_H
#define UI_BOARD_ANIM_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_board_anim.hpp
 * @brief "打印板放入" 动画占位
 *
 * 目前是 LVGL 纯绘制的简易示意：
 *   - 上方一个橙色矩形（代表打印板）反复上下滑入一个下方的"槽"
 *   - 底部一行 caption "放入打印板示意"
 *   - 留了 lv_image 容器，外部可通过 ui_board_anim_set_image() 喂真实资源
 *
 * 当 ui_board_anim_set_image 被调用后，几何示意会隐藏，改显示该 lv_image。
 */
typedef struct ui_board_anim_t ui_board_anim_t;

/** 创建组件；返回句柄。size <= 0 时使用默认 (USER_HOME_DEVICE_W/2, ...) */
ui_board_anim_t *ui_board_anim_create(lv_obj_t *parent,
                                      int32_t width, int32_t height);

/** 取得根节点供布局 */
lv_obj_t *ui_board_anim_get_root(ui_board_anim_t *self);

/** 切换到真实图片资源（未实现 GIF/Lottie 之前的过渡方案） */
void ui_board_anim_set_image(ui_board_anim_t *self, const void *image_src);

/** 销毁 */
void ui_board_anim_destroy(ui_board_anim_t *self);

#ifdef __cplusplus
}
#endif

#endif
