#ifndef UI_SLIDE_UNLOCK_H
#define UI_SLIDE_UNLOCK_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_slide_unlock.hpp
 * @brief 滑动解锁控件（用于 D1 "滑动解锁开门"）
 *
 * 行为（对齐 Figma「滑动控件示意」）：
 *   - 一条半透明玻璃胶囊轨道。
 *   - 左端一段红色玻璃胶囊，按住向右拖动时"生长"，前沿圆头带开门图标与高光。
 *   - 静止态显示提示文字 + 3 个 chevron 箭头；拖动中隐藏；到底变 "RELEASE TO OPEN"。
 *   - 滑到底不自动触发，需在末端「松手」才触发 unlock 回调；中途松手则动画回弹。
 *   - 解锁后控件自动 disable（防止双触发），由调用方决定是否销毁/复位。
 *
 * 不依赖任何业务模块，可被多个弹窗复用。
 */
typedef void (*ui_slide_unlock_cb_t)(void *user_data);

typedef struct ui_slide_unlock_t ui_slide_unlock_t;

typedef struct {
    const char           *hint_text;   /**< 静止态提示文字，NULL = "SLIDE TO UNLOCK" */
    ui_slide_unlock_cb_t  on_unlocked; /**< 解锁回调（在 LVGL 主线程） */
    void                 *user_data;
    int32_t               track_w;     /**< 轨道宽，<=0 用 USER_SLIDE_W */
    int32_t               track_h;     /**< 轨道高，<=0 用 USER_SLIDE_H */
    /** 圆头直径，<=0 时沿用「按 track_h 推导」的老规则（见 .cpp）。显式给值即同时
     *  定下内缩量 = (track_h - knob_sz)/2——稿上开门滑块与打印态取件胶囊都是内缩 8，
     *  而老规则在 1280 档只能推出 4。 */
    int32_t               knob_sz;
    bool                  custom_accent; /**< true 时用 accent_color 替代默认主色 */
    lv_color_t            accent_color;  /**< knob/解锁文字配色（custom_accent=true 生效） */
    /** knob 中心图标（A8 掩膜），NULL = 默认的 ic_door_open。 */
    const lv_image_dsc_t *knob_icon;
    /** true = 不画玻璃轨道底（打印态复合控件里那条底衬由调用方统一画，
     *  轨道自己再压一层半透明黑会让胶囊段比相邻的圆钮底衬明显更深）。 */
    bool                  transparent_track;
    /**
     * true = 浅色档（首页待机态的开门滑块，稿 6060:69168）。
     *
     * 控件其余部分默认是给暗背景画的：黑 50% 的玻璃轨道、白提示字、红渐变滑块。浅色稿
     * 里这三样全反过来——白 90% 的轨道、深色提示字、深色 #333145 的滑块，而且轨道与滑块
     * 都从全圆改成了圆角矩形。差异集中在配色与圆角，**结构一模一样**，所以做成一个开关
     * 而不是另起一套控件；其余三处调用（打印态取件、监控页、放版弹窗）不设它，观感不变。
     */
    bool                  light_theme;
} ui_slide_unlock_cfg_t;

/** 在 parent 上创建一个滑动解锁条；返回句柄供后续销毁/查询用 */
ui_slide_unlock_t *ui_slide_unlock_create(lv_obj_t *parent,
                                          const ui_slide_unlock_cfg_t *cfg);

/** 取得控件根节点（用于布局对齐：lv_obj_align 等） */
lv_obj_t *ui_slide_unlock_get_root(ui_slide_unlock_t *self);

/** 复位：knob 弹回起点、重新可滑（用于可重复触发的场景，如首页开门） */
void ui_slide_unlock_reset(ui_slide_unlock_t *self);

/** 启用/禁用滑动交互（禁用时不响应拖动） */
void ui_slide_unlock_set_enabled(ui_slide_unlock_t *self, bool enabled);

/**
 * 改静止态提示文字（打印态取件条要在「SWIPE TO COLLECT」和吹粉中文案之间切换）。
 * text 必须是长期有效的字面量——控件只存指针不拷贝。当前正处于静止态时立即生效。
 */
void ui_slide_unlock_set_hint(ui_slide_unlock_t *self, const char *text);

/** 销毁控件（root 会被 lv_obj_delete） */
void ui_slide_unlock_destroy(ui_slide_unlock_t *self);

#ifdef __cplusplus
}
#endif

#endif
