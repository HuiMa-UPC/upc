#include "ui_notice_stack.hpp"

#include "ui_message_store.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "page_manager.hpp"
#include "home_metric_icons.h"

#include <cstdio>

namespace {

/** 浮层最多摆两张卡，多出来的收进「查看剩余 N 条」。 */
constexpr int kMaxCards = 2;

lv_obj_t *g_root          = nullptr;
int32_t   g_top_offset    = 0;
/** 当前页允不允许显示（由 PageManager 按页设）。与"有没有消息"是两个条件，
 *  必须分开记：只用 HIDDEN 标志的话，重建时无从知道是哪一边把它藏起来的。 */
bool      g_page_allows   = true;

void reposition(void)
{
    if(!g_root) {
        return;
    }
    /* 整栈建在铺满屏的 shell_ 上，贴右上角。退一个 USER_PAD_LG（稿 32）——与首页打印态/
     * 监控态右上角那块环境区（build_env_wrap）同一个退让量，两块上下相接看着是一栏；
     * 这个量对所有页面所有状态都一样，不按页特例。纵向的让位量才是按态给的：稿
     * 4972:27560 里这栈在 y=104，而 32(环境区顶) + 64(环境区高) + 8(间距) 正是 104。 */
    lv_obj_align(g_root, LV_ALIGN_TOP_RIGHT, -USER_PAD_LG, USER_PAD_LG + g_top_offset);
}

void dismiss_async(void *code_raw)
{
    ui_message_dismiss((uint16_t)(uintptr_t)code_raw);
}

void on_close_click(lv_event_t *e)
{
    /* 撤下会 lv_obj_clean 整栈，也就是把正在派发事件的这颗 × 一起删掉。
     * 推迟到事件之外做，否则回到 LVGL 的派发循环时对象已经没了。 */
    lv_async_call(dismiss_async, lv_event_get_user_data(e));
}

void goto_message_page_async(void *)
{
    ui::PageManager::instance().switch_to("user_message");
}

void on_more_click(lv_event_t *)
{
    /* 同 on_close_click：进消息页会把消息全标已读 → 整栈重建 → 本按钮被删。 */
    lv_async_call(goto_message_page_async, nullptr);
}

/** 一张通知卡：色块图标 + 标题/副标题两行，信息卡右上多一颗 ×。 */
void build_card(lv_obj_t *parent, const ui_message_t *m)
{
    /* 只有信息卡带 ×（告警级不可关，见 ui_message_dismiss）。配色不再由这个
     * 布尔决定：四档各自的色走 ui_message_severity_color()。 */
    const bool closable = (m->severity == UI_MSG_SEV_INFO);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(card, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
    /* 稿 5277:11715 的圆角是 8/0/0/8——右侧两角是直的，因为稿上这摞卡紧贴屏幕右边缘。
     * 这里整栈退了一个 USER_PAD 与环境区对齐，没有贴边，故四角一律圆。 */
    lv_obj_set_style_radius(card, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 稿上是 32 见方的实色圆角块，里面一枚 18 的白图标。底色与图标走
     * ui_message_store 那份共用的等级映射，同一条消息在消息页列表里同色。 */
    const int32_t box_sz = USER_PRINT_BADGE_SZ * 2;
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, box_sz, box_sz);
    lv_obj_set_style_bg_color(box, ui_message_severity_color(m->severity), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = ui_user_make_icon_img(box,
        ui_message_severity_icon(m->severity), box_sz * 9 / 16, lv_color_white());
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
     * 透明度区分。Montserrat 只有一个字重，字重那半做不出来，所以只落地「同号 + 透明度」
     * ——两处都取 user_font_text_sm()。原先是标题 18 / 副标题 14，字号差反而盖过了
     * 稿想要的那种「同号、靠深浅分层」的观感。 */
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

/** 卡片下面那条「查看剩余 N 条」，点进消息页。
 *  稿 5277:11779 的写法是文字靠左、右端一枚 ›，两端对齐。 */
void build_summary_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
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
    /* 与卡片标题同号（稿 4972:27560 的 5281:11817 也是 16）。 */
    lv_obj_set_style_text_font(lbl, user_font_text_sm(), 0);

    lv_obj_t *chevron = lv_label_create(btn);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, lv_color_white(), 0);
    lv_obj_set_style_text_opa(chevron, LV_OPA_60, 0);
    lv_obj_set_style_text_font(chevron, user_font_caption(), 0);
}

/** 消息集合一变就整栈重建。条数封顶三个子节点，重建比增量维护省心得多。 */
void rebuild(void)
{
    if(!g_root) {
        return;
    }
    lv_obj_clean(g_root);

    const int total = ui_message_visible_count();
    if(total <= 0 || !g_page_allows) {
        lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    const int shown = total < kMaxCards ? total : kMaxCards;
    for(int i = 0; i < shown; i++) {
        build_card(g_root, ui_message_visible_at(i));
    }
    if(total > shown) {
        char text[64];
        std::snprintf(text, sizeof(text), "View remaining %d messages",
                      total - shown);
        build_summary_row(g_root, text);
    }
    reposition();   /* 高度随条数变，重新贴一次 */
}

} /* namespace */

lv_obj_t *ui_notice_stack_create(lv_obj_t *parent)
{
    if(g_root) {
        return g_root;
    }

    g_root = lv_obj_create(parent);
    lv_obj_remove_style_all(g_root);
    /* FLOATING：shell_ 是横向 flex，本栈不能占格位（同 menu_panel_ / 设备信息条） */
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_FLOATING);
    /* 宽度走本栈自己的 USER_NOTICE_CARD_W = 400@1280。
     * **稿自己在两个 frame 里给了不同的值**：首页待机 5339:27299/27310 是 400，而新版
     * 打印态 4972:27560 的 5281:11797 是 416（与 stats-card 同宽）。通知栈是建在 shell_
     * 上的全局对象、两个态共用一个实例，只能取一个值——这里以待机 frame 为准取 400。
     * 早先与 stats-card 共用 USER_PRINT_CARD_W，是因为那时整栈还摆在左下、要与那张卡
     * 左右对齐；挪到右上之后这层对齐关系已经不存在了。 */
    lv_obj_set_width(g_root, USER_NOTICE_CARD_W);
    lv_obj_set_height(g_root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_root, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    /* 容器本身不可点，免得空白处吃掉底下页面的触摸；卡片里的 × 和按钮各自可点 */
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
