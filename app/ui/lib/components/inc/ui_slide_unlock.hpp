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
 *   - 静止态提示文字（深色玻璃档另带 3 个 chevron 箭头）随拖动淡出，到底淡入
 *     "RELEASE TO OPEN"；两行字的位置都不随拖动移动。提示文字垫在滑块下面，
 *     "RELEASE TO OPEN" 压在滑块上面。
 *   - 滑到底不自动触发，需在末端「松手」才触发 unlock 回调；中途松手则动画回弹。
 *   - 解锁后控件自动 disable（防止双触发），由调用方决定是否销毁/复位。
 *
 * 不依赖任何业务模块，可被多个弹窗复用。
 */
typedef void (*ui_slide_unlock_cb_t)(void *user_data);

typedef struct ui_slide_unlock_t ui_slide_unlock_t;

/**
 * 三档配色。**0 必须是深色玻璃档**：所有调用点都是 `= {}` 零初始化，零值得落在
 * 「不设就是原样」的那一档上。
 *
 * LIGHT 与 INK 共用同一套版式（圆角矩形轨道与滑块、提示字右偏到旋钮右边那段、没有
 * chevron），INK 是 LIGHT 整体反相。稿上两者尺寸相同（94 高、84 旋钮、一圈 1px 白描边；
 * 旋钮离外框 5、离描边内侧 4），几何差只在圆角：轨道 / 旋钮 LIGHT 是 28 / 24，INK 是
 * 24 / 20——两档都是「旋钮比同心值大 1」，所以拐角间隙的样子一致。
 * INK 的白边不用 LVGL border 画（深色 bg + 1px border 圆角会漏色起毛边），而是深色区
 * 内缩 1px 露出底下的白卡，所以它只适合放在白底上。
 * 提示字也不同：INK 那两张弹窗稿的字比首页小一号，用 user_font_title()、不加字距；
 * LIGHT 用 user_font_body() + 字距 2。
 */
typedef enum {
    /** 深色玻璃：黑 50% 全圆轨道 + 红渐变全圆滑块 + 三个 chevron（给暗背景画的）。 */
    UI_SLIDE_THEME_GLASS = 0,
    /** 浅色：#FCFCFC 实底轨道 + 深色 #333145 滑块 + 深色满色字（首页开门滑块、放版弹窗，
     *  稿 6060:69168；轨道底、投影与字色按 6578:41426 / 6388:48980 校过）。 */
    UI_SLIDE_THEME_LIGHT,
    /** 墨色：#5A5872→#333145 横向渐变轨道 + **白**滑块 + 白字（监控页取件弹窗 6338:27929、
     *  请先开外门弹窗 6339:28971）。 */
    UI_SLIDE_THEME_INK,
} ui_slide_theme_t;

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
     * 配色档，见 ui_slide_theme_t。
     *
     * 控件默认是给暗背景画的：黑 50% 的玻璃轨道、白提示字、红渐变滑块。浅色稿里这三样
     * 全反过来，墨色弹窗那条又在浅色的版式上把配色再反一次。差异集中在配色与圆角，
     * **结构一模一样**，所以做成档位而不是另起几套控件。
     *
     * 早先这里是一个 `bool light_theme`；加第三档时换成了枚举，免得再叠一个互斥的 bool。
     */
    ui_slide_theme_t      theme;
} ui_slide_unlock_cfg_t;

/** 在 parent 上创建一个滑动解锁条；返回句柄供后续销毁/查询用 */
ui_slide_unlock_t *ui_slide_unlock_create(lv_obj_t *parent,
                                          const ui_slide_unlock_cfg_t *cfg);

/** 取得控件根节点（用于布局对齐：lv_obj_align 等） */
lv_obj_t *ui_slide_unlock_get_root(ui_slide_unlock_t *self);

/** 复位：knob 弹回起点、重新可滑（用于可重复触发的场景，如首页开门）。
 *  **会顺带解除禁用**，调用方若仍需禁用要在之后再 set_enabled(false)。 */
void ui_slide_unlock_reset(ui_slide_unlock_t *self);

/** 启用/禁用滑动交互（禁用时不响应拖动）。
 *  禁用观感：浅色 / 墨色档轨道不动、滑块与静止提示字压到 30%（稿 6683:34165 打印中的
 *  首页开门滑块）；深色玻璃档整条轨道 70%。 */
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
