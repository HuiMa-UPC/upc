#ifndef PAGE_FILES_H
#define PAGE_FILES_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_files_create(lv_obj_t *parent);

/** Files 页显示/隐藏（隐藏时不刷新右侧进度条，避免切走页面后状态错乱） */
void page_files_set_visible(int visible);

/** 刷新 Files 页（G-code / CAL 子视图） */
void page_files_refresh(void);

/**
 * Reset / 外部触发的 gcode_start_parse：挂接解析进度。
 * 须在释放 JOB_LOCK 之后调用（内部 refresh_detail_panel 会再次加锁）。
 */
void page_files_notify_parse_restarted(void);

#ifdef __cplusplus
}
#endif

#endif
