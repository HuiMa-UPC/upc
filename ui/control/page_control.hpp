#ifndef PAGE_CONTROL_H
#define PAGE_CONTROL_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_control_create(lv_obj_t *parent);

/** 刷新控件门控与作业区标签 */
void page_control_refresh(void);

#ifdef __cplusplus
}
#endif

#endif
