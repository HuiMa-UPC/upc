#ifndef PAGE_SETTINGS_H
#define PAGE_SETTINGS_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_settings_create(lv_obj_t *parent);

/** 刷新连接状态与参数表单可编辑性 */
void page_settings_refresh(bool force);

#ifdef __cplusplus
}
#endif

#endif
