#ifndef UI_SEGMENTED_H
#define UI_SEGMENTED_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_segmented.hpp
 * @brief 浅色档分段控件：圆角轨道 + 一枚跟着选中项走的实色胶囊
 *
 * 视觉结构（监控页 Tab 行，稿 6338:27240）：
 *   ┌──────────────────────────────────────┐  ← 轨道 radius，track_color/opa
 *   │ ╭────────╮                           │
 *   │ │  Job   │  Status      Manual       │  ← 选中项：pill_color/opa + pill_radius
 *   │ ╰────────╯                           │
 *   └──────────────────────────────────────┘
 *
 * 监控页两处共用一份：顶部三 Tab（Job/Status/Manual）与 Manual 里的 Plate Position
 * （Top/Level/Bottom，多一枚图标、胶囊改主色蓝、内缩为 0）。差别全在 cfg 里。
 *
 * **与 `ui_user_make_segmented()` 不是一回事，别合并**（`ui_user_widgets.hpp`）：
 * 那枚是深色档、写死几何、只有文字、没有逐项禁用，调用方全在调试页（14 处）。
 * 本控件要的四样它一样都没有：浅色配色、图标+文字竖排、逐项置灰、轨道/胶囊两套
 * 圆角。把它改成通吃要给那 14 处都补参数，得不偿失。
 */

/** 单段最多几项。当前用到 3（Tab 行 / Plate Position），留点余量。 */
#define UI_SEGMENTED_MAX_ITEMS 6

/** 「无选中」哨兵：`ui_segmented_get_active()` 在没有任何一项选中时返回它。 */
#define UI_SEGMENTED_NONE 0xFFu

/** 一项：图标可选，文字可选，但不能都为空。 */
typedef struct {
    const char *text;                       /**< 段文字；NULL = 纯图标 */
    /** 多档图标 set（`ic_xxx_set`，NULL 结尾）。NULL = 纯文字段。
     *  按 `cfg.icon_sz` 走 `ui_user_pick_icon()` 挑原生尺寸，上屏 1:1 不缩放。 */
    const lv_image_dsc_t *const *icon_set;
} ui_segmented_item_t;

typedef struct {
    const ui_segmented_item_t *items;
    uint8_t      count;                     /**< 1..UI_SEGMENTED_MAX_ITEMS */

    int32_t      track_w;                   /**< 轨道宽，<=0 = LV_PCT(100) */
    int32_t      track_h;                   /**< 轨道高，<=0 = LV_SIZE_CONTENT */
    int32_t      track_radius;
    int32_t      pill_radius;
    /** 胶囊相对轨道四周内缩。Tab 行是 2（稿 892→894），Plate Position 是 0。 */
    int32_t      inset;
    /** 相邻两段之间的缝。Tab 行 4、Plate Position 10（都由稿的胶囊宽反推）。 */
    int32_t      gap;

    lv_color_t   track_color;
    lv_opa_t     track_opa;
    lv_color_t   pill_color;
    lv_opa_t     pill_opa;

    lv_color_t   color_on;                  /**< 选中项的文字/图标色 */
    lv_color_t   color_off;                 /**< 未选中 */
    lv_color_t   color_disabled;            /**< 禁用（见 ui_segmented_set_item_enabled） */

    const lv_font_t *font;
    int32_t      icon_sz;                   /**< 图标上屏边长；<=0 且有 icon_set 时按 font 行高估 */
    int32_t      icon_gap;                  /**< 图标与文字之间的竖向间距 */

    uint8_t      selected;                  /**< 初始选中项；UI_SEGMENTED_NONE = 全不选 */
} ui_segmented_cfg_t;

/**
 * 建一个分段控件。返回轨道对象，**它就是事件源**：
 * 用户点某段后向它发 `LV_EVENT_VALUE_CHANGED`，用 `ui_segmented_get_active()` 取新值。
 *
 *   lv_obj_t *seg = ui_segmented_create(parent, &cfg);
 *   lv_obj_add_event_cb(seg, on_tab, LV_EVENT_VALUE_CHANGED, nullptr);
 *
 * 点已选中的那段不重复发事件；点禁用段、整体锁定时点任何段都什么都不发。
 */
lv_obj_t *ui_segmented_create(lv_obj_t *parent, const ui_segmented_cfg_t *cfg);

/** 置选中项，**不触发回调**（供代码侧同步状态）。越界视为 UI_SEGMENTED_NONE。 */
void ui_segmented_set_active(lv_obj_t *seg, uint8_t index);

/** 当前选中项；无选中返回 UI_SEGMENTED_NONE。 */
uint8_t ui_segmented_get_active(const lv_obj_t *seg);

/**
 * 逐项启用/禁用。禁用项走 `color_disabled` 并摘掉 `LV_OBJ_FLAG_CLICKABLE`，
 * 点它没有任何反应（连按压反馈都没有）。
 *
 * 最早给打印态的 Manual Tab 用；打印中 Manual 改为可进之后暂时没有调用方，留作通用能力。
 * **禁用当前选中项时会自动退到第一个可用项**并发一次 `LV_EVENT_VALUE_CHANGED`——
 * 不退的话页面会停在一个点不回来的视图上。所以**别拿它锁「选中项有副作用」的控件**
 * （Manual 的 Plate Position 选中即发 Z 运动命令），那种用 ui_segmented_set_locked()。
 */
void ui_segmented_set_item_enabled(lv_obj_t *seg, uint8_t index, bool enabled);

/** 某项当前是否可点。越界返回 false。 */
bool ui_segmented_item_enabled(const lv_obj_t *seg, uint8_t index);

/**
 * 整体锁定 / 解锁：锁定时摘掉全部项的 `LV_OBJ_FLAG_CLICKABLE`，解锁时只给 enabled 的项加回。
 * **不改选中项、不发事件、不动配色**——与 ui_segmented_set_item_enabled() 不同，后者禁掉选中项
 * 会退位并发 VALUE_CHANGED。锁住的样子由调用方给（例如整体 opa）。
 * 用在打印中的 Manual：Plate Position 选中即发 Z 运动，锁的时候不能有任何事件冒出来。
 */
void ui_segmented_set_locked(lv_obj_t *seg, bool locked);

#ifdef __cplusplus
}
#endif

#endif
