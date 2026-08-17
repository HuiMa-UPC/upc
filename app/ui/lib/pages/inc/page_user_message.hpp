#ifndef PAGE_USER_MESSAGE_H
#define PAGE_USER_MESSAGE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 消息中心页：设备事件 + MCU 报警混排列表 + 详情抽屉（Reset） */
lv_obj_t *page_user_message_create(lv_obj_t *parent);

void page_user_message_refresh(bool force);

/** 离页：关抽屉、收起左滑、停止观察者驱动的重建 */
void page_user_message_exit(void);

#ifdef __cplusplus
}
#endif

#endif
