#ifndef PAGE_USER_SETTINGS_H
#define PAGE_USER_SETTINGS_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 用户设置页：左栏分类导航 + 右栏详情（稿 6403 / 6457 一族，页面树见 UI.md「设置页布局」） */
lv_obj_t *page_user_settings_create(lv_obj_t *parent);

void page_user_settings_refresh(bool force);

/** 离开设置页（页面 on_exit 时调用）：关掉本页所有弹窗，停 Wi-Fi 周期重扫与版本查询重发 */
void page_user_settings_dismiss_dialog(void);

#ifdef __cplusplus
}
#endif

#endif
