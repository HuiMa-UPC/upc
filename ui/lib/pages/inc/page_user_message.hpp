#ifndef PAGE_USER_MESSAGE_H
#define PAGE_USER_MESSAGE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 用户信息（消息/通知）页：当前为骨架，后续接报警上报 */
lv_obj_t *page_user_message_create(lv_obj_t *parent);

void page_user_message_refresh(bool force);

#ifdef __cplusplus
}
#endif

#endif
