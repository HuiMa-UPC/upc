#ifndef PAGE_USER_TIMELAPSE_H
#define PAGE_USER_TIMELAPSE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 延时摄影播放器（历史页隐藏子页 user_timelapse）：全屏 PNG 序列 + 顶部标题/时间 +
 * 右上关闭 + 中央播放/暂停 + 底部进度条。点画面显隐控件。
 *
 * 进入前由历史页调用 page_user_timelapse_set_source 传入序列目录。
 */
lv_obj_t *page_user_timelapse_create(lv_obj_t *parent);
void page_user_timelapse_refresh(bool force);
void page_user_timelapse_exit(void);

/**
 * 设置待播序列目录（switch_to("user_timelapse") 之前调用）。
 * @param sequence_dir_abs 绝对路径，如 "/home/video/a_20260722_111500"
 * @param title          顶部标题（如作业名）
 * @param subtitle       顶部副标题（如时间）
 */
void page_user_timelapse_set_source(const char *sequence_dir_abs,
                                    const char *title,
                                    const char *subtitle);

#ifdef __cplusplus
}
#endif

#endif
