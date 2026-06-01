#ifndef UI_USER_SKELETON_H
#define UI_USER_SKELETON_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 用户 Tab 占位页：标题 + 半透明卡片 + 居中提示 */
lv_obj_t *ui_user_skeleton_create(lv_obj_t *parent,
                                  const char *title,
                                  const char *hint);

#ifdef __cplusplus
}
#endif

#endif
