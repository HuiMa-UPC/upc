#include "ui_slide_unlock.hpp"
#include "ui_theme.hpp"
#include "slide_icons.h"

#include <cstdlib>
#include <cstring>

/* 可视状态（对齐 Figma「滑动控件示意」的 4 帧）：
 *   REST     = Default / pressing（红条收拢成左侧圆头）
 *   SLIDING  = sliding（红条随拖动向右生长）
 *   END      = sliding end（红条几乎占满，叠红色氛围）
 * 交互：滑到底不自动触发，需在末端「松手」才解锁；中途松手回弹。
 *
 * **两行提示文字不归状态机管**：它们是随拖动量连续叠化的，见 apply_progress()。
 * 早先是一个标签在三态之间换文案（拖动中整行藏起来），那样做不出叠化——
 * 同一个标签没法和自己交叉淡入淡出。 */
enum { VST_REST = 0, VST_SLIDING, VST_END };

struct ui_slide_unlock_t {
    lv_obj_t             *root;      /**< 玻璃轨道（= 对外根节点） */
    lv_obj_t             *track;
    lv_obj_t             *fill;      /**< 红色可伸长胶囊（左端固定，右端随拖动生长） */
    lv_obj_t             *head;      /**< 前沿圆头容器（固定 knob_sz，贴 fill 右端） */
    lv_obj_t             *tint;      /**< 末端红色氛围叠层 */
    lv_obj_t             *hint_row;  /**< 静止态提示行（文字 + 箭头），置于最上层 */
    lv_obj_t             *hint;      /**< 静止态提示文字（"SLIDE TO UNLOCK"） */
    lv_obj_t             *release;   /**< 到底态提示文字（"RELEASE TO OPEN"），与上者叠化 */
    lv_obj_t             *arrows;    /**< 3 个 chevron 箭头容器 */
    lv_point_precise_t    chev_pts[3][3]; /**< chevron 折线点（lv_line 只存指针，须持久） */
    int32_t               knob_sz;
    bool                  light;     /**< 浅色档（首页待机态），见 cfg->light_theme */
    /** 静止态提示行相对轨道中心的右偏量，随拖动归零，见 apply_progress()。
     *  只有浅色档非零：稿上那行字是居中在**旋钮右边那段白区**里的（实测文字中心
     *  1048.5，而轨道中心 1005.5），深色那四条滑块没有同口径的稿，维持整条轨道居中。 */
    int32_t               hint_off;
    int32_t               min_w;     /**< 红条最小宽（收拢成圆头） */
    int32_t               max_w;     /**< 红条最大宽 */
    int32_t               threshold_w;   /**< >= 此宽视为到底，可松手解锁 */
    lv_color_t            accent;
    lv_grad_dsc_t         fill_grad;     /**< 红条对角(45°)线性渐变（须持久存储） */
    lv_grad_dsc_t         gloss_grad;    /**< 圆头玻璃高光径向渐变（须持久存储） */
    const char           *rest_text;     /**< 静止态提示文字（指向调用方字面量） */
    int                   vstate;
    bool                  unlocked;
    bool                  enabled;
    bool                  dragging;
    bool                  at_end;
    int32_t               press_origin_x;
    int32_t               fill_origin_w;
    ui_slide_unlock_cb_t  on_unlocked;
    void                 *user_data;
};

static void apply_state(ui_slide_unlock_t *self, int st)
{
    if(self->vstate == st) {
        return;
    }
    self->vstate = st;

    /* 这里只管末端的红色氛围层；两行文字由 apply_progress() 连续驱动。 */
    switch(st) {
    case VST_REST:
    case VST_SLIDING:
        self->at_end = false;
        if(self->tint) lv_obj_add_flag(self->tint, LV_OBJ_FLAG_HIDDEN);
        break;

    case VST_END:
        self->at_end = true;
        if(self->tint) lv_obj_remove_flag(self->tint, LV_OBJ_FLAG_HIDDEN);
        break;

    default:
        break;
    }
}

/**
 * 按当前拖动量叠化两行提示：
 *   "SLIDE TO UNLOCK"  淡出，同时从白区中心左移到轨道中心
 *   "RELEASE TO OPEN"  淡入（位置不动，一直是整条轨道居中）
 *
 * 进度的分母取 threshold_w 而不是 max_w：到达阈值就是「可松手解锁」那一刻，
 * 那时 RELEASE TO OPEN 必须已经完全显出来，再往后拖不该还有变化。
 *
 * **两条斜坡错开、各占一半行程**，不是同步对冲：两行字最终都落在轨道中心、又几乎
 * 一样长，同步淡入淡出的话中段会是两行半透明的字叠在一起，糊成一片。错开之后任一
 * 时刻至多一行可见，前半程「SLIDE TO UNLOCK 边淡出边归位」，后半程「RELEASE TO
 * OPEN 浮现」。
 *
 * 用 text_opa / line_opa 而不是对象级的 lv_obj_set_style_opa()：后者一旦小于 255，
 * LVGL 会把对象先渲染到独立图层再合成，而这函数是拖动期间每帧都要跑的。
 */
static void apply_progress(ui_slide_unlock_t *self, int32_t w)
{
    const int32_t span = self->threshold_w - self->min_w;
    int32_t t = (span > 0) ? ((w - self->min_w) * 256 / span) : 256;   /* 0..256 定点 */
    if(t < 0)   t = 0;
    if(t > 256) t = 256;

    /* 前半程（t 0..128）淡出并归位，后半程（128..256）另一行浮现 */
    int32_t out_left = 256 - t * 2;          /* 静止行的剩余量：256 → 0 */
    if(out_left < 0) out_left = 0;
    int32_t in_amt = t * 2 - 256;            /* 到底行的显现量：0 → 256 */
    if(in_amt < 0) in_amt = 0;

    if(self->hint) {
        lv_obj_set_style_text_opa(self->hint,
            static_cast<lv_opa_t>(LV_OPA_70 * out_left / 256), 0);
    }
    if(self->arrows) {
        /* chevron 是三条 lv_line，静止态 line_opa 50%，跟着文字一起淡出 */
        const uint32_t n = lv_obj_get_child_count(self->arrows);
        for(uint32_t i = 0; i < n; ++i) {
            lv_obj_set_style_line_opa(lv_obj_get_child(self->arrows, static_cast<int32_t>(i)),
                static_cast<lv_opa_t>(LV_OPA_50 * out_left / 256), 0);
        }
    }
    if(self->hint_row) {
        /* 淡出的同时左移，两者同一条斜坡：字消失那一刻正好落在轨道中心 */
        lv_obj_set_style_translate_x(self->hint_row, self->hint_off * out_left / 256, 0);
    }
    if(self->release) {
        lv_obj_set_style_text_opa(self->release,
            static_cast<lv_opa_t>(LV_OPA_COVER * in_amt / 256), 0);
    }
}

/* 设定红条宽度（左端固定），圆头随之贴右端，并按宽度切换可视状态。 */
static void set_fill_w(ui_slide_unlock_t *self, int32_t w)
{
    if(w < self->min_w) w = self->min_w;
    if(w > self->max_w) w = self->max_w;
    lv_obj_set_width(self->fill, w);
    if(self->head) {
        lv_obj_align(self->head, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    int st;
    if(w <= self->min_w + 1) {
        st = VST_REST;
    } else if(w >= self->threshold_w) {
        st = VST_END;
    } else {
        st = VST_SLIDING;
    }
    apply_state(self, st);
    /* 放在状态切换之后：回弹动画也走 set_fill_w，叠化因此自动倒放，不必另写。 */
    apply_progress(self, w);
}

static void anim_fill_w(void *obj, int32_t v)
{
    set_fill_w(static_cast<ui_slide_unlock_t *>(obj), v);
}

static void return_fill(ui_slide_unlock_t *self)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, self);
    lv_anim_set_exec_cb(&a, anim_fill_w);
    lv_anim_set_values(&a, lv_obj_get_width(self->fill), self->min_w);
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
    if(!self || self->unlocked || !self->enabled) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if(!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_anim_delete(self, anim_fill_w);   /* 打断可能在进行的回弹动画 */
    self->dragging = true;
    self->press_origin_x = p.x;
    self->fill_origin_w  = lv_obj_get_width(self->fill);
}

static void on_pressing(lv_event_t *e)
{
    auto *self = static_cast<ui_slide_unlock_t *>(lv_event_get_user_data(e));
    if(!self || self->unlocked || !self->enabled || !self->dragging) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if(!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const int32_t dx = p.x - self->press_origin_x;
    set_fill_w(self, self->fill_origin_w + dx);
    /* 注意：到底不自动触发，等待松手（见 on_release）。 */
}

static void on_release(lv_event_t *e)
{
    auto *self = static_cast<ui_slide_unlock_t *>(lv_event_get_user_data(e));
    if(!self || self->unlocked || !self->enabled || !self->dragging) {
        return;
    }
    self->dragging = false;

    if(lv_obj_get_width(self->fill) >= self->threshold_w) {
        /* 滑到底后松手 -> 解锁 */
        self->unlocked = true;
        set_fill_w(self, self->max_w);
        lv_obj_remove_flag(self->fill, LV_OBJ_FLAG_CLICKABLE);
        if(self->on_unlocked) {
            self->on_unlocked(self->user_data);
        }
    } else {
        /* 未到底松手 -> 回弹 */
        return_fill(self);
    }
}

/* 生成 3 个 chevron 箭头（">"）到 arrows 容器；点集存于 self 以保证生命周期。 */
static void build_chevrons(ui_slide_unlock_t *self, int32_t track_h)
{
    int32_t cw = track_h * 7 / 96;   if(cw < 4) cw = 4;
    int32_t ch = track_h * 12 / 96;  if(ch < 8) ch = 8;
    int32_t lw = track_h * 3 / 96;   if(lw < 2) lw = 2;
    const int32_t gap = cw / 2 > 2 ? cw / 2 : 2;

    lv_obj_t *arrows = lv_obj_create(self->hint_row);
    lv_obj_remove_style_all(arrows);
    lv_obj_set_size(arrows, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(arrows, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arrows, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(arrows, gap, 0);
    lv_obj_remove_flag(arrows, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arrows, LV_OBJ_FLAG_SCROLLABLE);
    self->arrows = arrows;

    for(int i = 0; i < 3; ++i) {
        self->chev_pts[i][0].x = 0;    self->chev_pts[i][0].y = 0;
        self->chev_pts[i][1].x = cw;   self->chev_pts[i][1].y = ch / 2;
        self->chev_pts[i][2].x = 0;    self->chev_pts[i][2].y = ch;

        lv_obj_t *ln = lv_line_create(arrows);
        lv_line_set_points(ln, self->chev_pts[i], 3);
        lv_obj_set_style_line_width(ln, lw, 0);
        lv_obj_set_style_line_color(ln, self->light ? user_light_text : lv_color_white(), 0);
        lv_obj_set_style_line_opa(ln, LV_OPA_50, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
    }
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
    self->accent = cfg->custom_accent ? cfg->accent_color : user_color_accent;
    self->light = cfg->light_theme;
    self->rest_text = cfg->hint_text ? cfg->hint_text : "SLIDE TO UNLOCK";
    self->vstate = -1;
    self->enabled = true;

    const int32_t track_w = cfg->track_w > 0 ? cfg->track_w : USER_SLIDE_W;
    const int32_t track_h = cfg->track_h > 0 ? cfg->track_h : USER_SLIDE_H;
    /* 圆头直径：调用方显式给值优先（同时也就定下了内缩量）；否则沿用老规则——
     * 按 track_h 与 USER_SLIDE_H/USER_SLIDE_KNOB_SZ 的差推导。 */
    const int32_t knob_sz = (cfg->knob_sz > 0)
        ? cfg->knob_sz
        : ((cfg->track_h > 0) ? (track_h - (USER_SLIDE_H - USER_SLIDE_KNOB_SZ))
                              : USER_SLIDE_KNOB_SZ);
    self->knob_sz = knob_sz;

    const int32_t inset = (track_h - knob_sz) / 2;
    self->min_w = knob_sz;
    self->max_w = track_w - 2 * inset;
    self->threshold_w = self->min_w + (self->max_w - self->min_w) * 9 / 10;

    /* 静止态提示行的右偏量：让它居中在**旋钮右边那段白区**里，即 [inset+knob_sz, track_w]
     * 的中心，减去轨道中心 track_w/2，化简得 (inset + knob_sz) / 2。
     * 三档算下来 66.5 / 44.5 / 35.5，1280 档与稿实测的 44.5（文字心 1050 − 轨心 1005.5）
     * 分毫不差。深色那四条滑块没有同口径的稿，给 0 维持整条轨道居中。 */
    self->hint_off = self->light ? (inset + knob_sz) / 2 : 0;

    /* ---- 玻璃胶囊轨道 ---- */
    /* 半透明黑底让背后内容（首页设备照片 / 弹窗卡片）透出，营造通透玻璃感；
     * 细白描边勾勒边缘高光。LVGL 无原生背景模糊，通透感靠低不透明度近似。 */
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, track_w, track_h);
    /* 浅色档的轨道与滑块都是圆角矩形（稿 28 / 24），暗色档仍是全圆胶囊。
     * 轨道那 28 落不回 USER_LIGHT_RADIUS(24) 的梯度，按 7/6 折——三档算下来
     * 42 / 28 / 22，与稿的 ×1.5 / ×1 / ×0.8 都对得上。 */
    const int32_t track_r = self->light ? USER_LIGHT_RADIUS * 7 / 6 : LV_RADIUS_CIRCLE;
    const int32_t knob_r  = self->light ? USER_LIGHT_RADIUS : LV_RADIUS_CIRCLE;
    lv_obj_set_style_radius(track, track_r, 0);
    if(!cfg->transparent_track) {
        if(self->light) {
            lv_obj_set_style_bg_color(track, user_light_glass, 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_90, 0);
            lv_obj_set_style_border_width(track, 1, 0);
            lv_obj_set_style_border_color(track, lv_color_white(), 0);
            lv_obj_set_style_border_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(track, 6, 0);
            lv_obj_set_style_shadow_ofs_y(track, 2, 0);
            lv_obj_set_style_shadow_color(track, lv_color_hex(0x283184), 0);
            lv_obj_set_style_shadow_opa(track, 26, 0);
        } else {
            lv_obj_set_style_bg_color(track, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_50, 0);
            lv_obj_set_style_border_width(track, 1, 0);
            lv_obj_set_style_border_color(track, lv_color_white(), 0);
            lv_obj_set_style_border_opa(track, LV_OPA_10, 0);
        }
    }
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    self->root = track;
    self->track = track;

    /* ---- 末端红色氛围叠层（默认隐藏） ---- */
    lv_obj_t *tint = lv_obj_create(track);
    lv_obj_remove_style_all(tint);
    lv_obj_set_size(tint, track_w, track_h);
    lv_obj_set_pos(tint, 0, 0);
    lv_obj_set_style_radius(tint, track_r, 0);
    lv_obj_set_style_bg_color(tint, self->accent, 0);
    lv_obj_set_style_bg_opa(tint, LV_OPA_10, 0);
    lv_obj_add_flag(tint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(tint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tint, LV_OBJ_FLAG_SCROLLABLE);
    self->tint = tint;

    /* ---- 可伸长滑块（左端固定，宽度随拖动生长） ---- */
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, self->min_w, knob_sz);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, inset, 0);
    if(self->light) {
        /* 浅色档是稿上那块深色 #333145 实底，没有渐变也没有发光描边——它压在白轨道上，
         * 对比度本来就够，再加高光反而脏。 */
        lv_obj_set_style_bg_color(fill, user_light_nav_on, 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(fill, 10, 0);
        lv_obj_set_style_shadow_ofs_y(fill, 5, 0);
        lv_obj_set_style_shadow_color(fill, lv_color_hex(0xA1A1A1), 0);
        lv_obj_set_style_shadow_opa(fill, LV_OPA_20, 0);
    } else {
        /* 鲜艳红色对角(45°)渐变（Figma #FF2424 → #FF4B4B）：左上→右下，收拢成圆头时即 45°。 */
        const lv_color_t fill_cols[2] = { lv_color_hex(0xD00A0A), lv_color_hex(0xFF8484) };
        const lv_opa_t   fill_opa[2]  = { LV_OPA_COVER, LV_OPA_COVER };
        const uint8_t    fill_frac[2] = { 0, 255 };
        lv_grad_init_stops(&self->fill_grad, fill_cols, fill_opa, fill_frac, 2);
        lv_grad_linear_init(&self->fill_grad, LV_GRAD_LEFT, LV_GRAD_TOP,
                            LV_GRAD_RIGHT, LV_GRAD_BOTTOM, LV_GRAD_EXTEND_PAD);
        lv_obj_set_style_bg_color(fill, lv_color_hex(0xD00A0A), 0);
        lv_obj_set_style_bg_grad(fill, &self->fill_grad, 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        /* 1px 带红调的浅描边（避免生硬白边）+ 偏黑柔和投影（沉下去，不发光）。 */
        lv_obj_set_style_border_width(fill, 1, 0);
        lv_obj_set_style_border_color(fill, lv_color_mix(lv_color_white(), self->accent, 128), 0);
        lv_obj_set_style_border_opa(fill, LV_OPA_20, 0);
        lv_obj_set_style_shadow_width(fill, 16, 0);
        lv_obj_set_style_shadow_ofs_y(fill, 3, 0);
        lv_obj_set_style_shadow_color(fill, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(fill, LV_OPA_30, 0);
    }
    lv_obj_set_style_radius(fill, knob_r, 0);
    lv_obj_add_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    self->fill = fill;

    /* ---- 前沿圆头（固定尺寸，贴 fill 右端；承载图标与高光） ---- */
    lv_obj_t *head = lv_obj_create(fill);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, knob_sz, knob_sz);
    lv_obj_align(head, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    self->head = head;

    /* 左上径向高光（玻璃反光，不吃点击）。gloss_grad 须活到销毁，故存于 self。
     * 浅色档不画：稿上那块深色滑块是哑光的，加了高光反而像另一种材质。 */
    lv_obj_t *gloss = self->light ? nullptr : lv_obj_create(head);
    if(gloss) {
        lv_obj_remove_style_all(gloss);
        lv_obj_set_size(gloss, knob_sz, knob_sz);
        lv_obj_center(gloss);
        lv_obj_remove_flag(gloss, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(gloss, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(gloss, LV_RADIUS_CIRCLE, 0);
        /* 弱化并收拢的左上柔和反光：低透明度 + 半径只到约 60% 处即淡出，
         * 避免满面油光的塑料感。 */
        const lv_color_t gloss_cols[2] = { lv_color_white(), lv_color_white() };
        const lv_opa_t   gloss_opa[2]  = { LV_OPA_20, LV_OPA_TRANSP };
        const uint8_t    gloss_frac[2] = { 0, 255 };
        lv_grad_init_stops(&self->gloss_grad, gloss_cols, gloss_opa, gloss_frac, 2);
        lv_grad_radial_init(&self->gloss_grad, LV_GRAD_LEFT, LV_GRAD_TOP,
                            lv_pct(60), lv_pct(55), LV_GRAD_EXTEND_PAD);
        lv_obj_set_style_bg_grad(gloss, &self->gloss_grad, 0);
        lv_obj_set_style_bg_opa(gloss, LV_OPA_COVER, 0);
    }

    /* 圆头中心图标（A8 掩膜 recolor 白色）。深色档是那枚"开门"图标、占约 55% 直径；
     * 浅色档稿上换成了右箭头，且只占 32/84 ≈ 38%（三档算下来 48 / 32 / 25，与
     * gen_home_metric_icons.py 的 ICON_SIZES["arrow_right"] 一致，1:1 不缩放）。 */
    const lv_image_dsc_t *knob_src = cfg->knob_icon ? cfg->knob_icon : &ic_door_open;
    const int32_t knob_src_sz = (int32_t)knob_src->header.w;
    lv_obj_t *icon = lv_image_create(head);
    lv_image_set_src(icon, knob_src);
    lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    const int32_t icon_target = self->light ? (knob_sz * 8 / 21)    /* 稿 32/84 */
                                            : (knob_sz * 11 / 20);  /* ~0.55 × 直径 */
    lv_image_set_pivot(icon, knob_src_sz / 2, knob_src_sz / 2);
    lv_image_set_scale(icon, (uint32_t)(256 * icon_target / knob_src_sz));
    lv_obj_center(icon);

    /* ---- 提示行（文字 + 箭头），置于最上层，末端可覆盖在红条上 ---- */
    lv_obj_t *hint_row = lv_obj_create(track);
    lv_obj_remove_style_all(hint_row);
    lv_obj_set_size(hint_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hint_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint_row, track_h * 10 / 96, 0);
    lv_obj_remove_flag(hint_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);
    self->hint_row = hint_row;

    lv_obj_t *hint = lv_label_create(hint_row);
    lv_label_set_text(hint, self->rest_text);
    lv_obj_set_style_text_color(hint, self->light ? user_light_text : lv_color_white(), 0);
    lv_obj_set_style_text_font(hint, self->light ? user_font_body() : user_font_text(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    if(self->light) {
        lv_obj_set_style_text_letter_space(hint, 2, 0);   /* 稿 tracking 1.92 */
    }
    self->hint = hint;

    /* 三个 chevron 只有深色档有：浅色稿那条滑块上只有一行字，没有箭头。 */
    if(!self->light) {
        build_chevrons(self, track_h);
    }

    /* 对齐基准是整条轨道居中；浅色档的右偏由 apply_progress() 用 translate_x 给
     * （静止态偏满 hint_off、拖到底归零），不动这里的 align。 */
    lv_obj_center(hint_row);

    /* ---- 到底态提示「RELEASE TO OPEN」：独立一层，与上面那行叠化 ----
     * 位置固定在整条轨道居中（拖动时不移），只改不透明度。
     * 到底那帧它压在红条上（浅色档滑块也是深色），所以两档都取白。 */
    lv_obj_t *release = lv_label_create(track);
    lv_label_set_text(release, "RELEASE TO OPEN");
    lv_obj_set_style_text_color(release, lv_color_white(), 0);
    lv_obj_set_style_text_font(release, self->light ? user_font_body() : user_font_text(), 0);
    lv_obj_set_style_text_opa(release, LV_OPA_TRANSP, 0);
    if(self->light) {
        lv_obj_set_style_text_letter_space(release, 2, 0);   /* 与静止态那行同 tracking */
    }
    lv_obj_remove_flag(release, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(release);
    self->release = release;

    /* 拖动事件挂在红条上 */
    lv_obj_add_event_cb(fill, on_press, LV_EVENT_PRESSED, self);
    lv_obj_add_event_cb(fill, on_pressing, LV_EVENT_PRESSING, self);
    lv_obj_add_event_cb(fill, on_release, LV_EVENT_RELEASED, self);
    lv_obj_add_event_cb(fill, on_release, LV_EVENT_PRESS_LOST, self);

    lv_obj_add_event_cb(track, slide_unlock_delete_cb, LV_EVENT_DELETE, self);

    /* 初始进入 REST 态 */
    set_fill_w(self, self->min_w);

    return self;
}

lv_obj_t *ui_slide_unlock_get_root(ui_slide_unlock_t *self)
{
    return self ? self->root : nullptr;
}

void ui_slide_unlock_set_hint(ui_slide_unlock_t *self, const char *text)
{
    if(!self || !text) {
        return;
    }
    self->rest_text = text;
    /* 静止态那行现在是独立标签、不再被状态机换文案，所以随时可写。 */
    if(self->hint) {
        lv_label_set_text(self->hint, text);
    }
}

void ui_slide_unlock_reset(ui_slide_unlock_t *self)
{
    if(!self) {
        return;
    }
    self->unlocked = false;
    self->enabled = true;
    self->dragging = false;
    self->at_end = false;
    if(self->fill) {
        lv_obj_add_flag(self->fill, LV_OBJ_FLAG_CLICKABLE);
    }
    self->vstate = -1;         /* 强制重新应用可视状态 */
    return_fill(self);
}

void ui_slide_unlock_set_enabled(ui_slide_unlock_t *self, bool enabled)
{
    if(!self) {
        return;
    }
    self->enabled = enabled;
    if(self->fill) {
        if(enabled && !self->unlocked) {
            lv_obj_add_flag(self->fill, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_flag(self->fill, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if(enabled) {
        lv_obj_set_style_opa(self->track, LV_OPA_COVER, 0);
        return;
    }
    self->dragging = false;
    self->at_end = false;
    lv_anim_delete(self, anim_fill_w);
    set_fill_w(self, self->min_w);
    lv_obj_set_style_opa(self->track, LV_OPA_70, 0);
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
