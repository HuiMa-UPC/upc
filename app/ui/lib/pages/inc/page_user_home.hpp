#ifndef PAGE_USER_HOME_H
#define PAGE_USER_HOME_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_user_home_create(lv_obj_t *parent);
void page_user_home_refresh(bool force);

#ifdef __cplusplus
}
#endif

#endif
