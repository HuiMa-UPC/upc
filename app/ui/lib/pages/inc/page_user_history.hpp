#ifndef PAGE_USER_HISTORY_H
#define PAGE_USER_HISTORY_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 用户打印历史页：三列缩略图网格，按自然月分组、时间倒序。
 *
 * 三个 Tab —— Timelapse / Video 读 /userdata/media/video/config.json，
 * Photo 扫 /userdata/media/photo。长按卡片进多选，多选态下可批量删除。
 */
lv_obj_t *page_user_history_create(lv_obj_t *parent);

/** 进入/刷新：重新加载 manifest 并重建网格 */
void page_user_history_refresh(bool force);

/** 离页清理：关掉确认弹窗、退出多选态 */
void page_user_history_exit(void);

#ifdef __cplusplus
}
#endif

#endif
