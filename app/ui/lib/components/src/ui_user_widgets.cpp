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
    /* 稿 4014:16631 / 2567:5555 起弹窗标题下不再带主色装饰线，全用户层统一去掉 */
    cfg.accent_underline = false;
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

lv_obj_t *ui_user_make_divider_vline(lv_obj_t *parent, lv_coord_t height)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 1, height);
    lv_obj_set_style_bg_color(line, user_color_divider, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

lv_obj_t *ui_user_make_overlay_round_btn(lv_obj_t *parent, const char *sym,
                                         int32_t size, lv_event_cb_t cb,
                                         lv_obj_t **out_icon)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    /* 只补到 UI_USER_MIN_TOUCH 为止：外扩是四边等量的，给多了会和相邻控件的命中区打架 */
    if(size < UI_USER_MIN_TOUCH) {
        lv_obj_set_ext_click_area(btn, (UI_USER_MIN_TOUCH - size + 1) / 2);
    }
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_color(icon, user_color_text, 0);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);
    lv_obj_center(icon);
    if(out_icon) {
        *out_icon = icon;
    }
    return btn;
}

void ui_user_make_tab_item(lv_obj_t *parent, const char *text,
                           const lv_font_t *font,
                           lv_event_cb_t cb, void *user_data,
                           lv_obj_t **out_root, lv_obj_t **out_label,
                           lv_obj_t **out_mark)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    /* 命中区外扩（不改布局与视觉）：横向把 Tab 之间那段死区覆盖上，竖向把文字上方
     * 也纳进来。外扩量取 USER_PAD_SM，小于 Tab 间距 USER_PAD_LG 的一半，故相邻两项
     * 的命中区不会重叠。 */
    lv_obj_set_ext_click_area(item, USER_PAD_SM);
    if(cb) {
        lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, text);
    /* 字号由调用方给：两页的稿值不同（消息页 16、历史页浅色稿 24）。**必须在量宽之前
     * 设好**，理由见头文件里 @param font 那段。 */
    lv_obj_set_style_text_font(lbl, font ? font : user_font_text_sm(), 0);

    /* 量出文字宽度后把项宽写死：SIZE_CONTENT 的容器里放百分比宽的子对象会互相
     * 依赖（下划线要按文字宽算，文字宽又决定容器宽），量一次直接给定值最省心。 */
    lv_obj_update_layout(item);
    const int32_t txt_w = lv_obj_get_width(lbl);
    lv_obj_set_width(item, txt_w);

    /*
     * 「文字居中 + 下划线紧跟其下」的相对关系，而不是稿上那套「文字贴行顶 + 下划线
     * 贴行底」的绝对定位。
     *
     * 绝对定位只在行高恰为稿值（1280 档 40）时成立：行高一改，多出来的空档全落在文字
     * 与下划线之间——文字看着偏上、下划线掉到行底，命中区中心还会被拖到文字中心下方
     * 一截（而 ext_click_area 是四边等量的，治不了这种不对称偏差）。改成相对关系后，
     * 整组随行高居中，三个档位与任何行高都不散架。
     */
    const int32_t mark_h = 2;
    const int32_t mark_gap = USER_PAD_SM / 2;
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -(mark_h + mark_gap) / 2);

    lv_obj_t *mark = lv_obj_create(item);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, txt_w / 2, mark_h);   /* 稿里下划线是文字宽的一半、居中 */
    lv_obj_set_style_bg_color(mark, user_color_accent, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    /* align_to 自己会先 update_layout，故不必在此额外补一次 */
    lv_obj_align_to(mark, lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, mark_gap);

    if(out_root) {
        *out_root = item;
    }
    if(out_label) {
        *out_label = lbl;
    }
    if(out_mark) {
        *out_mark = mark;
    }
}

lv_obj_t *ui_user_make_icon_img(lv_obj_t *parent, const lv_image_dsc_t *src,
                                int32_t target, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, target, target);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if(!src) {
        return box;
    }

    const int32_t src_sz = static_cast<int32_t>(src->header.w);
    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    /* 缩放绕中心：pivot 不挪到中心的话 lv_image_set_scale 会以左上角为基准，
     * 图会偏出固定盒。 */
    lv_image_set_pivot(img, src_sz / 2, src_sz / 2);
    if(src_sz > 0 && target != src_sz) {
        lv_image_set_scale(img, static_cast<uint32_t>(256 * target / src_sz));
    }
    lv_obj_center(img);
    return box;
}

const lv_image_dsc_t *ui_user_pick_icon(const lv_image_dsc_t *const *set, int32_t sz)
{
    if(!set || !set[0]) {
        return nullptr;
    }
    const lv_image_dsc_t *best = set[0];
    int32_t best_d = INT32_MAX;
    for(const lv_image_dsc_t *const *p = set; *p != nullptr; ++p) {
        const int32_t d = LV_ABS(static_cast<int32_t>((*p)->header.w) - sz);
        if(d < best_d) {
            best_d = d;
            best = *p;
        }
    }
    return best;
}

lv_obj_t *ui_user_make_primary_pill_btn(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                        const char *text, lv_event_cb_t cb,
                                        int32_t width, bool spread)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    /* 比通用主按钮再高一点（×5/4，贴近 Figma 64px 胶囊），避免过扁。 */
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H * 5 / 4);
    lv_obj_set_width(btn, width > 0 ? width : LV_SIZE_CONTENT);
    /* 蓝色对角(45°)渐变（对齐 Figma #4054FD → #4D80EE），带少量半透明玻璃感，
     * 与首页滑动解锁材质统一。bg_grad 只存指针，故渐变描述符用函数内 static。 */
    static lv_grad_dsc_t s_grad;
    static bool s_grad_ready = false;
    if(!s_grad_ready) {
        const lv_color_t cols[2] = { lv_color_hex(0x4054FD), lv_color_hex(0x4D80EE) };
        const lv_opa_t   opa[2]  = { LV_OPA_COVER, LV_OPA_COVER };
        const uint8_t    frac[2] = { 0, 255 };
        lv_grad_init_stops(&s_grad, cols, opa, frac, 2);
        lv_grad_linear_init(&s_grad, LV_GRAD_LEFT, LV_GRAD_TOP,
                            LV_GRAD_RIGHT, LV_GRAD_BOTTOM, LV_GRAD_EXTEND_PAD);
        s_grad_ready = true;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4054FD), 0);
    lv_obj_set_style_bg_grad(btn, &s_grad, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);              /* 保留一点通透 */
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);       /* 胶囊 */
    /* 玻璃亮边（近似 Figma 白色渐变描边）+ 柔和蓝色投影 */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(btn, 16, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 4, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x4054FD), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_ver(btn, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn,
                          spread ? LV_FLEX_ALIGN_SPACE_BETWEEN : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    if(icon) {
        ui_user_make_icon_img(btn, icon, USER_BTN_PRIMARY_H * 5 / 8, lv_color_white());
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    return btn;
}

lv_obj_t *ui_user_make_confirm_footer_btn(lv_obj_t *footer, const char *text,
                                          bool filled, lv_color_t fill_color,
                                          bool light,
                                          lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_remove_style_all(btn);
    /* 稿上两枚各 212、间距 24，正好铺满 496 卡片的内宽，所以用 flex_grow 而不是定宽 */
    lv_obj_set_flex_grow(btn, 1);
    /* 浅色档的按钮比深色档高一档、圆角也大一档（稿 6251:94239 是 72 / 12，深色那套是
     * 64 / 8）。两个量都从现成 token 推，不另开字段：
     *   高   USER_DLG_FOOTER_BTN_H * 9/8  → 108 / 72 / 57（稿 ×1.5 / ×0.8 = 108 / 57.6）
     *   圆角 USER_LIGHT_RADIUS / 2        → 18 / 12 / 9 */
    lv_obj_set_height(btn, light ? USER_DLG_FOOTER_BTN_H * 9 / 8 : USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, light ? USER_LIGHT_RADIUS / 2 : USER_BTN_RADIUS, 0);
    if(filled) {
        lv_obj_set_style_bg_color(btn, fill_color, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    } else if(light) {
        /* 稿 #333145 @10%：浅色卡上要压深色，沿用深色档那套「白 4%」会整个消失。
         * LV_OPA_10 是 25/255，白底上混出来正好是稿的 (235,235,237)。 */
        lv_obj_set_style_bg_color(btn, user_light_text, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_10, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    } else {
        /* 稿是 rgba(255,255,255,0.04)：白底压到 4%（LV_OPA 里约 10/255） */
        lv_obj_set_style_bg_color(btn, user_color_text, 0);
        lv_obj_set_style_bg_opa(btn, 10, 0);
        lv_obj_set_style_bg_opa(btn, 30, LV_STATE_PRESSED);
    }
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    /* 填充档两边都是白字（底是实色主色/危险色）；非填充档跟着卡片底走 */
    lv_obj_set_style_text_color(lbl,
        (light && !filled) ? user_light_text : user_color_text, 0);
    /* 浅色档的按钮文字稿上是 22（稿 6024:23245/23259 的 Poppins Medium 22/32），
     * 比深色档那套的 18 大两档。
     *
     * 原先借的是 user_font_title()（24/20/16），1280 档落在 20——比稿小一档，实机量到
     * 字身窄约 9%。现在走单开的 user_font_dlg_btn（32/22/18），1280 档正中稿值。
     *
     * 字重追不平：稿是 Medium，项目字库只有一个字重（同 apply_tab_style() 那里的处境）。 */
    lv_obj_set_style_text_font(lbl, light ? user_font_dlg_btn() : user_font_text(), 0);
    lv_obj_center(lbl);
    return btn;
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
 *  index 越界（含哨兵 UI_USER_SEG_NONE）= 不选中任何段，用于「必须由用户
 *  显式选择」的场景（如打印板厚度）。
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

    segmented_apply_state(seg, selected < count ? selected : UI_USER_SEG_NONE);
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
        return UI_USER_SEG_NONE;
    }
    return static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(seg)));
}

lv_obj_t *ui_user_make_footer_ghost_btn(lv_obj_t *footer, const char *text,
                                        lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    /* 与 ui_user_make_confirm_footer_btn(filled=false) 同一档副按钮观感：
     * USER_BTN_RADIUS 圆角 + 4% 白底、无描边。原先套的 user_style_btn_ghost()
     * 是 panel 实心 + 1px 描边 + USER_RADIUS，跟其余弹窗对不上。 */
    lv_obj_set_style_radius(btn, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_color_text, 0);
    lv_obj_set_style_bg_opa(btn, 10, 0);
    lv_obj_set_style_bg_opa(btn, 30, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
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
