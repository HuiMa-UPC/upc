#ifndef PAGE_USER_HISTORY_H
#define PAGE_USER_HISTORY_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 用户历史页：当前为骨架（标题 + 占位卡片），后续接打印记录 */
lv_obj_t *page_user_history_create(lv_obj_t *parent);

void page_user_history_refresh(bool force);

#ifdef __cplusplus
}
#endif

#endif
