#ifndef PAGE_COMMON_H
#define PAGE_COMMON_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 带标题的卡片面板（Progress / Connection 等区块复用） */
lv_obj_t *ui_make_card(lv_obj_t *parent, const char *title);

#ifdef __cplusplus
}
#endif

#endif
