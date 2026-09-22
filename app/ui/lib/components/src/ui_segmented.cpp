#include "ui_segmented.hpp"

#include <cstdlib>

#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"

namespace {

struct SegItem {
    lv_obj_t *btn   = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *icon  = nullptr;
    bool      enabled = true;
};

/* 轨道的 user_data。生命周期跟着轨道对象走，LV_EVENT_DELETE 里释放。 */
struct SegCtx {
    uint8_t    count  = 0;
    uint8_t    active = UI_SEGMENTED_NONE;
    lv_color_t color_on{};
    lv_color_t color_off{};
    lv_color_t color_dis{};
    SegItem    items[UI_SEGMENTED_MAX_ITEMS];
};

SegCtx *ctx_of(const lv_obj_t *seg)
{
    return seg ? static_cast<SegCtx *>(lv_obj_get_user_data(const_cast<lv_obj_t *>(seg)))
               : nullptr;
}

/** 按当前 active / enabled 刷一遍配色与选中底板。
 *
 *  图标色不能靠继承：`text_color` 只管 label，`lv_image` 要的是 `image_recolor`，
 *  而图标是按钮的子节点、不会跟着按钮进 CHECKED 态。所以两者都在这里显式写。 */
void apply_state(lv_obj_t *seg)
{
    SegCtx *c = ctx_of(seg);
    if(!c) {
        return;
    }
    for(uint8_t i = 0; i < c->count; i++) {
        SegItem &it = c->items[i];
        if(!it.btn) {
            continue;
        }
        const bool sel = (i == c->active);
        if(sel) {
            lv_obj_add_state(it.btn, LV_STATE_CHECKED);
        }
        else {
            lv_obj_clear_state(it.btn, LV_STATE_CHECKED);
        }
        const lv_color_t col = !it.enabled ? c->color_dis
                                           : (sel ? c->color_on : c->color_off);
        if(it.label) {
            lv_obj_set_style_text_color(it.label, col, 0);
        }
        if(it.icon) {
            lv_obj_set_style_image_recolor(it.icon, col, 0);
            lv_obj_set_style_image_recolor_opa(it.icon, LV_OPA_COVER, 0);
        }
    }
}

void seg_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *seg = lv_obj_get_parent(btn);
    SegCtx   *c   = ctx_of(seg);
    if(!c) {
        return;
    }
    const auto idx = static_cast<uint8_t>(
        reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
    if(idx >= c->count || !c->items[idx].enabled || idx == c->active) {
        return;
    }
    c->active = idx;
    apply_state(seg);
    lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, nullptr);
}

void seg_deleted(lv_event_t *e)
{
    auto *c = static_cast<SegCtx *>(lv_event_get_user_data(e));
    delete c;
}

}  // namespace

lv_obj_t *ui_segmented_create(lv_obj_t *parent, const ui_segmented_cfg_t *cfg)
{
    if(!parent || !cfg || !cfg->items || cfg->count == 0) {
        return nullptr;
    }
    const uint8_t count = cfg->count > UI_SEGMENTED_MAX_ITEMS ? UI_SEGMENTED_MAX_ITEMS
                                                              : cfg->count;

    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_width(seg, cfg->track_w > 0 ? cfg->track_w : LV_PCT(100));
    lv_obj_set_height(seg, cfg->track_h > 0 ? cfg->track_h : LV_SIZE_CONTENT);
    lv_obj_set_style_radius(seg, cfg->track_radius, 0);
    lv_obj_set_style_bg_color(seg, cfg->track_color, 0);
    lv_obj_set_style_bg_opa(seg, cfg->track_opa, 0);
    lv_obj_set_style_pad_all(seg, cfg->inset, 0);
    lv_obj_set_style_pad_column(seg, cfg->gap, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);

    auto *c = new SegCtx();
    c->count     = count;
    c->color_on  = cfg->color_on;
    c->color_off = cfg->color_off;
    c->color_dis = cfg->color_disabled;
    lv_obj_set_user_data(seg, c);
    lv_obj_add_event_cb(seg, seg_deleted, LV_EVENT_DELETE, c);

    for(uint8_t i = 0; i < count; i++) {
        const ui_segmented_item_t &src = cfg->items[i];

        lv_obj_t *btn = lv_button_create(seg);
        /* `remove_style_all` 不只是清样式，也是在躲默认主题：主题给每个按钮的
         * CHECKED 态都挂了 `bg_color_secondary`（`lv_theme_default.c`），而副色是
         * 自动初始化时传的 LV_PALETTE_RED——不清的话选中段会是一块不透明红底。
         * 侧栏那五枚导航按钮踩过同一个坑，见 page_manager.cpp。 */
        lv_obj_remove_style_all(btn);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_style_radius(btn, cfg->pill_radius, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(btn, cfg->pill_color, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, cfg->pill_opa, LV_STATE_CHECKED);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_row(btn, src.icon_set && src.text ? cfg->icon_gap : 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(btn, seg_clicked, LV_EVENT_CLICKED, nullptr);

        SegItem &it = c->items[i];
        it.btn = btn;

        if(src.icon_set) {
            const int32_t sz = cfg->icon_sz > 0 ? cfg->icon_sz : USER_METRIC_ICON_SZ;
            const lv_image_dsc_t *dsc = ui_user_pick_icon(src.icon_set, sz);
            if(dsc) {
                it.icon = lv_image_create(btn);
                lv_image_set_src(it.icon, dsc);
                lv_obj_remove_flag(it.icon, LV_OBJ_FLAG_CLICKABLE);
            }
        }
        if(src.text) {
            it.label = lv_label_create(btn);
            lv_label_set_text(it.label, src.text);
            if(cfg->font) {
                lv_obj_set_style_text_font(it.label, cfg->font, 0);
            }
            lv_obj_remove_flag(it.label, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    c->active = cfg->selected < count ? cfg->selected : UI_SEGMENTED_NONE;
    apply_state(seg);
    return seg;
}

void ui_segmented_set_active(lv_obj_t *seg, uint8_t index)
{
    SegCtx *c = ctx_of(seg);
    if(!c) {
        return;
    }
    c->active = index < c->count ? index : UI_SEGMENTED_NONE;
    apply_state(seg);
}

uint8_t ui_segmented_get_active(const lv_obj_t *seg)
{
    const SegCtx *c = ctx_of(seg);
    return c ? c->active : UI_SEGMENTED_NONE;
}

void ui_segmented_set_item_enabled(lv_obj_t *seg, uint8_t index, bool enabled)
{
    SegCtx *c = ctx_of(seg);
    if(!c || index >= c->count || c->items[index].enabled == enabled) {
        return;
    }
    SegItem &it = c->items[index];
    it.enabled  = enabled;
    if(it.btn) {
        if(enabled) {
            lv_obj_add_flag(it.btn, LV_OBJ_FLAG_CLICKABLE);
        }
        else {
            lv_obj_remove_flag(it.btn, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* 把当前选中项禁掉时要退位，否则页面停在一个点不回来的视图上。
     * 退到第一个还能点的项，并照常发一次事件让调用方换视图。 */
    bool moved = false;
    if(!enabled && c->active == index) {
        c->active = UI_SEGMENTED_NONE;
        for(uint8_t i = 0; i < c->count; i++) {
            if(c->items[i].enabled) {
                c->active = i;
                break;
            }
        }
        moved = true;
    }
    apply_state(seg);
    if(moved) {
        lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, nullptr);
    }
}

bool ui_segmented_item_enabled(const lv_obj_t *seg, uint8_t index)
{
    const SegCtx *c = ctx_of(seg);
    return c && index < c->count && c->items[index].enabled;
}
