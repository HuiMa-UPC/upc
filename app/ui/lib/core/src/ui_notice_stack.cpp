#include "ui_notice_stack.hpp"

#include "ui_message_store.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "page_manager.hpp"
#include "page_user_message.hpp"
#include "home_metric_icons.h"

#include <cstdio>

namespace {

/** toast 最多摆两张卡（稿上的数），面板最多列两条，多出来的收进「查看剩余 N 条」。
 *  面板改版前按产品定的是三条、不跟稿；2026-10-10 那版稿（6578:41549）把每条加高到 72，
 *  三条会把面板拉到 y≈384、盖住设备图上的余量胶囊，改为照稿的两条。 */
constexpr int kMaxToast = 2;
constexpr int kMaxRows  = 2;

/** 右上角这块此刻是三态里的哪一个。深色档（打印态/监控态）不走这套，见 g_dark。 */
enum Phase { PH_TOAST, PH_BELL, PH_PANEL };

/** 容器四周给投影留的余量。三态的投影（0 2 6，见各 build_*）都画在控件外沿：左右各约 3、
 *  上约 1、下约 5。LVGL 按父对象范围裁子对象，容器零内距时铃铛只剩左边那道投影（容器往左
 *  还空着 328），toast / 面板与容器同宽，两侧和底边也被裁掉。所以容器四周垫一圈内距、宽度
 *  补回、贴角偏移同步扣掉（见 reposition），屏上位置不变。
 *  不用 OVERFLOW_VISIBLE：重绘区仍会被裁到容器范围内，容器外的旧投影擦不掉、留残影。 */
int32_t shadow_room(void)
{
    return USER_PAD_SM / 2;
}

lv_obj_t *g_root       = nullptr;
/** 面板展开时垫在它下面的全屏透明捕获层，吃掉落在面板外的那一下点击。 */
lv_obj_t *g_catcher    = nullptr;
int32_t   g_top_offset = 0;
/** 当前页允不允许显示（由 PageManager 按页设）。与"有没有消息"是两个条件，
 *  必须分开记：只用 HIDDEN 标志的话，重建时无从知道是哪一边把它藏起来的。 */
bool      g_page_allows = true;
bool      g_dark        = false;   /**< 打印态/监控态：走改版前那摞黑玻璃卡 */
bool      g_panel_open  = false;
bool      g_badge_on    = false;

void rebuild(void);

void reposition(void)
{
    if(!g_root) {
        return;
    }
    /* 整块建在铺满屏的 shell_ 上，贴右上角，退一个 USER_HOME_EDGE_GAP（稿 48）。
     *
     * **这个值同时管着三处的对齐**，改一处就得改另外两处：
     *   - 稿上铃铛右缘 1232 = 右下开门滑块的右缘，两者都退 48（滑块那边同样走
     *     USER_HOME_EDGE_GAP），差一点点屏上就看得出来；
     *   - 首页打印态/监控态右上角那块环境区（build_env_wrap）与本栈上下相接看着是
     *     一栏，所以那边也退同一个量。
     * 早先这三处退的是 USER_PAD_LG（32），铃铛比滑块往外多出 16、顶也高 16。
     *
     * 纵向的让位量才是按态给的：稿 4972:27560 里这栈在 y=104，而
     * 48(环境区顶) + 64(环境区高) + 8(间距) 正是 120——那张是旧稿的 32 起算，
     * 换 48 之后整体跟着环境区一起下移，相对关系不变。
     *
     * 上面说的都是内容的位置；容器四周还有一圈投影余量（shadow_room），对齐时扣掉。 */
    lv_obj_align(g_root, LV_ALIGN_TOP_RIGHT, -USER_HOME_EDGE_GAP + shadow_room(),
                 USER_HOME_EDGE_GAP + g_top_offset - shadow_room());
}

/* ---------------------------------------------------------------------------
 *  事件：全都推迟到事件派发之外做
 *
 *  这几个动作最后都会走到 rebuild()，而 rebuild() 第一件事就是 lv_obj_clean(g_root)，
 *  把正在派发事件的那颗按钮本身一起删掉。回到 LVGL 的派发循环时对象已经没了。
 * ------------------------------------------------------------------------- */

void dismiss_async(void *code_raw)
{
    ui_message_dismiss((uint16_t)(uintptr_t)code_raw);
}

void on_close_click(lv_event_t *e)
{
    lv_async_call(dismiss_async, lv_event_get_user_data(e));
}

void goto_message_page_async(void *)
{
    g_panel_open = false;
    /* 从这里进消息页一律落在 All Tab 顶部：面板里列的是跨 Tab 的「还需留意的事」，
     * 停在上次的 Device / System 上可能一条都看不到。 */
    page_user_message_request_all_tab();
    ui::PageManager::instance().switch_to("user_message");
}

void on_more_click(lv_event_t *)
{
    lv_async_call(goto_message_page_async, nullptr);
}

void open_panel_async(void *)
{
    g_panel_open = true;
    rebuild();
}

void on_bell_click(lv_event_t *)
{
    /* 没红点就等于没什么可看的（判据与面板取数同源），直接跳消息中心，
     * 不弹一个空面板。 */
    if(!ui_message_needs_attention()) {
        lv_async_call(goto_message_page_async, nullptr);
        return;
    }
    lv_async_call(open_panel_async, nullptr);
}

void close_panel_async(void *)
{
    g_panel_open = false;
    rebuild();
}

void on_catcher_click(lv_event_t *)
{
    lv_async_call(close_panel_async, nullptr);
}

/* ---------------------------------------------------------------------------
 *  深色档（打印态 / 监控态）：改版前那摞黑玻璃卡，原样保留
 * ------------------------------------------------------------------------- */

/** 一张通知卡：色块图标 + 标题/副标题两行，信息卡右上多一颗 ×。 */
void build_dark_card(lv_obj_t *parent, const ui_message_t *m)
{
    /* 只有信息卡带 ×（告警级不可关，见 ui_message_dismiss）。配色不由这个布尔决定：
     * 四档各自的色走 ui_message_severity_color()。 */
    const bool closable = (m->severity == UI_MSG_SEV_INFO);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    /* 定宽而不是铺满：容器按（更宽的）铃铛面板给宽，见 ui_notice_stack_create */
    lv_obj_set_width(card, USER_NOTICE_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(card, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_radius(card, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 32 见方的实色圆角块，里面一枚 18 的白图标。底色与图标走 ui_message_store 那份
     * 共用的等级映射，同一条消息在消息页列表里同色。 */
    const int32_t box_sz = USER_PRINT_BADGE_SZ * 2;
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, box_sz, box_sz);
    lv_obj_set_style_bg_color(box, ui_message_severity_color(m->severity), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t ic_sz = box_sz * 9 / 16;
    lv_obj_t *icon = ui_user_make_icon_img(box,
        ui_message_severity_icon(m->severity, ic_sz), ic_sz, lv_color_white());
    lv_obj_center(icon);

    lv_obj_t *texts = lv_obj_create(card);
    lv_obj_remove_style_all(texts);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(texts, USER_PAD_SM / 2, 0);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);
    /* 信息卡右上那颗 × 是浮层子节点，不占 flex 格位，文字得自己躲开它 */
    if(closable) {
        lv_obj_set_style_pad_right(texts, USER_PAD_SM * 3 / 2, 0);
    }

    /* 稿 4972:27560 里标题与副标题**同为 16**，只靠字重（SemiBold / Regular）和 60%
     * 透明度区分。Montserrat 只有一个字重，字重那半做不出来，所以只落地「同号 +
     * 透明度」——两处都取 user_font_text_sm()。 */
    lv_obj_t *title = lv_label_create(texts);
    lv_label_set_text(title, m->title);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, user_font_text_sm(), 0);

    if(m->body && m->body[0]) {
        lv_obj_t *sub = lv_label_create(texts);
        lv_label_set_text(sub, m->body);
        lv_obj_set_style_text_color(sub, lv_color_white(), 0);
        lv_obj_set_style_text_opa(sub, LV_OPA_60, 0);
        lv_obj_set_style_text_font(sub, user_font_text_sm(), 0);
    }

    if(closable) {
        lv_obj_t *close = lv_label_create(card);
        lv_label_set_text(close, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(close, lv_color_white(), 0);
        lv_obj_set_style_text_opa(close, LV_OPA_60, 0);
        lv_obj_set_style_text_font(close, user_font_caption(), 0);
        lv_obj_add_flag(close, LV_OBJ_FLAG_FLOATING);
        /* 偏移取 0：align 是相对内容区算的，卡片的 pad_all 已经让开了一圈 */
        lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
        /* 图标本身才 10 来像素，撑开点击热区免得点不中 */
        lv_obj_set_ext_click_area(close, USER_PAD_SM);
        lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(close, on_close_click, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)m->code);
    }
}

/** 深色档卡片下面那条「查看剩余 N 条」，点进消息页。 */
void build_dark_more_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, USER_NOTICE_CARD_W);   /* 与上面的卡同宽，见 build_dark_card */
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(btn, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, USER_RADIUS / 2, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_more_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, user_font_text_sm(), 0);

    lv_obj_t *chevron = lv_label_create(btn);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, lv_color_white(), 0);
    lv_obj_set_style_text_opa(chevron, LV_OPA_60, 0);
    lv_obj_set_style_text_font(chevron, user_font_caption(), 0);
}

void build_dark(void)
{
    const int total = ui_message_visible_count();
    const int shown = total < kMaxToast ? total : kMaxToast;
    for(int i = 0; i < shown; i++) {
        build_dark_card(g_root, ui_message_visible_at(i));
    }
    if(total > shown) {
        const int rest = total - shown;
        char text[64];
        std::snprintf(text, sizeof(text), "View remaining %d message%s",
                      rest, rest == 1 ? "" : "s");
        build_dark_more_row(g_root, text);
    }
}

/* ---------------------------------------------------------------------------
 *  浅色档：toast / 铃铛 / 面板
 * ------------------------------------------------------------------------- */

/** 一张 toast 白卡（稿 6064:71781）：等级色徽标 + 标题/正文两行，INFO 右侧多一颗关闭钮。 */
void build_toast_card(lv_obj_t *parent, const ui_message_t *m)
{
    const bool closable = (m->severity == UI_MSG_SEV_INFO);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    /* toast 卡仍是 400（稿 5339:27299/27310），比 460 的铃铛面板窄。容器按面板给宽，
     * 这里定宽、靠容器的交叉轴 END 贴右，右缘与面板、铃铛同在 1232。 */
    lv_obj_set_width(card, USER_NOTICE_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);          /* 稿 16 */
    lv_obj_set_style_pad_column(card, USER_PAD_SM, 0);       /* 稿 16 */
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, USER_BTN_RADIUS, 0);       /* 稿 8 */
    lv_obj_set_style_shadow_width(card, 6, 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_shadow_color(card, user_light_shadow, 0);
    lv_obj_set_style_shadow_opa(card, 26, 0);                /* 稿 10% */
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 32 见方的等级色圆角块 + 一枚 18 的白图标，与消息页列表共用同一份等级映射。 */
    const int32_t box_sz = USER_PRINT_BADGE_SZ * 2;
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, box_sz, box_sz);
    lv_obj_set_style_bg_color(box, ui_message_severity_color(m->severity), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, USER_BTN_RADIUS, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    const int32_t ic_sz = box_sz * 9 / 16;
    lv_obj_t *icon = ui_user_make_icon_img(box,
        ui_message_severity_icon(m->severity, ic_sz), ic_sz, lv_color_white());
    lv_obj_center(icon);

    lv_obj_t *texts = lv_obj_create(card);
    lv_obj_remove_style_all(texts);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(texts, USER_PAD_SM / 2, 0);     /* 稿 8 */
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);

    /* 稿上标题与正文**同为 16**，只靠字重（SemiBold / Regular）和 60% 透明度分层。
     * Montserrat 只有一个字重，字重那半做不出来，落地的是「同号 + 透明度」。 */
    lv_obj_t *title = lv_label_create(texts);
    lv_label_set_text(title, m->title);
    lv_obj_set_style_text_color(title, user_light_text, 0);
    lv_obj_set_style_text_font(title, user_font_text_sm(), 0);

    if(m->body && m->body[0]) {
        lv_obj_t *sub = lv_label_create(texts);
        lv_label_set_text(sub, m->body);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, LV_PCT(100));
        lv_obj_set_style_text_color(sub, user_light_text, 0);
        lv_obj_set_style_text_opa(sub, LV_OPA_60, 0);
        lv_obj_set_style_text_font(sub, user_font_text_sm(), 0);
    }

    if(closable) {
        /* 稿上是一枚 24 见方、圆角 8、底 #333145 @ 5% 的方块，里面一个叉。
         * 24 取等级色块（32）的 3/4，圆角与卡片同走 USER_BTN_RADIUS。 */
        lv_obj_t *close = lv_obj_create(card);
        lv_obj_remove_style_all(close);
        lv_obj_set_size(close, box_sz * 3 / 4, box_sz * 3 / 4);
        lv_obj_set_style_radius(close, USER_BTN_RADIUS, 0);
        lv_obj_set_style_bg_color(close, user_light_text, 0);
        lv_obj_set_style_bg_opa(close, 13, 0);              /* 稿 5% */
        lv_obj_set_style_bg_opa(close, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(close, USER_PAD_SM);
        lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(close, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(close, on_close_click, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)m->code);

        lv_obj_t *x = lv_label_create(close);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(x, user_light_text, 0);
        lv_obj_set_style_text_opa(x, LV_OPA_40, 0);
        lv_obj_set_style_text_font(x, user_font_caption_sm(), 0);
        lv_obj_remove_flag(x, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(x);
    }
}

void build_toast(void)
{
    const int total = ui_message_visible_count();
    const int shown = total < kMaxToast ? total : kMaxToast;
    for(int i = 0; i < shown; i++) {
        build_toast_card(g_root, ui_message_visible_at(i));
    }
}

/** 铃铛（稿 6073:73172）：72 见方的白色圆角方钮，右上角一颗红点。
 *
 *  稿上是 rgba(255,255,255,0.1) + backdrop blur(15px)。LVGL 没有背景模糊，而白 10%
 *  叠在 #f5f5f5 上肉眼等于没有——稿 PNG 里那个白其实是模糊把背景糊白之后的结果，
 *  所以这里直接给不透明白，靠投影划出边界。 */
void build_bell(void)
{
    lv_obj_t *btn = lv_button_create(g_root);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_LIGHT_BTN_SZ, USER_LIGHT_BTN_SZ);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_light_glass, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, user_light_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
    /* 投影与 toast / 面板同一套（稿 6230:89954：0 2 6 #0C1C61 @10%） */
    lv_obj_set_style_shadow_width(btn, 6, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 2, 0);
    lv_obj_set_style_shadow_color(btn, user_light_shadow, 0);
    lv_obj_set_style_shadow_opa(btn, 26, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_bell_click, LV_EVENT_CLICKED, nullptr);

    /* 稿上是一枚 40 见方的实心铃铛。按钮 72 配图标 40，故三档按 *5/9 折算得
     * 60 / 40 / 32，与 gen_home_metric_icons.py 的 ICON_SIZES["bell"] 一致、
     * 上屏 1:1 不缩放。早先这里借的是 Montserrat 自带的铃铛字形，形状与稿不同。
     *
     * **字形按有没有红点分两版**：带红点那版（稿 6201:85994）把铃铛右上角咬掉了一块
     * 给红点让出一圈空隙，无红点那版（稿 6212:87571）是完整轮廓。两版外接框一样、
     * 只有轮廓差，拿错了不会错位、但常态下铃铛右上会多一个没来由的豁口。 */
    const int32_t ic_sz = USER_LIGHT_BTN_SZ * 5 / 9;
    const lv_image_dsc_t *const *bell = g_badge_on ? ic_bell_dot_set : ic_bell_set;
    lv_obj_t *ic = ui_user_make_icon_img(btn, ui_user_pick_icon(bell, ic_sz),
                                         ic_sz, user_light_text);
    lv_obj_center(ic);

    if(g_badge_on) {
        /* 红点是稿上铃铛的一部分：circle(50.5, 34.5) r=4.5 落在 72 的按钮里，即直径
         * 按钮的 1/8、圆心在按钮坐标 (44.5, 30.5)——**挂在字形右上方、在按钮内部**，
         * 不是骑在按钮圆角外面。左上角坐标因此是 (44.5-4.5, 30.5-4.5) = (40, 26)。 */
        const int32_t sz = USER_LIGHT_BTN_SZ / 8;             /* 稿 9 */
        lv_obj_t *dot = lv_obj_create(btn);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, sz, sz);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, user_light_notice_dot, 0);   /* 稿值 #FE6868 */
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, USER_LIGHT_BTN_SZ * 40 / 72,
                     USER_LIGHT_BTN_SZ * 26 / 72);
    }
}

/** 面板里的一条（稿 6578:41551，2026-10-10 版）：左侧等级色竖条 + 标题/正文两行。
 *  整行可点，点了去消息页 All Tab 顶部，与「查看剩余」同一个落点。 */
void build_panel_row(lv_obj_t *parent, const ui_message_t *m)
{
    /* 稿上标题 SemiBold 20、正文 Regular 18 @60%（改版前两行都是 16）。 */
    const lv_font_t *title_font = user_font_title();
    const lv_font_t *body_font  = user_font_text();
    const int32_t title_h = lv_font_get_line_height(title_font);
    const int32_t body_h  = lv_font_get_line_height(body_font);
    const bool has_body = m->body && m->body[0];
    const int32_t pad_ver = USER_PAD_SM * 3 / 4;                  /* 稿 12 */
    const int32_t pad_hor = USER_PAD_SM / 2;                      /* 稿 8 */

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    /* 稿上条目是按内容撑的（12 + 48 + 12 = 72；48 是色条，也正好是两行字的高）。改版前那版
     * 是 8 + 19 + 2 + 19 + 8 = 56。行高取内容与 USER_PANEL_ROW_H 的大者：低档位字号没跟着
     * 等比缩，两行字撑出来可能比按比例折的行高还高，取小了会把第二行挤出去。
     *
     * **不能写成 LV_SIZE_CONTENT + min_height**：高度按内容时 LVGL 的 flex 把整行内容贴顶
     * 排（lv_flex.c 里强制 track_cross_place = START），多出来的那截全堆在底下——竖条比稿
     * 高一格，没有正文的单行条目更是顶在上面、下面空一大块。两行字都锁了一行高（见下），
     * 内容高度算得出来，所以直接给定值。 */
    const int32_t text_h = has_body ? title_h + 2 + body_h : title_h;
    lv_obj_set_height(row, LV_MAX(USER_PANEL_ROW_H, text_h + pad_ver * 2));   /* 稿 72 */
    lv_obj_set_style_radius(row, USER_LIGHT_RADIUS / 2, 0);   /* 稿 12 */
    lv_obj_set_style_bg_color(row, user_light_row_bg, 0);
    lv_obj_set_style_bg_opa(row, 13, 0);                      /* 稿 5% */
    lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_pad_ver(row, pad_ver, 0);
    lv_obj_set_style_pad_hor(row, pad_hor, 0);
    lv_obj_set_style_pad_column(row, USER_PAD_SM * 3 / 4, 0); /* 稿 12 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_more_click, LV_EVENT_CLICKED, nullptr);

    /* 竖条按行高折（72 的 1/18 与 2/3），三档得 6×72 / 4×48 / 3×38。稿上 rx=2 即
     * 两端全圆，所以圆角给 CIRCLE、跟着条宽走。
     * 子对象一律摘掉 CLICKABLE（lv_obj_create 默认带）：点在竖条或文字上也要落到整行。 */
    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, USER_PANEL_ROW_H / 18, USER_PANEL_ROW_H * 2 / 3);  /* 稿 4×48 */
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, ui_message_severity_color(m->severity), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *texts = lv_obj_create(row);
    lv_obj_remove_style_all(texts);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(texts, 2, 0);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(texts, LV_OBJ_FLAG_CLICKABLE);

    /* 稿上标题与正文靠字号（20 / 18）、字重（SemiBold / Regular）和 60% 透明度分层。
     * Montserrat 只有一个字重，字重那半做不出来，落地的是「字号 + 透明度」。
     *
     * 超长一律单行截断出省略号，条目不随文案长短变高。v9 的 DOTS 是「先按宽度换行、
     * 再截末行」，不锁高度就只会换行、出不来省略号，所以两行都锁成一行高。 */
    lv_obj_t *title = lv_label_create(texts);
    lv_label_set_text(title, m->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(title, LV_PCT(100), title_h);
    lv_obj_set_style_text_color(title, user_light_text, 0);
    lv_obj_set_style_text_font(title, title_font, 0);

    if(has_body) {
        lv_obj_t *sub = lv_label_create(texts);
        lv_label_set_text(sub, m->body);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_size(sub, LV_PCT(100), body_h);
        lv_obj_set_style_text_color(sub, user_light_text, 0);
        lv_obj_set_style_text_opa(sub, LV_OPA_60, 0);
        lv_obj_set_style_text_font(sub, body_font, 0);
    }
}

/** 面板底部那行「查看剩余 N 条」（稿 6578:41561），点进消息中心。 */
void build_panel_more(lv_obj_t *parent, int rest)
{
    /* 行尾箭头的边长（稿 14），按行高折得三档 21 / 14 / 11（行高 108 / 72 / 58 的 7/36），
     * 与生成脚本 ICON_SIZES["panel_chevron"] 一一对应、上屏 1:1 不缩放。改版前行高是
     * 84 / 56 / 45、取的是 1/4，两版算出来的三档相同，图标不用重出。 */
    const int32_t ic_sz = USER_PANEL_ROW_H * 7 / 36;

    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    /* 2026-10-10 版稿上这一行是定高 72、与上面的条目同高（改版前按内容撑：16 + 一行字 19
     * + 16 = 51），取 USER_PANEL_ROW_H。 */
    lv_obj_set_height(btn, USER_PANEL_ROW_H);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(btn, user_light_more_bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, user_light_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn, USER_PAD_SM, 0);             /* 稿 16 */
    /* 稿上右边那个 16 是量到箭头矢量本身的，而位图盒子比矢量宽：矢量右缘离盒右缘还有
     * 盒宽的 2/7（1280 档 4，裁窗见 gen_home_metric_icons.py 的 panel_chevron）。
     * 这一截从右内距里扣掉，箭头才落在稿上的位置。 */
    lv_obj_set_style_pad_right(btn, USER_PAD_SM - ic_sz * 2 / 7, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_more_click, LV_EVENT_CLICKED, nullptr);

    char text[64];
    std::snprintf(text, sizeof(text), "View remaining %d message%s",
                  rest, rest == 1 ? "" : "s");
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);     /* 稿 20（改版前 16） */

    ui_user_make_icon_img(btn, ui_user_pick_icon(ic_panel_chevron_set, ic_sz),
                          ic_sz, user_light_text);
}

void build_panel(void)
{
    lv_obj_t *card = lv_obj_create(g_root);
    lv_obj_remove_style_all(card);
    /* 面板宽 460@1280（USER_NOTICE_PANEL_W）就是容器的宽，直接铺满。改版前它与 toast
     * 同为 400，新稿单独加宽了。 */
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, USER_PAD_SM * 3 / 4, 0);    /* 稿 12 */
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);        /* 稿 8 */
    /* 投影与 toast 卡同一套（稿上两者引的是同一个 effect：0 2 6 #0C1C61 @10%） */
    lv_obj_set_style_shadow_width(card, 6, 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_shadow_color(card, user_light_shadow, 0);
    lv_obj_set_style_shadow_opa(card, 26, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const int total = ui_message_attention_count();
    const int shown = total < kMaxRows ? total : kMaxRows;
    for(int i = 0; i < shown; i++) {
        build_panel_row(card, ui_message_attention_at(i));
    }
    if(total > shown) {
        build_panel_more(card, total - shown);
    }
}

/* ---------------------------------------------------------------------------
 *  重建
 * ------------------------------------------------------------------------- */

/** 面板展开时垫的全屏捕获层。
 *
 *  **必须建在 shell_ 上（g_root 的父节点），不能挂 lv_layer_top()**：弹窗都在 top 层，
 *  捕获层放上去会把「面板 vs 弹窗」的层级关系变成按创建时序碰运气——面板展开时突然
 *  冒出来的 WiFi 配对弹窗要么点不到、要么被当成「面板外」一点就把面板收了。留在
 *  shell_ 里则弹窗恒在它之上，关系是确定的。
 *
 *  两个顺序约束：FLOATING 要在 move_foreground 之前加（shell_ 是横向 flex，直接对
 *  flex 子节点改 z 序会连布局顺序一起改，侧栏当年就是这么被挪到屏幕右边的）；捕获层
 *  先提到最前、g_root 再提一次，这样面板压在捕获层之上，而两者都盖过侧栏——不盖住
 *  侧栏的话点侧栏收不起面板，会直接切页、把面板留在那儿飘着。 */
void ensure_catcher(lv_obj_t *parent)
{
    if(g_catcher) {
        return;
    }
    g_catcher = lv_obj_create(parent);
    lv_obj_remove_style_all(g_catcher);
    lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(g_catcher, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_catcher, 0, 0);
    /* 全透明：稿上面板展开时背景没有压暗，这层只为吃点击而存在。 */
    lv_obj_set_style_bg_opa(g_catcher, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_catcher, on_catcher_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_HIDDEN);
}

/** 消息集合一变就整个重建。条数封顶四五个子节点，重建比增量维护省心得多。 */
void rebuild(void)
{
    if(!g_root) {
        return;
    }
    lv_obj_clean(g_root);

    if(!g_page_allows) {
        g_panel_open = false;
        lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        if(g_catcher) lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const int visible = ui_message_visible_count();

    if(g_dark) {
        /* 打印态/监控态：没有铃铛也没有面板，没消息就整块收起来。 */
        g_panel_open = false;
        if(g_catcher) lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_HIDDEN);
        if(visible <= 0) {
            lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        build_dark();
        reposition();
        return;
    }

    /* 三态择一：有 toast 就摆 toast（此时铃铛让位、面板也收起），否则看面板开没开。 */
    Phase ph = PH_BELL;
    if(visible > 0) {
        ph = PH_TOAST;
        g_panel_open = false;   /* 新消息到达时把展开的面板顶掉 */
    } else if(g_panel_open) {
        ph = PH_PANEL;
    }

    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    switch(ph) {
        case PH_TOAST: build_toast(); break;
        case PH_PANEL: build_panel(); break;
        case PH_BELL:  build_bell();  break;
    }

    if(g_catcher) {
        if(ph == PH_PANEL) {
            lv_obj_remove_flag(g_catcher, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_catcher);
            lv_obj_move_foreground(g_root);
        } else {
            lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_HIDDEN);
            /* 展开面板时这两层越到了侧栏之上，收起后把侧栏提回最前还原次序。
             * 眼下 toast/铃铛都在右上、与侧栏不重叠，看不出差别，但留着错位的 z 序
             * 迟早会在别处咬人。 */
            ui::PageManager::instance().raise_sidebar();
        }
    }
    reposition();   /* 高度随内容变，重新贴一次 */
}

} /* namespace */

lv_obj_t *ui_notice_stack_create(lv_obj_t *parent)
{
    if(g_root) {
        return g_root;
    }

    ensure_catcher(parent);

    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    /* FLOATING：shell_ 是横向 flex，本块不能占格位（同 menu_panel_ / 设备信息条） */
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_FLOATING);
    /* 宽度按三态里最宽的铃铛面板给（稿 6578:41549 的 2026-10-10 版是 460@1280），面板
     * 直接铺满；toast 卡（400）与铃铛（72）比它窄，各自定宽、在容器里右对齐，右缘都在
     * 1232。改版前面板与 toast 同为 400、容器按 toast 卡给宽。
     *
     * toast 卡宽的来历：**稿自己在两个 frame 里给了不同的值**——首页待机
     * 5339:27299/27310 是 400，而打印态 4972:27560 的 5281:11797 是 416（与 stats-card
     * 同宽）。本块是建在 shell_ 上的全局对象、两个态共用一个实例，只能取一个值——以待机
     * frame 为准取 400（USER_NOTICE_CARD_W）。两侧另加投影余量，内容宽不变。 */
    lv_obj_set_width(g_root, USER_NOTICE_PANEL_W + 2 * shadow_room());
    lv_obj_set_height(g_root, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(g_root, shadow_room(), 0);
    lv_obj_set_flex_flow(g_root, LV_FLEX_FLOW_COLUMN);
    /* 交叉轴靠右：铃铛（72）与 toast 卡（400）都比容器（460）窄，要贴着右缘与面板对齐。 */
    lv_obj_set_flex_align(g_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(g_root, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    /* 容器本身不可点，免得空白处吃掉底下页面的触摸；卡片里的关闭钮、铃铛、面板里的
     * 按钮各自可点。**捕获层要的恰恰相反**（它就是来吃点击的），别把这行抄过去。 */
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_CLICKABLE);

    ui_message_add_observer(rebuild);
    rebuild();   /* 首帧：按当前消息填一次 */
    return g_root;
}

void ui_notice_stack_set_visible(bool visible)
{
    g_page_allows = visible;
    /* 必须走 rebuild 而不能只摘/挂 HIDDEN：藏起来期间若有消息变更（比如进
     * 消息页 mark_all_read 触发观察者），rebuild 会先 lv_obj_clean 再因
     * !g_page_allows 提前返回，容器已被清空——回来只摘标志就是个空壳。 */
    rebuild();
}

void ui_notice_stack_set_top_offset(int32_t px)
{
    g_top_offset = px;
    reposition();
}

void ui_notice_stack_set_dark(bool dark)
{
    if(g_dark == dark) {
        return;
    }
    g_dark = dark;
    rebuild();
}

void ui_notice_stack_set_badge(bool on)
{
    if(g_badge_on == on) {
        return;
    }
    g_badge_on = on;
    /* 红点只画在铃铛上，其余两态无所谓；统一重建省得判当前是哪一态。 */
    rebuild();
}
