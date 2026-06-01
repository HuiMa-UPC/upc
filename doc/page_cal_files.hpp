#ifndef PAGE_CAL_FILES_H
#define PAGE_CAL_FILES_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 在 parent 内构建 CAL Files 页（左列表 + 右详情/Open） */
void page_cal_files_build(lv_obj_t *parent);

/** 注册「返回目录选择」回调（由 page_files 在 build 前设置） */
void page_cal_files_set_back_cb(void (*cb)(void));

/** 按当前 CAL 扫描目录重建文件列表 */
void page_cal_files_refresh_list(void);

/** 刷新 CAL 详情区 Open 按钮等轻量 UI */
void page_cal_files_refresh_ui(void);

#ifdef __cplusplus
}
#endif

#endif
