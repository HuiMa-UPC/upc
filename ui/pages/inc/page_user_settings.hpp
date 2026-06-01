#ifndef PAGE_USER_SETTINGS_H
#define PAGE_USER_SETTINGS_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 用户设置页：当前提供 "进入开发者模式"（带密码） 与若干占位项 */
lv_obj_t *page_user_settings_create(lv_obj_t *parent);

void page_user_settings_refresh(bool force);

/** 关闭密码对话框（页面 on_exit 时调用） */
void page_user_settings_dismiss_dialog(void);

#ifdef __cplusplus
}
#endif

#endif
