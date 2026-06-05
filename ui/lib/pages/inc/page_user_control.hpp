#ifndef PAGE_USER_CONTROL_H
#define PAGE_USER_CONTROL_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 用户控制页：当前为骨架（标题 + 占位卡片），后续接用户级控制项 */
lv_obj_t *page_user_control_create(lv_obj_t *parent);

void page_user_control_refresh(bool force);

#ifdef __cplusplus
}
#endif

#endif
