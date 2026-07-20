#ifndef PAGE_USER_HISTORY_H
#define PAGE_USER_HISTORY_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 用户打印历史页：读取 /home/video/manifest.json，按时间倒序展示每次打印的
 * 时间/文件名/状态/时长/耗材 + 延时摄影缩略图；支持上下翻页与左滑删除。
 */
lv_obj_t *page_user_history_create(lv_obj_t *parent);

/** 进入/刷新：重新加载 manifest 并重建列表 */
void page_user_history_refresh(bool force);

/** 离页清理：复位左滑状态 */
void page_user_history_exit(void);

#ifdef __cplusplus
}
#endif

#endif
