#ifndef PAGE_USER_HOME_H
#define PAGE_USER_HOME_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_user_home_create(lv_obj_t *parent);
void page_user_home_refresh(bool force);
/**
 * 按打印态切底部那一行（稿 6578:41426 待机 / 6683:34165 打印中 / 6683:34027 打印结束）：
 *  - 待机：左下深色 PRINT + 右下可用的开门滑块；
 *  - 打印中（`control_is_printing()`）：左下换成白底 MONITOR，开门滑块置灰不可拖；
 *  - 出结果待取件（`control_print_result_ready()`）：滑块换成 RETRIEVE。
 * 由 control.cpp 在打印态变化时调用，本页 refresh 也会调。**只动这几颗控件**，不碰通知栈
 * 与侧栏档位（refresh 会复位它们，在别的页面上调会干扰那一页）。页面还没建时什么也不做。
 */
void page_user_home_sync_print_state(void);

#ifdef __cplusplus
}
#endif

#endif
