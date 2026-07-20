#ifndef PAGE_USER_TIMELAPSE_H
#define PAGE_USER_TIMELAPSE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 延时摄影播放器（历史页隐藏子页 user_timelapse）：全屏视频 + 顶部标题/时间 +
 * 右上下载(占位)/关闭 + 中央播放/暂停 + 底部进度条。点画面显隐控件。
 *
 * 进入前由历史页调用 page_user_timelapse_set_source 传入待播视频。
 */
lv_obj_t *page_user_timelapse_create(lv_obj_t *parent);
void page_user_timelapse_refresh(bool force);
void page_user_timelapse_exit(void);

/**
 * 设置待播视频（switch_to("user_timelapse") 之前调用）。
 * @param video_abs_path 绝对路径，如 "/home/video/a.mp4"
 * @param title          顶部标题（如作业名）
 * @param subtitle       顶部副标题（如时间）
 */
void page_user_timelapse_set_source(const char *video_abs_path,
                                    const char *title,
                                    const char *subtitle);

#ifdef __cplusplus
}
#endif

#endif
