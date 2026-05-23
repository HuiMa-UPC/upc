#ifndef PAGE_FILES_H
#define PAGE_FILES_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_files_create(lv_obj_t *parent);

/** Files 页显示/隐藏（隐藏时不刷新右侧进度条，避免切走页面后状态错乱） */
void page_files_set_visible(int visible);

/** Reset / 外部触发的 gcode_start_parse：立即挂接解析进度与完成 toast */
void page_files_notify_parse_restarted(void);

#ifdef __cplusplus
}
#endif

#endif
