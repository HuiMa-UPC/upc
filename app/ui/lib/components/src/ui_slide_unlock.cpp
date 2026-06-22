#include "ui_slide_unlock.hpp"
#include "ui_theme.hpp"

#include <cstdlib>
#include <cstring>

struct ui_slide_unlock_t {
    lv_obj_t             *root;     /**< 整条胶囊（含背景） */
    lv_obj_t             *track;    /**< 内部轨道（用于计算 knob 移动范围） */
    lv_obj_t             *knob;     /**< 圆形 knob */
    lv_obj_t             *hint;     /**< 中部提示文字 */
    int32_t               knob_sz;
    int32_t               min_x;
    int32_t               max_x;
    int32_t               threshold;     /**< >= 此 x 视为解锁 */
    bool                  unlocked;
    bool                  dragging;
    int32_t               press_origin_x;
    int32_t               knob_origin_x;
    ui_slide_unlock_cb_t  on_unlocked;
    void                 *user_data;
};

static void knob_set_x(ui_slide_unlock_t *self, int32_t x)
{
    if(x < self->min_x) x = self->min_x;
    if(x > self->max_x) x = self->max_x;
    lv_obj_set_x(self->knob, x);

    /* hint 文字随推进变橙：knob 越靠右，文字 hue 越偏向 accent。 */
    if(self->hint) {
        const int32_t span = self->max_x - self->min_x;
        const int32_t prog = (span > 0) ? ((x - self->min_x) * 255 / span) : 0;
        const lv_opa_t opa = (lv_opa_t)(LV_OPA_30 + (LV_OPA_COVER - LV_OPA_30) * prog / 255);
        lv_obj_set_style_text_opa(self->hint, opa, 0);
    }
}

static void anim_knob_x(void *obj, int32_t v)
{
    auto *self = static_cast<ui_slide_unlock_t *>(obj);
    knob_set_x(self, v);
}

static void return_knob(ui_slide_unlock_t *self)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, self);
    lv_anim_set_exec_cb(&a, anim_knob_x);
    lv_anim_set_values(&a, lv_obj_get_x(self->knob), self->min_x);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void slide_unlock_delete_cb(lv_event_t *e)
{
    std::free(lv_event_get_user_data(e));
}

static void on_press(lv_event_t *e)
{
    auto *self = static_cast<ui_slide_unlock_t *>(lv_event_get_user_data(e));
    if(!self || self->unlocked) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if(!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    self->dragging = true;
    self->press_origin_x = p.x;
    self->knob_origin_x = lv_obj_get_x(self->knob);
}

static void on_pressing(lv_event_t *e)
{
    auto *self = static_cast<ui_slide_unlock_t *>(lv_event_get_user_data(e));
    if(!self || self->unlocked || !self->dragging) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if(!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const int32_t dx = p.x - self->press_origin_x;
    knob_set_x(self, self->knob_origin_x + dx);

    /* 拖到阈值即触发，无需等用户松手 —— 体验更"果断" */
    if(!self->unlocked && lv_obj_get_x(self->knob) >= self->threshold) {
        self->unlocked = true;
        self->dragging = false;
        knob_set_x(self, self->max_x);
        lv_obj_remove_flag(self->knob, LV_OBJ_FLAG_CLICKABLE);
        if(self->hint) {
            lv_obj_set_style_text_color(self->hint, user_color_accent, 0);
            lv_obj_set_style_text_opa(self->hint, LV_OPA_COVER, 0);
        }
        if(self->on_unlocked) {
            self->on_unlocked(self->user_data);
        }
    }
}

static void on_release(lv_event_t *e)
{
    auto *self = static_cast<ui_slide_unlock_t *>(lv_event_get_user_data(e));
    if(!self || self->unlocked || !self->dragging) {
        return;
    }
    self->dragging = false;
    return_knob(self);
}

ui_slide_unlock_t *ui_slide_unlock_create(lv_obj_t *parent,
                                          const ui_slide_unlock_cfg_t *cfg)
{
    if(!parent || !cfg) {
        return nullptr;
    }

    auto *self = static_cast<ui_slide_unlock_t *>(
        std::calloc(1, sizeof(ui_slide_unlock_t)));
    if(!self) {
        return nullptr;
    }
    self->on_unlocked = cfg->on_unlocked;
    self->user_data = cfg->user_data;

    const int32_t track_w = cfg->track_w > 0 ? cfg->track_w : USER_SLIDE_W;
    const int32_t track_h = cfg->track_h > 0 ? cfg->track_h : USER_SLIDE_H;
    const int32_t knob_sz = (cfg->track_h > 0)
        ? (track_h - (USER_SLIDE_H - USER_SLIDE_KNOB_SZ))
        : USER_SLIDE_KNOB_SZ;
    self->knob_sz = knob_sz;

    /* ---- 胶囊轨道 ---- */
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, track_w, track_h);
    lv_obj_set_style_bg_color(track, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, user_color_divider, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    self->root = track;
    self->track = track;

    /* ---- hint 文字（居中） ---- */
    lv_obj_t *hint = lv_label_create(track);
    lv_label_set_text(hint, cfg->hint_text ? cfg->hint_text : "Slide to Open  " LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(hint, user_color_text, 0);
    lv_obj_set_style_text_font(hint, user_font_text(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_30, 0);
    lv_obj_center(hint);
    self->hint = hint;

    /* ---- knob ---- */
    const int32_t inset = (track_h - knob_sz) / 2;
    self->min_x = inset;
    self->max_x = track_w - knob_sz - inset;
    self->threshold = self->min_x + (self->max_x - self->min_x) * 9 / 10;

    lv_obj_t *knob = lv_obj_create(track);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, knob_sz, knob_sz);
    lv_obj_set_style_bg_color(knob, user_color_accent, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(knob, 20, 0);
    lv_obj_set_style_shadow_color(knob, user_color_accent, 0);
    lv_obj_set_style_shadow_opa(knob, LV_OPA_40, 0);
    lv_obj_add_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(knob, self->min_x, inset);
    self->knob = knob;

    /* knob 上的方向箭头标识 */
    lv_obj_t *icon = lv_label_create(knob);
    lv_label_set_text(icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(icon, user_color_text, 0);
    lv_obj_set_style_text_font(icon, user_font_icon(), 0);
    lv_obj_center(icon);

    /* 拖动事件挂在 knob 上 */
    lv_obj_add_event_cb(knob, on_press, LV_EVENT_PRESSED, self);
    lv_obj_add_event_cb(knob, on_pressing, LV_EVENT_PRESSING, self);
    lv_obj_add_event_cb(knob, on_release, LV_EVENT_RELEASED, self);
    lv_obj_add_event_cb(knob, on_release, LV_EVENT_PRESS_LOST, self);

    lv_obj_add_event_cb(track, slide_unlock_delete_cb, LV_EVENT_DELETE, self);

    return self;
}

lv_obj_t *ui_slide_unlock_get_root(ui_slide_unlock_t *self)
{
    return self ? self->root : nullptr;
}

void ui_slide_unlock_reset(ui_slide_unlock_t *self)
{
    if(!self) {
        return;
    }
    self->unlocked = false;
    self->dragging = false;
    if(self->knob) {
        lv_obj_add_flag(self->knob, LV_OBJ_FLAG_CLICKABLE);
    }
    if(self->hint) {
        lv_obj_set_style_text_color(self->hint, user_color_text, 0);
        lv_obj_set_style_text_opa(self->hint, LV_OPA_30, 0);
    }
    return_knob(self);
}

void ui_slide_unlock_destroy(ui_slide_unlock_t *self)
{
    if(!self) {
        return;
    }
    if(self->root) {
        lv_obj_delete(self->root);
    } else {
        std::free(self);
    }
}
