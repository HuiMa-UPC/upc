#ifndef PAGE_COMMON_H
#define PAGE_COMMON_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 页面占位模板（各 page_xxx.c 可调用或自行扩展）
 * @param parent 右侧内容区容器
 * @param title  页面标题
 * @param hint   开发提示文字
 */
lv_obj_t *page_common_create_placeholder(lv_obj_t *parent, const char *title, const char *hint);

#ifdef __cplusplus
}
#endif

#endif
