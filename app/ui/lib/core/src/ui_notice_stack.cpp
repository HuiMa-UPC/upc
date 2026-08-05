#include "ui_notice_stack.hpp"

#include "ui_message_store.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "page_manager.hpp"
#include "home_metric_icons.h"

namespace {

/** 稿上两种卡的图标底色（4903:7118 告警红 / 4903:7132 信息蓝）。 */
constexpr uint32_t kSevWarnColor = 0xFF4242;
constexpr uint32_t kSevInfoColor = 0x485DFF;

/** 浮层最多摆两张卡，多出来的收进「查看剩余 N 条」。 */
constexpr int kMaxCards = 2;

lv_obj_t *g_root          = nullptr;
int32_t   g_bottom_offset = 0;
/** 当前页允不允许显示（由 PageManager 按页设）。与"有没有消息"是两个条件，
 *  必须分开记：只用 HIDDEN 标志的话，重建时无从知道是哪一边把它藏起来的。 */
bool      g_page_allows   = true;

void reposition(void)
{
    if(!g_root) {
        return;
    }
    /* 整栈建在铺满屏的 shell_ 上，左边要自己让开浮层侧栏，否则会钻到侧栏底下
     * （侧栏可点，压住的部分连触摸都收不到）。 */
    lv_obj_align(g_root, LV_ALIGN_BOTTOM_LEFT,
                 USER_SIDEBAR_W + USER_PAD, -USER_PAD - g_bottom_offset);
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
    const bool warn = (m->severity != UI_MSG_SEV_INFO);

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
    lv_obj_set_style_radius(card, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 稿上是 32 见方的实色圆角块，里面一枚 18 的白图标 */
    const int32_t box_sz = USER_PRINT_BADGE_SZ * 2;
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, box_sz, box_sz);
    lv_obj_set_style_bg_color(box,
        lv_color_hex(warn ? kSevWarnColor : kSevInfoColor), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = ui_user_make_icon_img(box,
        warn ? &ic_alert : &ic_toast_info, box_sz * 9 / 16, lv_color_white());
    lv_obj_center(icon);

    lv_obj_t *texts = lv_obj_create(card);
    lv_obj_remove_style_all(texts);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(texts, USER_PAD_SM / 2, 0);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);
    /* 信息卡右上那颗 × 是浮层子节点，不占 flex 格位，文字得自己躲开它 */
    if(!warn) {
        lv_obj_set_style_pad_right(texts, USER_PAD_SM * 3 / 2, 0);
    }

    lv_obj_t *title = lv_label_create(texts);
    lv_label_set_text(title, m->title);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);

    if(m->body && m->body[0]) {
        lv_obj_t *sub = lv_label_create(texts);
        lv_label_set_text(sub, m->body);
        lv_obj_set_style_text_color(sub, lv_color_white(), 0);
        lv_obj_set_style_text_opa(sub, LV_OPA_60, 0);
        lv_obj_set_style_text_font(sub, user_font_caption(), 0);
    }

    if(!warn) {
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

/** 卡片下面那条「查看剩余 N 条」，点进消息页。 */
void build_more_button(lv_obj_t *parent, int remaining)
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
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_more_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "View remaining %d messages", remaining);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_center(lbl);
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
        build_more_button(g_root, total - shown);
    }
    reposition();   /* 高度随条数变，重新贴一次底 */
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
    /* 宽度跟 stats-card 一致，打印态下两者左右边缘齐平 */
    lv_obj_set_width(g_root, USER_PRINT_CARD_W);
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
    if(!g_root) {
        return;
    }
    if(visible && ui_message_visible_count() > 0) {
        lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_notice_stack_set_bottom_offset(int32_t px)
{
    g_bottom_offset = px;
    reposition();
}
