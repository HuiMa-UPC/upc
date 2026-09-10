#ifndef PAGE_USER_TIMELAPSE_H
#define PAGE_USER_TIMELAPSE_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 延时摄影播放器（历史页隐藏子页 user_timelapse）：优先 MP4，失败回退 PNG 序列。
 * 拖动预览：MP4/PNG 均采用「后台小图 scrub + 松手 settle 主画面」。
 */
lv_obj_t *page_user_timelapse_create(lv_obj_t *parent);
void page_user_timelapse_refresh(bool force);
void page_user_timelapse_exit(void);

/**
 * 设置待播媒体源（switch_to("user_timelapse") 之前调用）。
 * @param sequence_dir_abs 绝对路径，如 "/userdata/media/video/benchy"
 * @param mp4_abs_path    MP4 绝对路径（可空）；有值时优先播放 MP4，失败回退 PNG 序列
 * @param title          顶部标题（如作业名）
 * @param subtitle       顶部副标题（如时间）
 */
void page_user_timelapse_set_source(const char *sequence_dir_abs,
                                    const char *mp4_abs_path,
                                    const char *title,
                                    const char *subtitle);

#ifdef __cplusplus
}
#endif

#endif
