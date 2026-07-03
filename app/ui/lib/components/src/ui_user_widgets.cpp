#include "ui_user_widgets.hpp"
#include "ui_theme.hpp"

#include <cstdio>
#include <cstdint>

ui_modal_dialog_cfg_t ui_modal_dialog_cfg_user(const char *title,
                                               int32_t card_width,
                                               bool with_footer)
{
    ui_modal_dialog_cfg_t cfg = {};
    cfg.title_text = title;
    cfg.card_width = card_width > 0 ? card_width : USER_DLG_W;
    cfg.with_footer = with_footer;
    cfg.accent_underline = true;
    return cfg;
}

lv_obj_t *ui_user_make_body_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, user_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    return lbl;
}

lv_obj_t *ui_user_make_ghost_link_btn(lv_obj_t *parent, const char *text,
                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_PCT(80), USER_DLG_BTN_GHOST_H);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, user_color_panel_alt, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, user_color_accent, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_center(lbl);
    return btn;
}

void ui_user_format_duration_min(char *buf, size_t buf_size, uint32_t duration_min)
{
    if(!buf || buf_size == 0) {
        return;
    }
    if(duration_min >= 60) {
        std::snprintf(buf, buf_size, "%uh %umin",
                      duration_min / 60, duration_min % 60);
    } else {
        std::snprintf(buf, buf_size, "%umin", duration_min);
    }
}

void ui_user_format_file_meta(char *buf, size_t buf_size,
                              uint32_t duration_min, float material_g)
{
    if(!buf || buf_size == 0) {
        return;
    }
    char dur[32];
    ui_user_format_duration_min(dur, sizeof(dur), duration_min);
    std::snprintf(buf, buf_size, "%s · %.1fg", dur, static_cast<double>(material_g));
}

lv_obj_t *ui_user_make_accent_preview(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, user_color_accent_dim, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, 0);
    lv_obj_set_style_bg_grad_color(obj, user_color_accent, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(obj, USER_RADIUS, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *ui_user_make_progress_bar(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w, h);

    lv_obj_set_style_bg_color(bar, user_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, user_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

lv_obj_t *ui_user_make_spinner_container(lv_obj_t *parent, int32_t inner_size)
{
    lv_obj_t *vc = lv_obj_create(parent);
    lv_obj_remove_style_all(vc);
    lv_obj_set_size(vc, inner_size + USER_PAD, inner_size + USER_PAD);
    lv_obj_set_style_bg_opa(vc, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(vc, LV_OBJ_FLAG_SCROLLABLE);
    return vc;
}

lv_obj_t *ui_user_make_frosted_card(lv_obj_t *parent, bool flex_grow)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    if(flex_grow) {
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, LV_PCT(100));
    } else {
        lv_obj_set_height(card, LV_SIZE_CONTENT);
    }
    lv_obj_set_style_radius(card, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, user_color_divider, 0);
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_user_make_divider_line(lv_obj_t *parent, lv_coord_t width)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, width, 1);
    lv_obj_set_style_bg_color(line, user_color_divider, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

lv_obj_t *ui_user_make_metric_block(lv_obj_t *parent, const char *caption,
                                    lv_text_align_t text_align,
                                    lv_obj_t **out_value)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_width(block, LV_PCT(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);

    ui_user_make_divider_line(block, USER_VALUE_LABEL_W);

    lv_obj_t *cap = lv_label_create(block);
    lv_label_set_text(cap, caption ? caption : "");
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_style_text_align(cap, text_align, 0);

    lv_obj_t *val = lv_label_create(block);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_value(), 0);
    lv_obj_set_width(val, LV_PCT(100));
    lv_obj_set_style_text_align(val, text_align, 0);
    if(out_value) {
        *out_value = val;
    }
    return block;
}

lv_obj_t *ui_user_make_status_dot(lv_obj_t *parent, bool ready)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, USER_STATUS_DOT_SZ, USER_STATUS_DOT_SZ);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot,
        ready ? user_color_status_ok : user_color_status_err, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    return dot;
}

lv_obj_t *ui_user_make_supply_indicator(lv_obj_t *parent, const char *name,
                                        bool ready)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    ui_user_make_status_dot(row, ready);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name ? name : "");
    lv_obj_set_style_text_color(name_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(name_lbl, user_font_text(), 0);

    lv_obj_t *status_lbl = lv_label_create(row);
    lv_label_set_text(status_lbl, ready ? "Ready" : "Not Ready");
    lv_obj_set_style_text_color(status_lbl,
        ready ? user_color_status_ok : user_color_status_err, 0);
    lv_obj_set_style_text_font(status_lbl, user_font_caption(), 0);

    return row;
}

lv_obj_t *ui_user_make_supply_bar(lv_obj_t *parent, const char *name,
                                  bool ok, int percent,
                                  lv_obj_t **out_bar,
                                  lv_obj_t **out_pct_lbl,
                                  lv_obj_t **out_status_lbl)
{
    if(percent < 0)   percent = 0;
    if(percent > 100) percent = 100;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 第一行：名称 + 状态 */
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_lbl = lv_label_create(head);
    lv_label_set_text(name_lbl, name ? name : "");
    lv_obj_set_style_text_color(name_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(name_lbl, user_font_text(), 0);

    lv_obj_t *status_lbl = lv_label_create(head);
    lv_label_set_text(status_lbl, ok ? "Adequate" : "Low");
    lv_obj_set_style_text_color(status_lbl,
        ok ? user_color_status_ok : user_color_status_err, 0);
    lv_obj_set_style_text_font(status_lbl, user_font_caption(), 0);

    /* 第二行：进度条 + 百分比 */
    lv_obj_t *bar_row = lv_obj_create(card);
    lv_obj_remove_style_all(bar_row);
    lv_obj_set_width(bar_row, LV_PCT(100));
    lv_obj_set_height(bar_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(bar_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = ui_user_make_progress_bar(bar_row, LV_PCT(100), USER_PROGRESS_H);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_style_bg_color(bar,
        ok ? user_color_status_ok : user_color_status_err, LV_PART_INDICATOR);
    lv_bar_set_value(bar, percent, LV_ANIM_OFF);

    lv_obj_t *pct_lbl = lv_label_create(bar_row);
    lv_label_set_text_fmt(pct_lbl, "%d%%", percent);
    lv_obj_set_style_text_color(pct_lbl,
        ok ? user_color_accent : user_color_status_err, 0);
    lv_obj_set_style_text_font(pct_lbl, user_font_caption(), 0);

    if(out_bar)        *out_bar = bar;
    if(out_pct_lbl)    *out_pct_lbl = pct_lbl;
    if(out_status_lbl) *out_status_lbl = status_lbl;
    return card;
}

lv_obj_t *ui_user_make_setting_row(lv_obj_t *parent, const char *label,
                                   const char *btn_text, lv_event_cb_t btn_cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, USER_PAD_SM, 0);
    lv_obj_set_style_pad_hor(row, USER_PAD_SM, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_color(row, user_color_divider, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label ? label : "");
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);

    if(btn_text && btn_cb) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_add_style(btn, user_style_btn_ghost(), 0);
        lv_obj_set_size(btn, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
        lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, btn_text);
        lv_obj_center(btn_lbl);
    }
    return row;
}

/* ---------------------------------------------------------------------------
 *  分段选择控件
 *  选中态完全由各段按钮的 LV_STATE_CHECKED 样式驱动，切换时只改 state，
 *  并把当前选中索引缓存在容器的 user_data 上。
 * ------------------------------------------------------------------------- */
static void segmented_apply_state(lv_obj_t *seg, uint8_t index)
{
    const uint32_t n = lv_obj_get_child_count(seg);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(seg, i);
        if(!btn) {
            continue;
        }
        if(i == index) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(btn, LV_STATE_CHECKED);
        }
    }
    lv_obj_set_user_data(seg, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
}

static void segmented_seg_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *seg = lv_obj_get_parent(btn);
    if(!seg) {
        return;
    }
    const uint8_t index =
        static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
    segmented_apply_state(seg, index);
    lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, nullptr);
}

lv_obj_t *ui_user_make_segmented(lv_obj_t *parent,
                                 const char *const labels[], uint8_t count,
                                 uint8_t selected)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_height(seg, LV_SIZE_CONTENT);
    lv_obj_set_width(seg, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(seg, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(seg, lv_color_hex(0x08080B), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(seg, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(seg, USER_PAD_SM / 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);

    for(uint8_t i = 0; i < count; i++) {
        lv_obj_t *btn = lv_button_create(seg);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_radius(btn, USER_RADIUS - 2 > 0 ? USER_RADIUS - 2 : USER_RADIUS, 0);
        lv_obj_set_style_pad_ver(btn, USER_PAD_SM, 0);
        lv_obj_set_style_pad_hor(btn, USER_PAD_SM, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        /* 选中段：浅白填充 + 白字（对齐设计稿 0.08 白）；
         * 文字色设在按钮上，子 label 继承（未选灰、选中白） */
        lv_obj_set_style_bg_color(btn, user_color_text, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, 24, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, user_color_text_dim, 0);
        lv_obj_set_style_text_color(btn, user_color_text, LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, segmented_seg_clicked, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels && labels[i] ? labels[i] : "");
        lv_obj_set_style_text_font(lbl, user_font_text(), 0);
        lv_obj_center(lbl);
    }

    segmented_apply_state(seg, selected < count ? selected : 0);
    return seg;
}

void ui_user_segmented_set_selected(lv_obj_t *seg, uint8_t index)
{
    if(seg) {
        segmented_apply_state(seg, index);
    }
}

uint8_t ui_user_segmented_get_selected(lv_obj_t *seg)
{
    if(!seg) {
        return 0;
    }
    return static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(seg)));
}

lv_obj_t *ui_user_make_footer_ghost_btn(lv_obj_t *footer, const char *text,
                                        lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_add_style(btn, user_style_btn_ghost(), 0);
    lv_obj_set_size(btn, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_center(lbl);
    return btn;
}
