#ifndef PAGE_FILES_H
#define PAGE_FILES_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_files_create(lv_obj_t *parent);

/** Files 页显示/隐藏（隐藏时不刷新右侧进度条，避免切走页面后状态错乱） */
void page_files_set_visible(int visible);

#ifdef __cplusplus
}
#endif

#endif
