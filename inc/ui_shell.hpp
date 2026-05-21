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

/** 获取右侧内容根容器（各 page 模块也可只用 parent 参数） */
lv_obj_t *ui_shell_content_root(void);

void ui_shell_show_page(ui_page_id_t page);

#ifdef __cplusplus
}
#endif

#endif
