#ifndef PAGE_USER_MESSAGE_H
#define PAGE_USER_MESSAGE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 消息中心页：设备事件 + MCU 报警混排列表 + 详情抽屉（Reset） */
lv_obj_t *page_user_message_create(lv_obj_t *parent);

void page_user_message_refresh(bool force);

/** 离页：关抽屉、收起左滑、退出管理态、停止观察者驱动的重建 */
void page_user_message_exit(void);

/**
 * 下次进页时落在 All Tab 顶部（首页铃铛面板点消息条 / 「查看剩余」时用）。调用方先调它
 * 再 switch_to("user_message")；只作用于紧接着的那一次进页。
 */
void page_user_message_request_all_tab(void);

#ifdef __cplusplus
}
#endif

#endif
