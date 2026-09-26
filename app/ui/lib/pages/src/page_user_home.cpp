#include "page_user_home.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "control.hpp"
#include "home_assets.h"
#include "home_metric_icons.h"
#include "ui_slide_unlock.hpp"
#include "ui_device_state.hpp"
#include "ui_notice_stack.hpp"
#include "page_manager.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>

/**
 * @file page_user_home.cpp
 * @brief Snowpod 风格用户首页（待机）
 *
 * 打印态与待机监控已迁到 `page_user_monitor.*`，本页只保留待机主区。
 *
 * 视觉层次（对齐设计稿 5277:11571）：
 *   ┌────────────────────────────────────────────────┐
 *   │ (设备信息由外壳的常驻条画)      (通知浮层贴右上) │ ← topbar（空占位带）
 *   │ ┌ HOME PM2.5 ─────┐            ○─ Cabin condition
 *   │ │ Fair (36.5)     │  [Device]     TEMPERATURE  │
 *   │ │ ▂▂▂▂ ●         │               O2 CONC.     │
 *   │ └─────────────────┘               HUMIDITY     │
 *   │                                   PRESSURE     │
 *   │ [PRINT]                     [滑动开门 >>>]     │ ← 都是浮动
 *   └────────────────────────────────────────────────┘
 *
 * 环境快照与运行状态在 `core/ui_device_state`，本页作为观察者读取；顶栏左半边的
 * 设备名/状态由外壳的常驻条 `ui_device_bar` 画。本页那条带子只剩占位作用。
 */

namespace {

/** 数值无效（后端未采集）：负值或 NaN → UI 显示 "--"。 */
static inline bool metric_invalid(float v) { return std::isnan(v) || v < 0.0f; }

/** 共享快照的观察者：`ui_device_state` 收到环境推送/状态置位后重绘本页。 */
void on_device_state(const ui_device_snapshot_t * /*snap*/)
{
    page_user_home_refresh(false);
}


void home_env_init(void)
{
    /* 环境订阅与默认值都在 ui_device_state（由 ui_app.cpp 初始化一次），本页只
     * 挂观察者——register_env_update 是单槽的，这里再订阅一次会把常驻状态栏的
     * 数据源顶掉。 */
    ui_device_state_add_observer(on_device_state);
}

void home_trigger_door_open(void)
{
     /* 走 control 的门弹窗流程（Opening，含 PM2.5 净化检查/最短停留），
      * 开门成功即回首页。不再直接 send_open_door()。 */
     control_open_door_request();
}

/* ---------- 首页左卡（HOME 空气质量·可切换 PM2.5/PM10） ---------- */
lv_obj_t *g_home_card_title = nullptr;  /**< "HOME PM2.5" / "HOME PM10" */
lv_obj_t *g_home_card_level = nullptr;  /**< 分级大字 Good/Fair/Poor/Very Poor */
lv_obj_t *g_home_card_value = nullptr;  /**< 分级大字下那行读数 "36.5 ug/m3" */
lv_obj_t *g_home_seg_track  = nullptr;  /**< 4 段色条容器（用于计算游标 x） */
lv_obj_t *g_home_seg_dot    = nullptr;  /**< 分级游标圆点 */
int       g_home_metric_idx = 0;        /**< 0=PM2.5 1=PM10（切换按钮，重启复位） */

/* ---------- 首页右侧四宫格（温度 / 氧浓度 / 湿度 / 气压） ----------
 * 浅色稿 6040:66164 把早先那个「悬浮文本 + 引线」的 CABIN 面板换成了 2×2 白卡，
 * 单位并进读数字符串，所以不再有单独的 unit label。 */
lv_obj_t *g_cab_temp_val  = nullptr;
lv_obj_t *g_cab_o2_val    = nullptr;
lv_obj_t *g_cab_hum_val   = nullptr;
lv_obj_t *g_cab_prs_val   = nullptr;

/* ---------- 首页问候语与设备图上的两颗余量胶囊 ---------- */
lv_obj_t *g_greet_title    = nullptr;  /**< Morning / Afternoon / Evening */
lv_obj_t *g_supply_pill_a  = nullptr;  /**< 左：1 号料桶余量 */
lv_obj_t *g_supply_pill_b  = nullptr;  /**< 右：2 号料桶余量（暂借 filter_pct，见 make_supply_pill） */

/* ---------- 页面容器 ---------- */
ui_slide_unlock_t *g_door_slide = nullptr; /**< 首页"滑动开门"控件 */

void on_files_click(lv_event_t *)
{
    /* 打印中（含 Prepare / 结果页）：PRINT 当作「回到打印监控页」，别再走选文件开新单。 */
    if(control_is_printing()) {
        ui::PageManager::instance().switch_to("user_monitor");
        return;
    }
    /* 待机：设计稿右上角为 "我的文件" 入口。后端文件管理流程后续重建，
     * 暂复用现有打印编排作为占位，保证流程可达。 */
    control_start();
}

/* 滑动开门解锁后：触发开门 + 提示，并在短暂延时后把滑块弹回以便再次使用 */
void door_slide_reset_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if(g_door_slide) {
        ui_slide_unlock_reset(g_door_slide);
    }
}

void on_door_slide_unlocked(void * /*user_data*/)
{
    /* 开门流程改由门弹窗承载，不再弹「Open Door」吐司（避免与弹窗重复） */
    home_trigger_door_open();
    lv_timer_t *t = lv_timer_create(door_slide_reset_cb, 700, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

/** 按钮内图标：A8 掩膜 recolor 成 color，等比缩放到 target；外套固定尺寸盒，
 *  保证在 flex 布局里只占 target×target（避免大尺寸源图撑开按钮）。
 *  源图边长从图头取，各档尺寸通吃；调用方通常先用 ui_user_pick_icon() 挑出与 target
 *  等宽的那份，走不到缩放。
 *
 *  **颜色必须在这里给，不能拿返回值再补一笔**：返回的是外层那个固定尺寸盒，而
 *  `LV_STYLE_IMAGE_RECOLOR` 在 LVGL 的属性表里 inherit 标志是 0（见 lv_style.c），
 *  设在盒上传不到里面那张图。浅色稿的白底方钮曾因此白底白图标、整枚隐身。
 *
 *  默认白是给压在深色底上的图标用的，占多数。 */
lv_obj_t *make_btn_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t target,
                        lv_color_t color = lv_color_white())
{
    return ui_user_make_icon_img(parent, src, target, color);
}


/** 顶部占位带：左半边（设备名 + 状态点 + 状态文字）由外壳的常驻条 ui_device_bar
 *  画，右半边原有的"查看监控"圆钮与"Start Print"胶囊在稿 5277:11571 里已经取消，
 *  改成左下两枚入口胶囊（见 build_action_tiles）。
 *
 *  这条带子现在没有任何内容，但**不能删**：本页根节点是纵向 flex，其余几层
 *  （主区 / 开门滑块 / 入口胶囊）全是 FLOATING 不占格位，
 *  它是唯一撑起那一格的子节点，删掉整页纵向布局要重新验证。行高仍取
 *  USER_TOPBAR_H，与常驻条同高、同起点。 */
lv_obj_t *build_topbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_TOPBAR_H);
    lv_obj_set_style_pad_hor(bar, USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(bar, USER_TOPBAR_PAD_VER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 纯布局容器，去掉 lv_obj_create 默认带的 CLICKABLE，避免挡住下层触摸。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    return bar;
}


/* ============================================================================
 *  首页 normal 视图（对齐 Figma 5277:11571 待机中）
 *    左上：HOME 空气质量磨砂卡（PM2.5/PM10 可切换 + 四档色条 + 游标）
 *    右侧：Cabin condition 悬浮 4 行（TEMPERATURE/O2/HUMIDITY/PRESSURE）
 *    左下：两枚入口胶囊（Monitor / Start Print，稿 5339:27275）
 *    右下：滑动开门
 * ========================================================================== */

/** 空气质量指标描述：标题 + 三档上界（Good/Fair/Poor 上限）+ Very Poor 插值伪上限。 */
struct AirMetric {
    const char *title;
    float       b_good;     /**< ≤ b_good → Good */
    float       b_fair;     /**< ≤ b_fair → Fair */
    float       b_poor;     /**< ≤ b_poor → Poor；否则 Very Poor */
    float       pseudo_max; /**< Very Poor 段游标插值用的伪上限 */
};

/* 阈值取自设计稿分级表：PM2.5 35/75/150，PM10 50/150/350（μg/m³）。 */
const AirMetric kAirMetrics[2] = {
    { "PM2.5", 35.0f,  75.0f, 150.0f, 300.0f },
    { "PM10",  50.0f, 150.0f, 350.0f, 700.0f },
};

const char *const kLevelName[4] = { "Good", "Fair", "Poor", "Very Poor" };

/* 早先这里还有一枚 air_level_color()，给色条那四段上绿/黄/橙/红。浅色稿把四段改成了
 * 白色、不透明度递增（分级改由游标位置和分级大字表达），它就没有调用点了，已删。 */

/** 由数值定档并给出色条全局比例（0..1）：档内按数值在区间内线性插值。 */
int air_classify(const AirMetric &m, float v, float *frac_global)
{
    int   level;
    float lo, hi;
    if(v <= m.b_good)      { level = 0; lo = 0;        hi = m.b_good; }
    else if(v <= m.b_fair) { level = 1; lo = m.b_good; hi = m.b_fair; }
    else if(v <= m.b_poor) { level = 2; lo = m.b_fair; hi = m.b_poor; }
    else                   { level = 3; lo = m.b_poor; hi = m.pseudo_max; }
    float fin = (hi > lo) ? (v - lo) / (hi - lo) : 0.5f;
    if(fin < 0.0f) fin = 0.0f;
    if(fin > 1.0f) fin = 1.0f;
    if(frac_global) *frac_global = (static_cast<float>(level) + fin) / 4.0f;
    return level;
}

/** 多档 set 挑图，实现在 `ui_user_pick_icon()`（侧栏、控制页也在用同一份）。
 *  本页调用点多，留个短名转调。 */
const lv_image_dsc_t *pick_icon(const lv_image_dsc_t *const *set, int32_t sz)
{
    return ui_user_pick_icon(set, sz);
}

/** 着色的指标图标：取 set 里与 sz 等宽的那份，1:1 画进 sz 方盒、居中。
 *  尺寸对不上（改了主题却没同步生成脚本）才退回缩放，保证形不崩、只是糊。 */
lv_obj_t *make_tinted_icon(lv_obj_t *parent, const lv_image_dsc_t *const *set,
                           int32_t sz, lv_color_t color)
{
    const lv_image_dsc_t *src = pick_icon(set, sz);

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, sz, sz);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    const int32_t sw = (int32_t)src->header.w;
    if(sw != sz) {
        lv_image_set_pivot(img, sw / 2, sw / 2);
        lv_image_set_scale(img, (uint32_t)(256 * sz / sw));
    }
    lv_obj_center(img);
    return box;
}

void home_card_update(void);   /* 定义见下方数据刷新区 */

/** 左卡右上角切换按钮：PM2.5 ⇄ PM10 循环（重启复位到 PM2.5）。 */
void on_home_metric_toggle(lv_event_t * /*e*/)
{
    g_home_metric_idx = (g_home_metric_idx + 1) % 2;
    home_card_update();
}

/** 把浅色稿 HOME 卡内部的设计坐标（基准 240×300@1280）折到当前档位。
 *  卡内所有元素都是稿上的绝对定位，逐个开主题字段不值当，统一按卡宽比例折。
 *  **基准要跟着 user_home_card_w 的 1280 档走**，改那个值就要改这里的除数。 */
int32_t card_px(int32_t design_v)
{
    return design_v * USER_HOME_CARD_W / 240;
}

/** 卡内那枚 dim 图标：浅色稿里 HOME 卡与四宫格的卡头图标都是 24@1280，
 *  即 USER_METRIC_ICON_SZ 的 3/4（三档整除成 36/24/19，生成脚本里都有原生尺寸）。 */
int32_t light_icon_sz(void)
{
    return USER_METRIC_ICON_SZ * 3 / 4;
}

const lv_image_dsc_t *home_card_bg_dsc(void);   /* 定义见下方背景挑图区 */

/* 左卡：HOME 空气质量卡（浅色稿 6064:70631）。
 *
 * 底是预生成位图（径向渐变 + 点阵 + 底缘柔光，见 tools/gen_home_bg.py），卡内元素全是
 * 稿上的绝对定位，所以这里一律 FLOATING + align，不走 flex——稿本身就没有流式结构，
 * 硬套 flex 反而要为每一格编一套间距。
 *
 * 分级阈值、四档配色、游标插值这些逻辑一个没动，只是换了外观：色条从「绿/黄/橙/红四段」
 * 变成稿上的「白色四段、不透明度 20/40/60/80% 递增」，分级靠游标位置而不是颜色表达。 */
lv_obj_t *build_home_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(card, USER_HOME_CARD_W, USER_HOME_CARD_H);
    lv_obj_set_style_radius(card, USER_LIGHT_RADIUS, 0);
    /* 背景位图是方的，圆角靠这个裁——不把圆角烤进图里，换圆角就不用重跑脚本。 */
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 左边距 32 是稿上内容区的统一左内距（正好等于 USER_PAD_LG），但父节点 area 铺满
     * 整屏、没让开侧栏，所以左对齐还得自己补一个侧栏宽——同 build_monitor_area 的 EXIT 钮。 */
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, USER_PAD_LG + USER_SIDEBAR_W,
                 USER_HOME_CARD_TOP);

    lv_obj_t *bg = lv_image_create(card);
    lv_image_set_src(bg, home_card_bg_dsc());
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(bg, LV_IMAGE_ALIGN_COVER);
    lv_obj_center(bg);

    /* 头部：房子图标 (24,24) + 标题，右上角一枚 48 的切换钮 (180,12)。
     * 标题左缘 52 = 图标左 24 + 图标 24 + 间距 4；顶 24 是让它与 24 见方的图标居中对齐
     * 后的视觉位置（稿上整组的 opacity 是 90%）。 */
    lv_obj_t *hicon = make_tinted_icon(card, ic_home_set, light_icon_sz(),
                                       lv_color_white());
    lv_obj_align(hicon, LV_ALIGN_TOP_LEFT, card_px(24), card_px(24));

    g_home_card_title = lv_label_create(card);
    lv_label_set_text(g_home_card_title, kAirMetrics[0].title);
    lv_obj_set_style_text_color(g_home_card_title, lv_color_white(), 0);
    lv_obj_set_style_text_opa(g_home_card_title, LV_OPA_90, 0);
    lv_obj_set_style_text_font(g_home_card_title, user_font_title(), 0);
    lv_obj_align(g_home_card_title, LV_ALIGN_TOP_LEFT, card_px(52), card_px(24));

    lv_obj_t *toggle = lv_button_create(card);
    lv_obj_remove_style_all(toggle);
    lv_obj_set_size(toggle, card_px(48), card_px(48));
    /* 稿上这枚是白 30% 的圆角方块（不是全圆），圆角 16 取卡圆角的 2/3 */
    lv_obj_set_style_radius(toggle, USER_LIGHT_RADIUS * 2 / 3, 0);
    lv_obj_set_style_bg_color(toggle, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_align(toggle, LV_ALIGN_TOP_LEFT, card_px(180), card_px(12));
    lv_obj_add_event_cb(toggle, on_home_metric_toggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *tg_icon = make_tinted_icon(toggle, ic_switch_set, light_icon_sz(),
                                         lv_color_white());
    lv_obj_center(tg_icon);

    /* 分级大字与读数。稿上是一个 gap 4 的竖向块，顶在 136；"Good" 行高 52 而字号 40，
     * 上下各有 6 的行内留白，所以标签自身的顶落在 142。左缘 24 与上面的图标对齐。 */
    g_home_card_level = lv_label_create(card);
    lv_label_set_text(g_home_card_level, "--");
    lv_obj_set_style_text_color(g_home_card_level, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_home_card_level, user_font_display(), 0);
    lv_obj_align(g_home_card_level, LV_ALIGN_TOP_LEFT, card_px(24), card_px(142));

    g_home_card_value = lv_label_create(card);
    lv_label_set_text(g_home_card_value, "");
    lv_obj_set_style_text_color(g_home_card_value, lv_color_white(), 0);
    lv_obj_set_style_text_opa(g_home_card_value, LV_OPA_70, 0);
    lv_obj_set_style_text_font(g_home_card_value, user_font_body(), 0);
    lv_obj_align(g_home_card_value, LV_ALIGN_TOP_LEFT, card_px(24), card_px(198));

    /* 四段色条 (24,262) 宽 192 高 8 间距 4，游标 20 居中骑在上面（外框按游标高取 20，
     * 顶因此比色条早 6，即 256——正好是稿上游标自己的顶）。
     * 稿上四段是**白色、不透明度 20/40/60/80% 递增**，不是早先那套绿/黄/橙/红——
     * 分级改由游标位置和上面那行大字表达，色条只当刻度。 */
    lv_obj_t *seg_wrap = lv_obj_create(card);
    lv_obj_remove_style_all(seg_wrap);
    lv_obj_set_size(seg_wrap, card_px(192), card_px(20));
    lv_obj_set_style_bg_opa(seg_wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(seg_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(seg_wrap, LV_ALIGN_TOP_LEFT, card_px(24), card_px(256));

    g_home_seg_track = lv_obj_create(seg_wrap);
    lv_obj_remove_style_all(g_home_seg_track);
    lv_obj_set_width(g_home_seg_track, LV_PCT(100));
    lv_obj_set_height(g_home_seg_track, card_px(8));
    lv_obj_align(g_home_seg_track, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_flex_flow(g_home_seg_track, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_home_seg_track, card_px(4), 0);
    lv_obj_set_style_bg_opa(g_home_seg_track, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_home_seg_track, LV_OBJ_FLAG_SCROLLABLE);
    static const lv_opa_t kSegOpa[4] = { LV_OPA_20, LV_OPA_40, LV_OPA_60, LV_OPA_80 };
    for(int i = 0; i < 4; ++i) {
        lv_obj_t *seg = lv_obj_create(g_home_seg_track);
        lv_obj_remove_style_all(seg);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_height(seg, LV_PCT(100));
        lv_obj_set_style_radius(seg, card_px(2), 0);
        lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(seg, kSegOpa[i], 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    }

    g_home_seg_dot = lv_obj_create(seg_wrap);
    lv_obj_remove_style_all(g_home_seg_dot);
    lv_obj_set_size(g_home_seg_dot, card_px(20), card_px(20));
    lv_obj_set_style_radius(g_home_seg_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_home_seg_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_home_seg_dot, LV_OPA_COVER, 0);
    lv_obj_align(g_home_seg_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(g_home_seg_dot, LV_OBJ_FLAG_SCROLLABLE);

    /* 色条下原先还有 Good / Fair / Poor / Bad 四个刻度名，新稿去掉了：分级只由上面
     * 那行大字和游标位置表达，色条纯当刻度。 */

    return card;
}

/** 同一行里混两个字号时，让小字与大字**基线**对齐要补的量。
 *
 *  LVGL 的 flex 只能按盒子边对齐，没有基线对齐。label 的盒高就是 font->line_height，
 *  基线距盒底正好是 font->base_line（下伸部）。两个 label 盒底对齐时，小字的基线会比
 *  大字低 (base_big - base_small)，把这个差值补成小字的 pad_bottom 即可抬回去。
 *  字号随档位变，所以这里从字体现取、不写死。 */
int32_t baseline_pad(const lv_font_t *big, const lv_font_t *small)
{
    const int32_t d = (int32_t)big->base_line - (int32_t)small->base_line;
    return d > 0 ? d : 0;
}

/* 四宫格里的一格（稿 6201:85940 一族）：白卡，左上 caption、左下大读数，没有图标。
 * 卡尺寸由外层网格算好后传进来，caption 与读数之间用 SPACE_BETWEEN 撑开。
 *
 * caption 与读数都可能是「大字 + 小一号后缀」的组合（稿上 `Temp.` 20 配 `(°C)` 13、
 * `592.0` 32 配 `kpa` 20），LVGL 一个 label 只能一种字体，所以各自拆成两个 label
 * 横排、用 baseline_pad() 把基线找齐。
 *
 * @param cap_sm  caption 的小字后缀，可为 NULL（只有 Temp. 这一格有）
 * @param unit    读数后面的单位，可为 NULL
 * @param out_val 回传**数值**那个 label；单位是它的下一个兄弟，见 update_grid_metric()
 */
lv_obj_t *make_metric_cell(lv_obj_t *parent, const char *label, const char *cap_sm,
                           const char *unit, int32_t w, int32_t h,
                           lv_obj_t **out_val)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_radius(cell, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(cell, user_light_card, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    /* 稿上是 drop-shadow(0 2 3 rgba(0,0,0,0.04))，很淡但不能省：白卡压在 #f5f5f5 上，
     * 没有这层影子边界会看不出来。 */
    lv_obj_set_style_shadow_width(cell, 3, 0);
    lv_obj_set_style_shadow_ofs_y(cell, 2, 0);
    lv_obj_set_style_shadow_color(cell, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(cell, 10, 0);
    lv_obj_set_style_pad_all(cell, USER_PAD, 0);   /* 稿 24 */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    /* caption 行：两段文字紧挨着排。
     * **主轴不能用 SPACE_BETWEEN**——那是改版前为了把右上角那枚图标顶到卡片另一端
     * 设的，图标删掉之后会把 `Temp.` 和 `(°C)` 撑到卡片两端去。
     * **交叉轴必须是 END**：baseline_pad() 补的是「盒底对齐时两个字号的基线差」，
     * 只有底对齐才成立；CENTER 下它反而会把小字往上抬半个补偿量。 */
    lv_obj_t *head = lv_obj_create(cell);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(head);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_color(cap, user_light_text, 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_60, 0);
    lv_obj_set_style_text_font(cap, user_font_title(), 0);

    if(cap_sm) {
        lv_obj_t *cs = lv_label_create(head);
        lv_label_set_text(cs, cap_sm);
        lv_obj_set_style_text_color(cs, user_light_text, 0);
        lv_obj_set_style_text_opa(cs, LV_OPA_60, 0);
        lv_obj_set_style_text_font(cs, user_font_caption(), 0);
        lv_obj_set_style_pad_bottom(cs, baseline_pad(user_font_title(),
                                                     user_font_caption()), 0);
    }

    /* 读数行：数值 + 单位，底对齐后靠 pad_bottom 把基线找齐。 */
    lv_obj_t *vrow = lv_obj_create(cell);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_bg_opa(vrow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(vrow);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_light_value, 0);
    lv_obj_set_style_text_font(val, user_font_metric(), 0);

    if(unit) {
        lv_obj_t *u = lv_label_create(vrow);
        lv_label_set_text(u, unit);
        lv_obj_set_style_text_color(u, user_light_value, 0);
        lv_obj_set_style_text_font(u, user_font_title(), 0);
        lv_obj_set_style_pad_bottom(u, baseline_pad(user_font_metric(),
                                                    user_font_title()), 0);
    }

    if(out_val) *out_val = val;
    return cell;
}

/* 右侧四宫格（稿 6040:66164）：2×2 白卡，与左边 HOME 卡等高并排、中间隔 16。
 *
 * 这块取代了早先那个「悬浮文本 + 引线 + 空心圆环」的 CABIN 面板：浅色稿里它变成了
 * 规规矩矩的卡片网格，引线那套几何（USER_HOME_CABIN_LEAD_W 等）随之作废。
 * 四项语义没变，仍是舱内的温度/氧浓度/湿度/气压，数据源也还是同一份快照。
 *
 * 单卡尺寸由总宽高减去一道间距再折半推出来（1280 档正好是稿的 166×139），不单开字段。
 * 稿上单位是并进读数字符串的（"23.6°" / "59.0kpa"），不再单起一个 label。 */
lv_obj_t *build_metric_grid(lv_obj_t *parent)
{
    const int32_t gap = USER_PAD_SM / 2;                    /* 稿 8 */
    const int32_t cw  = (USER_HOME_GRID_W - gap) / 2;
    const int32_t ch  = (USER_HOME_CARD_H - gap) / 2;

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(grid, USER_HOME_GRID_W, USER_HOME_CARD_H);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, gap, 0);
    lv_obj_set_style_pad_row(grid, gap, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    /* 紧贴 HOME 卡右侧：左缘 = 卡左缘 + 卡宽 + 间距，顶与卡齐。 */
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT,
                 USER_PAD_LG + USER_SIDEBAR_W + USER_HOME_CARD_W + USER_PAD_SM,
                 USER_HOME_CARD_TOP);

    make_metric_cell(grid, "Temp.", " (\xC2\xB0""C)", "\xC2\xB0", cw, ch, &g_cab_temp_val);
    make_metric_cell(grid, "O2",       nullptr, "%",   cw, ch, &g_cab_o2_val);
    make_metric_cell(grid, "Humidity", nullptr, "%",   cw, ch, &g_cab_hum_val);
    make_metric_cell(grid, "Pressure", nullptr, "kpa", cw, ch, &g_cab_prs_val);
    return grid;
}

/** 取当前 profile 的首页背景图。1280 档用「背景图1」——新首页稿 5339:27207 那层背景就是
 *  它的构图，设备右缘上屏落在 910、与 CABIN 文字左缘 946 之间正好是稿上的 36px 间隙。
 *  1024 与 1920 档用「背景图2」：它自带一段左移，配合那两档尚未折算到稿值的 CABIN 宽度
 *  才不会让设备压进文字（换成背景图1 的话 1024 档要压 3px、1920 档压 11px）。
 *
 *  背景图2 那段左移**在生成时又被右移 23px 抵掉一部分**：当初只校对了右侧那条间隙，
 *  没量左侧——机身左缘落在 394，而 HOME 卡右缘是 399，实机上压进卡里 5px。右移后
 *  1024 档两侧变成 18 / 19px。推导与 1920 档的连带影响见 tools/gen_home_bg.py 的 PROFILES。
 *
 *  两个节点怎么选的、哪档换图要连带改什么，见 tools/gen_home_bg.py 的注释。 */
const lv_image_dsc_t *home_device_dsc()
{
    return (g_ui->profile_id == UI_RES_1024) ? &home_device_1024 : &home_device_1280;
}

const lv_image_dsc_t *home_card_bg_dsc()
{
    return (g_ui->profile_id == UI_RES_1024) ? &home_card_bg_1024 : &home_card_bg_1280;
}

/* 早先这里还有一个 build_status_hints()，在 HOME 卡与四宫格下方画 Home Status /
 * Cabin Status 两行说明小字。新稿把它们去掉了（卡片本身已经贴到内容区底部），
 * 连带主题里那枚只给它俩用的 user_light_hint 也一并删了。 */

/** 问候语块（稿 6201:86001）：大字 + 一行副标题。
 *  稿上左缘 160 与 HOME 卡对齐，两行间距 7。 */
void build_greeting(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(box, USER_PAD_SM / 2, 0);   /* 稿 7，取 8 */
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, USER_PAD_LG + USER_SIDEBAR_W,
                 USER_HOME_GREET_TOP);

    g_greet_title = lv_label_create(box);
    lv_label_set_text(g_greet_title, "Good morning");
    lv_obj_set_style_text_color(g_greet_title, user_light_text, 0);
    lv_obj_set_style_text_font(g_greet_title, user_font_metric(), 0);

    lv_obj_t *sub = lv_label_create(box);
    lv_label_set_text(sub, "Ready for your next print?");
    lv_obj_set_style_text_color(sub, user_light_text, 0);
    lv_obj_set_style_text_font(sub, user_font_body(), 0);
}

/** 设备图上那两颗余量胶囊（稿 6201:86004 / 86016）：白底全圆、20 料桶图标 + 20 文字。
 *
 *  图标是**主色的料桶 + 桶号**（`ic_barrel_1_set` / `ic_barrel_2_set`），与设备透图里
 *  那两个料桶一一对应——两颗胶囊就钉在各自的桶上，所以编号不能对调。
 *
 *  **第二颗的取值还是借来的**：图标已经说明两颗都是料桶，但协议的 `SupplyStatusPayload`
 *  只有 `material_pct` / `filter_pct` 两项、没有第二桶的余量，所以 update_supply_pills()
 *  暂时拿 `filter_pct` 顶着。后端补上第二桶字段后改那一行即可，几何与样式不用动。 */
lv_obj_t *make_supply_pill(lv_obj_t *parent, const lv_image_dsc_t *const *icon,
                           int32_t x, int32_t y, lv_obj_t **out_label)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, user_light_card, 0);
    lv_obj_set_style_bg_opa(pill, 230, 0);               /* 稿 white @90%，压在设备图上 */
    lv_obj_set_style_shadow_width(pill, 5, 0);
    lv_obj_set_style_shadow_ofs_y(pill, 4, 0);
    lv_obj_set_style_shadow_color(pill, lv_color_hex(0x4B68DE), 0);
    lv_obj_set_style_shadow_opa(pill, 26, 0);            /* 稿 10% */
    /* 稿量到：左内距 12、右内距 16（右边比左边宽一截，文字那侧要多留白）、
     * 上下 11（胶囊高 44 = 11 + 文字行高 22 + 11）。上下与左取同一个值，
     * 落地高 46、与稿差 2，在圆整范围内。 */
    lv_obj_set_style_pad_left(pill, USER_PAD_SM * 3 / 4, 0);
    lv_obj_set_style_pad_right(pill, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(pill, USER_PAD_SM * 3 / 4, 0);
    lv_obj_set_style_pad_column(pill, USER_PAD_SM / 4, 0);     /* 稿 4 */
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(pill, LV_ALIGN_TOP_LEFT, x, y);

    /* 稿 20；三档折算正好是 USER_METRIC_ICON_SZ * 5/8 = 30/20/16，与图标的尺寸表对齐 */
    make_tinted_icon(pill, icon, light_icon_sz() * 5 / 6, user_light_accent);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, "--");
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);   /* 稿 20 */
    if(out_label) *out_label = lbl;
    return pill;
}

/** 中部主区：浅色底 + 定位摆放的设备图 + 问候语 + HOME 卡 + 四宫格 + 余量胶囊。
 *
 *  area 是**铺满整屏**的浮层（含状态栏区域与侧栏之下那条），用负偏移 + LV_HOR_RES 跨回
 *  本页根节点的 pad_left，与打印态/监控态的摄像头层同一套做法（见 build_printing_area）。
 *  稿上的坐标都是整屏坐标系（设备图 x=772、HOME 卡 x=160 都含侧栏那 128），所以这里必须
 *  铺整屏才能直接照抄；若改回只铺内容区，所有 x 都要减去一个侧栏宽。
 *
 *  设备图右下两侧都是**出血**的（772 + 525 = 1297 > 1280、89 + 791 = 880 > 800），
 *  超出部分由 area 裁掉，稿本身就这样。 */
lv_obj_t *build_main_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(area, LV_HOR_RES, LV_PCT(100));
    lv_obj_set_pos(area, -USER_SIDEBAR_W, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_style_bg_color(area, user_light_bg, 0);
    lv_obj_set_style_bg_opa(area, LV_OPA_COVER, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    /* 设备渲染图：资源按档位出原生尺寸（见 tools/gen_home_bg.py），1280/1024 两档
     * COVER 都落到 1.000、零重采样；1920 档复用 1280 那份、放大 1.5×，会软。 */
    lv_obj_t *dev = lv_image_create(area);
    lv_image_set_src(dev, home_device_dsc());
    lv_obj_set_size(dev, USER_HOME_DEV_IMG_W, USER_HOME_DEV_IMG_H);
    lv_image_set_inner_align(dev, LV_IMAGE_ALIGN_COVER);
    lv_obj_align(dev, LV_ALIGN_TOP_LEFT, USER_HOME_DEV_IMG_X, USER_HOME_DEV_IMG_Y);

    build_greeting(area);
    build_home_card(area);
    build_metric_grid(area);

    /* 两颗余量胶囊钉在设备图上。稿上给的是整屏坐标 (862,316) / (1077,368)，这里换算成
     * 相对设备图左上角（772,89）的比例，换档位时跟着图一起缩放、不会飘离料盒。
     * 分母是 1280 档的框宽/框高（525 × 791），改 USER_HOME_DEV_IMG_W/H 要同步改。
     * 图标的桶号要与钉的位置对上：A 在左桶、B 在右桶。 */
    make_supply_pill(area, ic_barrel_1_set,
                     USER_HOME_DEV_IMG_X + USER_HOME_DEV_IMG_W *  90 / 525,
                     USER_HOME_DEV_IMG_Y + USER_HOME_DEV_IMG_H * 227 / 791, &g_supply_pill_a);
    make_supply_pill(area, ic_barrel_2_set,
                     USER_HOME_DEV_IMG_X + USER_HOME_DEV_IMG_W * 305 / 525,
                     USER_HOME_DEV_IMG_Y + USER_HOME_DEV_IMG_H * 279 / 791, &g_supply_pill_b);

    return area;
}

/** 右下角浮动 "Slide to Unlock" 滑动解锁条（真正可拖动；滑到底触发开门） */
lv_obj_t *build_door_floating(lv_obj_t *parent)
{
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text  = "SLIDE TO UNLOCK";   /* 箭头由控件绘制 3 个 chevron */
    cfg.on_unlocked = on_door_slide_unlocked;
    cfg.user_data  = nullptr;
    /* 浅色稿 6201:85972 的开门滑块：1280 档 453×94、旋钮 84 上下各内缩 5，圆角 28/24。
     * 不走 USER_DOOR_SLIDE_*（旧打印态深色滑块尺寸）。本条单独一套 USER_HOME_SLIDE_*，
     * 旋钮与左下方钮、PRINT 同源，都是 USER_ACTION_BTN_SZ。 */
    cfg.track_w    = USER_HOME_SLIDE_W;
    cfg.track_h    = USER_HOME_SLIDE_H;
    cfg.knob_sz    = USER_ACTION_BTN_SZ;
    cfg.theme      = UI_SLIDE_THEME_LIGHT;   /* 待机态是浅色底，见 ui_slide_unlock.hpp */
    /* 旋钮里那枚门（稿 6230:89954 的 x 815..836）。上一版浅色稿（6201:85974）画的是
     * 右箭头，新稿换回了门——与监控页 Manual 底部那颗 OPEN 是**逐点相同的同一枚**
     * 图标（只差整体平移），所以共用 ic_door_panel_set，`ic_arrow_right_set` 随之
     * 没有调用点。按控件浅色档的图标边长（knob_sz * 8/21）挑原生尺寸，上屏 1:1。 */
    cfg.knob_icon  = ui_user_pick_icon(ic_door_panel_set, USER_ACTION_BTN_SZ * 8 / 21);
    g_door_slide = ui_slide_unlock_create(parent, &cfg);

    lv_obj_t *root = ui_slide_unlock_get_root(g_door_slide);
    if(root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_FLOATING);
        /* 右距就是 USER_HOME_EDGE_GAP；底距要再减掉旋钮的内缩——稿上与左下按钮行上下
         * 对齐的是**旋钮**（两者顶都在 668、底都在 752），轨道比旋钮上下各高出 5。 */
        const int32_t inset = (USER_HOME_SLIDE_H - USER_ACTION_BTN_SZ) / 2;
        lv_obj_align(root, LV_ALIGN_BOTTOM_RIGHT, -USER_HOME_EDGE_GAP,
                     -(USER_HOME_EDGE_GAP - inset));
    }
    return root;
}

/* ---------------------------------------------------------------------------
 *  左下的 PRINT 入口（稿 6230:89954 的 (160,668) 216×84）：
 *    ┌──────────────────────┐
 *    │  [文件夹]   PRINT      │
 *    └──────────────────────┘
 *  这个入口一路搬过家：先从顶栏的 Start Print 胶囊挪到左下变成方块，再变成一行等高
 *  的全圆胶囊，浅色稿又换成圆角矩形并把文案缩成 PRINT。
 *
 *  **新稿把同排左边那枚摄像机方钮删掉了**：监控已经独立成页、挂在侧栏第 2 格，首页
 *  不再需要入口。整行于是只剩 PRINT 一枚，左缘从 x=260 挪回 x=160（与 HOME 卡同一条
 *  竖线），宽度从 202 涨到 216。`make_light_square_btn()` 随之没有调用点、一并删掉。
 *
 *  稿上白色都带 backdrop blur(15px)，LVGL 没有背景模糊，直接给不透明白——底下是
 *  #f5f5f5，肉眼差别只在边缘那一点点。同一处理见铃铛。
 * ------------------------------------------------------------------------- */

/** 首页底部那批按钮里的图标边长（稿 40@1280）。取 USER_METRIC_ICON_SZ 的 5/4：
 *  三档算下来正好是 60 / 40 / 32，与稿的 ×1.5 / ×1 / ×0.8 逐档吻合。 */
int32_t action_icon_sz(void)
{
    return USER_METRIC_ICON_SZ * 5 / 4;
}

/** PRINT **不复用** ui_user_make_primary_pill_btn：那个组件是全圆胶囊、主色蓝底
 *  （历史页空态那枚还在用），而稿这里要的是圆角 24 的深色 #333145 底，两处已经不是
 *  同一枚按钮了。整行的来龙去脉见上面那段。 */
lv_obj_t *build_action_tiles(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* 稿上这一行的左缘距侧栏 32（USER_PAD_LG，与上方卡片同一条竖线），底缘距屏底 48
     * （USER_HOME_EDGE_GAP）——**这两个值不一样，稿本身就不对称**。横向只退一个
     * USER_PAD_LG：本页根节点的 pad_left 已经让开侧栏，不用再补侧栏宽度
     * （同 build_door_floating）。 */
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, USER_PAD_LG, -USER_HOME_EDGE_GAP);

    lv_obj_t *print = lv_button_create(row);
    lv_obj_remove_style_all(print);
    lv_obj_set_size(print, USER_HOME_PRINT_BTN_W, USER_ACTION_BTN_SZ);
    lv_obj_set_style_radius(print, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(print, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(print, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(print, user_light_accent, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(print, 10, 0);
    lv_obj_set_style_shadow_ofs_y(print, 5, 0);
    lv_obj_set_style_shadow_color(print, lv_color_hex(0xA1A1A1), 0);
    lv_obj_set_style_shadow_opa(print, LV_OPA_20, 0);
    lv_obj_set_style_pad_column(print, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(print, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(print, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(print, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(print, on_files_click, LV_EVENT_CLICKED, nullptr);
    make_btn_icon(print, ui_user_pick_icon(ic_folder_set, action_icon_sz()),
                  action_icon_sz());
    lv_obj_t *plbl = lv_label_create(print);
    lv_label_set_text(plbl, "PRINT");
    lv_obj_set_style_text_color(plbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(plbl, user_font_body(), 0);   /* 稿 24 */
    lv_obj_set_style_text_letter_space(plbl, 2, 0);   /* 稿 tracking 1.92 */

    return row;
}

/** 刷新左卡（HOME 空气质量）：按当前切换指标填 标题/分级/数值/游标位置。 */
void home_card_update(void)
{
    if(!g_home_card_title) {
        return;
    }
    const AirMetric &m = kAirMetrics[g_home_metric_idx];
    lv_label_set_text(g_home_card_title, m.title);

    const ui_device_snapshot_t &snap = *ui_device_state_get();
    const float v = (g_home_metric_idx == 0) ? snap.pm25_indoor
                                             : snap.pm10_indoor;
    if(metric_invalid(v)) {
        /* 无数据：分级显示 "--"，清空数值、隐藏游标 */
        lv_label_set_text(g_home_card_level, "--");
        lv_obj_set_style_text_color(g_home_card_level, user_color_text_dim, 0);
        lv_label_set_text(g_home_card_value, "");
        if(g_home_seg_dot) {
            lv_obj_add_flag(g_home_seg_dot, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    float frac = 0.0f;
    const int level = air_classify(m, v, &frac);
    lv_label_set_text(g_home_card_level, kLevelName[level]);
    lv_obj_set_style_text_color(g_home_card_level, user_color_text, 0);
    /* 稿是 `35 μg/m³`，没有括号。**单位只能写成 ug/m3**：LVGL 的 Montserrat 字符集是
     * `0x20-0x7F,0xB0,0x2022`（ASCII + 度号 + 圆点，`23.6°` 能显示就是靠那个 0xB0），
     * `μ`(U+03BC) 与 `³`(U+00B3) 都不在里面。要上稿的写法得扩字符集重新生成整套字体。 */
    lv_label_set_text_fmt(g_home_card_value, "%.1f ug/m3", v);

    /* 游标：档内插值得到全局比例 frac，落在色条对应位置。 */
    if(g_home_seg_dot && g_home_seg_track) {
        lv_obj_remove_flag(g_home_seg_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(g_home_seg_track);
        const int32_t tw = lv_obj_get_width(g_home_seg_track);
        const int32_t dot_sz = lv_obj_get_width(g_home_seg_dot);
        int32_t dx = static_cast<int32_t>(frac * static_cast<float>(tw)) - dot_sz / 2;
        if(dx < 0) dx = 0;
        if(dx > tw - dot_sz) dx = tw - dot_sz;
        lv_obj_align(g_home_seg_dot, LV_ALIGN_LEFT_MID, dx, 0);
    }
}

/** 四宫格那一格的读数。稿上数值与单位是同一行的两个字号（`592.0` 32 配 `kpa` 20），
 *  所以它们是两个 label：`val` 是数值，单位是它的下一个兄弟（见 make_metric_cell）。
 *  无效时整行收成 "--"，单位一并藏掉。 */
void update_grid_metric(lv_obj_t *val, float v)
{
    if(!val) {
        return;
    }
    lv_obj_t *unit = lv_obj_get_sibling(val, 1);
    if(metric_invalid(v)) {
        lv_label_set_text(val, "--");
        if(unit) lv_obj_add_flag(unit, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(val, "%.1f", v);
        if(unit) lv_obj_remove_flag(unit, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 问候语按本机时段切（稿上只画了 "Good morning" 这一态，另两档照它的句式补）。
 *  副标题是固定文案，不随状态变。 */
void update_greeting(void)
{
    if(!g_greet_title) {
        return;
    }
    const time_t now = time(nullptr);
    struct tm tmv {};
    localtime_r(&now, &tmv);
    const char *word = (tmv.tm_hour < 12) ? "Good morning"
                     : (tmv.tm_hour < 18) ? "Good afternoon" : "Good evening";
    lv_label_set_text(g_greet_title, word);
}

/** 余量百分比 → 稿上那三档词。阈值是照常识取的，稿只给了 Medium / Low 两个样例，
 *  确认后若有正式分档表，改这里一处即可。 */
const char *supply_level_name(float pct)
{
    if(pct >= 60.0f) return "High";
    if(pct >= 25.0f) return "Medium";
    return "Low";
}

/** 刷新设备图上那两颗余量胶囊。 */
void update_supply_pills(void)
{
    const control_supply_t *s = control_supply_get();
    if(g_supply_pill_a && s) {
        lv_label_set_text(g_supply_pill_a, supply_level_name(s->material_pct));
    }
    if(g_supply_pill_b && s) {
        /* **等后端补第二桶字段**：稿上这颗是 2 号料桶，但 SupplyStatusPayload 只有
         * material_pct / filter_pct，没有第二桶的量，先拿滤芯顶着。补上之后换掉这一行。 */
        lv_label_set_text(g_supply_pill_b, supply_level_name(s->filter_pct));
    }
}

void on_supply_changed(const control_supply_t * /*s*/)
{
    update_supply_pills();
}

} /* namespace */


void page_user_home_refresh(bool /*force*/)
{
    /* 设备名/WiFi/状态点那一组不在本页——外壳的常驻条自己观察同一份快照。 */
    const ui_device_snapshot_t &snap = *ui_device_state_get();

    /* 换页时 PageManager 会把通知栈的让位量与侧栏档位都复位，回到本页要按待机再置。 */
    ui_notice_stack_set_top_offset(0);
    ui::PageManager::instance().set_sidebar_video_mode(false);
    ui_notice_stack_set_dark(false);

    home_card_update();
    update_greeting();
    update_supply_pills();

    update_grid_metric(g_cab_temp_val, snap.temperature_c);
    update_grid_metric(g_cab_o2_val,   snap.oxygen_percent);
    update_grid_metric(g_cab_hum_val,  snap.humidity_percent);
    update_grid_metric(g_cab_prs_val,  snap.pressure_kpa);
}

lv_obj_t *page_user_home_create(lv_obj_t *parent)
{
    home_env_init();

    /* 首页根节点跨过 content_panel_ 的左/顶 padding 铺满整屏，再用自己的 pad_left
     * 把内容区还原成侧栏右侧那块。
     * 纵向不还原 pad_top：本页自己那条按钮行就占在那一格，正好落在外壳常驻设备
     * 信息条的下面。
     * 尺寸不能用 LV_PCT(100)：那算的是父内容区，会被 pad_top 扣掉一条。 */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(page, -USER_SIDEBAR_W, -USER_CONTENT_TOP);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_left(page, USER_SIDEBAR_W, 0);
    lv_obj_set_style_pad_row(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    build_main_area(page);
    build_topbar(page);

    build_door_floating(page);
    build_action_tiles(page);

    control_supply_add_observer(on_supply_changed);
    control_supply_request();

    page_user_home_refresh(true);
    return page;
}
