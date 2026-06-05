#ifndef PAGE_USER_HOME_H
#define PAGE_USER_HOME_H

#include "lvgl_wrap.hpp"
#include "control.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_user_home_create(lv_obj_t *parent);
void page_user_home_refresh(bool force);

void page_user_home_enter_printing_mode(const control_file_t *file);
void page_user_home_exit_printing_mode(void);
void page_user_home_printing_update(const control_progress_t *p);

#ifdef __cplusplus
}
#endif

#endif
