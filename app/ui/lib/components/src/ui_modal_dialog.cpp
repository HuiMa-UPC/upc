#include "ui_modal_dialog.hpp"
#include "ui_theme.hpp"

#include <cstdlib>

/* 屏蔽点击穿透：遮罩自己就是按钮，但点击不做任何事。 */
static void backdrop_eat_click_cb(lv_event_t *e)
{
    lv_event_stop_processing(e);
}

ui_modal_dialog_t *ui_modal_dialog_open(lv_obj_t *parent,
                                        const ui_modal_dialog_cfg_t *cfg)
{
    if(!parent || !cfg) {
        return nullptr;
    }

    auto *dlg = static_cast<ui_modal_dialog_t *>(
        std::calloc(1, sizeof(ui_modal_dialog_t)));
    if(!dlg) {
        return nullptr;
    }

    /* ---- 背景遮罩 ---- */
    lv_obj_t *backdrop = lv_obj_create(parent);
    lv_obj_remove_style_all(backdrop);
    /* 铺满父容器。**这一行删不得**：remove_style_all 已经把主题给的尺寸清空，没有它
     * 遮罩会退回 LVGL 的默认边长 LV_DPI_DEF(130)，卡片在 130×130 里居中后绝大部分被
     * 裁到屏外，只在左上角露出中间一小条，且整屏不再压暗。踩过一次。 */
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    /* 浅色档的遮罩比深色档淡得多：白卡本身够抢眼，再压 70% 会让底下那页看着像断电。
     * 新稿（6423:51743）量下来是 **#333145 @28%** 而不是纯黑——压在 #fafafa 的页底上
     * 带一点蓝紫调，与整套浅色是同一个墨色。旧值是 black @50%（稿 6251:94118）。
     * 数字键盘那块遮罩仍是黑 @50%（稿 6363:44775 自己标了「50%黑色蒙版」），它不走
     * 本控件、在 ui_keyboard.cpp 里另给。 */
    lv_obj_set_style_bg_color(backdrop,
        cfg->light_theme ? user_light_text : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop,
        cfg->light_theme ? USER_LIGHT_BACKDROP_OPA : USER_DLG_BACKDROP_OPA, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    /* 让遮罩自身可点（吞掉穿透到底层 home 的事件） */
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, backdrop_eat_click_cb, LV_EVENT_CLICKED, nullptr);
    dlg->root = backdrop;

    /* ---- 弹窗主体卡片 ---- */
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    const int32_t w = cfg->card_width > 0 ? cfg->card_width : USER_DLG_W;
    lv_obj_set_width(card, w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    /* 两档只差外壳配色与几个量；结构完全一样，所以做成开关而不是另起一套控件。
     * 浅色档的几何值都落回了现成 token（稿 1280 档：圆角 24 / 内距 24 / 行距 16）：
     *   圆角 USER_LIGHT_RADIUS   36 / 24 / 19（稿 ×1.5 / ×0.8 = 36 / 19.2）
     *   内距 USER_PAD            40 / 24 / 22
     *   行距 USER_PAD_SM         20 / 16 / 11
     * 卡宽也不必分档：USER_DLG_W 的 1280 档就是稿值 496。 */
    lv_obj_set_style_bg_color(card, cfg->light_theme ? user_light_card : user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, cfg->light_theme ? USER_LIGHT_RADIUS : USER_DLG_RADIUS, 0);
    if(cfg->light_theme) {
        /* 稿上白卡只有一道极淡的外描边（white @10%），压在 50% 遮罩上几乎看不见，
         * 这里索性不画；投影也收掉——深色档那 60/60% 的重投影是给深底用的。 */
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
    } else {
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, user_color_divider, 0);
        lv_obj_set_style_shadow_width(card, 60, 0);
        lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);
    }
    const int32_t card_pad = cfg->light_theme ? USER_PAD : USER_DLG_PAD;
    lv_obj_set_style_pad_all(card, card_pad, 0);
    /* 浅色档的行距与内距不同值：稿把标题与正文收在同一个 16 间距的组里，组与页脚
     * 之间才是 32。card 的 pad_row 只有一个值，所以取 16 管住「标题→正文」，
     * 「正文→页脚」那 32 由它再加上页脚自己的 pad_top（同样 16）凑出来。 */
    lv_obj_set_style_pad_row(card, cfg->light_theme ? USER_PAD_SM : card_pad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(card);
    dlg->card = card;

    /* ---- 标题 ---- */
    if(cfg->title_text && cfg->title_text[0]) {
        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, cfg->title_text);
        lv_obj_set_style_text_color(title,
            cfg->light_theme ? user_light_text : user_color_text, 0);
        /* 浅色档另有一档标题字号（稿 28@1280）：user 系在那一档正好跳过 28，沿用
         * value(32) 实机量到卡片比稿高 9px、标题字身大 13%。深色那套不动。 */
        lv_obj_set_style_text_font(title,
            cfg->light_theme ? user_font_dlg_title() : user_font_value(), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(title, LV_PCT(100));
        if(cfg->light_theme) {
            /* 把 Montserrat 的 line_height 撑回稿的行盒，见 USER_DLG_TITLE_LEAD。
             * 上多下少是照稿的基线位置分的（稿基线在 39 的行盒里偏下）。 */
            const int32_t lead = USER_DLG_TITLE_LEAD;
            lv_obj_set_style_margin_top(title, lead - lead / 2, 0);
            lv_obj_set_style_margin_bottom(title, lead / 2, 0);
        }
        /* 有右上角关闭按钮时，左右对称预留其宽度，避免居中标题压到 "X" */
        if(cfg->with_close_btn) {
            lv_obj_set_style_pad_left(title, USER_BACK_BTN_H + USER_PAD_SM, 0);
            lv_obj_set_style_pad_right(title, USER_BACK_BTN_H + USER_PAD_SM, 0);
        }
        dlg->title = title;

        if(cfg->accent_underline) {
            lv_obj_t *line = lv_obj_create(card);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, USER_PAD_LG, 4);
            lv_obj_set_style_bg_color(line, user_color_accent, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(line, 2, 0);
            lv_obj_set_style_margin_bottom(line, USER_PAD_SM, 0);
        }
    }

    /* ---- body slot ---- */
    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    if(cfg->light_theme) {
        /* 同标题，补正文的行盒差；加在 slot 上而不是标签上，这样页面往 body 里塞
         * 什么都不用管这件事。见 USER_DLG_BODY_LEAD。 */
        const int32_t lead = USER_DLG_BODY_LEAD;
        lv_obj_set_style_pad_top(body, lead - lead / 2, 0);
        lv_obj_set_style_pad_bottom(body, lead / 2, 0);
    }
    lv_obj_set_style_pad_row(body, USER_PAD, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    dlg->body = body;

    /* ---- footer slot ---- */
    if(cfg->with_footer) {
        lv_obj_t *footer = lv_obj_create(card);
        lv_obj_remove_style_all(footer);
        lv_obj_set_width(footer, LV_PCT(100));
        lv_obj_set_height(footer, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_top(footer, USER_PAD_SM, 0);
        lv_obj_set_style_pad_column(footer, USER_PAD, 0);
        lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
        dlg->footer = footer;
    }

    /* ---- 右上角关闭按钮（可选）---- */
    if(cfg->with_close_btn) {
        lv_obj_t *btn = lv_button_create(card);
        lv_obj_remove_style_all(btn);
        /* 用方形触摸目标，尺寸跟随 profile（1024≈40 / 1280≈44 / 1920≈64）*/
        lv_obj_set_size(btn, USER_BACK_BTN_H, USER_BACK_BTN_H);
        /* 脱离 flex 布局，绝对贴卡片右上角（受 card pad 约束，留出内边距）*/
        lv_obj_add_flag(btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(btn,
            cfg->light_theme ? user_light_nav_on : user_color_panel_alt, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn,
            cfg->light_theme ? LV_OPA_10 : LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *x = lv_label_create(btn);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(x,
            cfg->light_theme ? user_light_text : user_color_text_dim, 0);
        lv_obj_set_style_text_font(x, user_font_icon(), 0);
        lv_obj_center(x);

        dlg->close_btn = btn;
    }

    /* 弹窗刚出现时做一个 fade-in，避免突兀 */
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(backdrop, 180, 0);
    lv_obj_move_foreground(backdrop);

    return dlg;
}

void ui_modal_dialog_close(ui_modal_dialog_t *dlg)
{
    if(!dlg) {
        return;
    }
    if(dlg->root) {
        lv_obj_delete(dlg->root);
    }
    std::free(dlg);
}

void ui_modal_dialog_set_title(ui_modal_dialog_t *dlg, const char *text)
{
    if(!dlg || !dlg->title) {
        return;
    }
    lv_label_set_text(dlg->title, text ? text : "");
}
