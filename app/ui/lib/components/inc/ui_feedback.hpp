#ifndef UI_FEEDBACK_H
#define UI_FEEDBACK_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 顶部 toast 的三类样式（稿 6448:54033 常规 / 54043 错误 / 54059 注意）。
 *
 * 版式完全一样，只差图标与颜色，颜色同时作用于图标圆底和文字。**按语义选类型，
 * 不要按想要的颜色反推**：归类规则与全量清单见 doc/Toast样式归类清单.md。
 *
 * 改版前这里传的是 lv_color_t，浅色档里只画成左侧一道色条；新稿去掉了色条，
 * 改成类型。参数类型一换，旧调用全部编不过，不会有漏改的。
 */
typedef enum {
    UI_TOAST_SUCCESS = 0,  /**< 常规反馈：#485DFF 圆 + 白对勾，文字同色。操作完成 / 已生效 */
    UI_TOAST_ERROR,        /**< 错误提示：#FF3131 圆 + 白叉，文字 #FF4242。操作失败 */
    UI_TOAST_INFORM,       /**< 注意：#FFAC26 圆 + 白色 i，文字同色。被拦下 / 请稍等 / 进行中 / 输入不合法 */
} ui_toast_type_t;

/**
 * 内容区左缘（toast 在它右侧到屏幕右缘这一段里水平居中）。由 PageManager 换页时设：
 * 有侧栏的页给侧栏宽，没有侧栏的全屏页（延时摄影、相册大图）给 0。页面自己不用管。
 */
void ui_toast_set_content_left(int32_t px);

/** 顶部 Toast 提示（自动消失）。纵向贴顶 USER_HOME_EDGE_GAP，横向在内容区居中。 */
void ui_toast_show(const char *msg, ui_toast_type_t type, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif
