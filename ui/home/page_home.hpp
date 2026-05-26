#ifndef PAGE_HOME_H
#define PAGE_HOME_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_home_create(lv_obj_t *parent);

/** 刷新监控区；@p force 为 true 时忽略 ui_generation 跳过逻辑 */
void page_home_refresh(bool force);

#ifdef __cplusplus
}
#endif

#endif
