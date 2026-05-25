#ifndef UI_SHELL_H
#define UI_SHELL_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_CONTROL,
    UI_PAGE_FILES,
    UI_PAGE_SETTINGS,
    UI_PAGE_SERIAL,
    UI_PAGE_TCP,
    UI_PAGE_COUNT
} ui_page_id_t;

/** 初始化横屏壳：左侧导航 + 右侧页面区 */
void ui_shell_init(void);

#ifdef __cplusplus
}
#endif

#endif
