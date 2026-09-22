#ifndef PAGE_USER_PHOTO_H
#define PAGE_USER_PHOTO_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 相册全屏浏览器（历史页 Album 隐藏子页 user_photo）：
 * 左右滑动 / 两侧箭头翻页（首末循环回绕），单击主图切换控件层显隐。
 *
 * 大图走 ui::PhotoDecoder 后台解码，进图瞬间先贴 .rthm 缩略图顶住画面。
 */
lv_obj_t *page_user_photo_create(lv_obj_t *parent);
void page_user_photo_refresh(bool force);
void page_user_photo_exit(void);

/**
 * 设置待看照片（switch_to("user_photo") 之前调用）。
 * 照片列表由 ui::album::scan() 统一枚举，这里只需要落点。
 * @param photo_abs_path 绝对路径，如 "/userdata/media/photo/photo_xxx.png"
 */
void page_user_photo_set_source(const char *photo_abs_path);

#ifdef __cplusplus
}
#endif

#endif
