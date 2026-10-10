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
 * 同一个标签没法和自己交叉淡入淡出。
 * **文字的位置不随拖动变**：静止态那行原本边淡出边左移到轨道中心，移动这部分按需求
 * 取消了，只留不透明度的叠化。 */
enum { VST_REST = 0, VST_SLIDING, VST_END };

struct ui_slide_unlock_t {
    lv_obj_t             *root;      /**< 玻璃轨道（= 对外根节点） */
    lv_obj_t             *track;
    lv_obj_t             *fill;      /**< 红色可伸长胶囊（左端固定，右端随拖动生长） */
    lv_obj_t             *head;      /**< 前沿圆头容器（固定 knob_sz，贴 fill 右端） */
    lv_obj_t             *tint;      /**< 末端红色氛围叠层 */
    lv_obj_t             *hint;      /**< 静止态提示文字（"SLIDE TO UNLOCK"），垫在滑块下面 */
    lv_obj_t             *release;   /**< 到底态提示文字（"RELEASE TO OPEN"），压在滑块上面，与上者叠化 */
    lv_obj_t             *arrows;    /**< 3 个 chevron 箭头容器 */
    lv_point_precise_t    chev_pts[3][3]; /**< chevron 折线点（lv_line 只存指针，须持久） */
    int32_t               knob_sz;
    /** 浅色稿那套**版式**（圆角矩形、提示字右偏、无 chevron）。LIGHT 与 INK 两档都是
     *  true，配色与提示字字号再由 ink 分。 */
    bool                  light;
    bool                  ink;       /**< 墨色档：渐变深轨道 + 白滑块，见 UI_SLIDE_THEME_INK */
    /** 静止态提示字的不透明度。深色档 70%；浅色档（稿 6578:41426 / 6388:48980 都是
     *  #333145 满色）与墨色档（满白）都是 100%。
     *  apply_progress() 按它往 0 淡出，所以不能写死在那里。 */
    lv_opa_t              hint_opa;
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
 * 按当前拖动量叠化两行提示（两行的位置都不动，只改不透明度）：
 *   "SLIDE TO UNLOCK"  淡出（停在旋钮右边那段白区的中心，见 hint_off）
 *   "RELEASE TO OPEN"  淡入（一直是整条轨道居中）
 *
 * 进度的分母取 threshold_w 而不是 max_w：到达阈值就是「可松手解锁」那一刻，
 * 那时 RELEASE TO OPEN 必须已经完全显出来，再往后拖不该还有变化。
 *
 * **两条斜坡错开、各占一半行程**，不是同步对冲：两行字几乎一样长、中心又只差 hint_off
 * （1280 档 44.5px），同步淡入淡出的话中段会是两行半透明的字叠在一起，糊成一片。错开
 * 之后任一时刻至多一行可见，前半程「SLIDE TO UNLOCK 淡出」，后半程「RELEASE TO OPEN
 * 浮现」。
 *
 * 用 text_opa / line_opa 而不是对象级的 lv_obj_set_style_opa()：后者一旦小于 255，
 * LVGL 会把对象先渲染到独立图层再合成，而这函数是拖动期间每帧都要跑的。
 *
 * 静止态那行原本还会边淡出边左移到轨道中心（translate_x 由 hint_off 归零），移动这部分
 * 按需求取消了。
 */
static void apply_progress(ui_slide_unlock_t *self, int32_t w)
{
    const int32_t span = self->threshold_w - self->min_w;
    int32_t t = (span > 0) ? ((w - self->min_w) * 256 / span) : 256;   /* 0..256 定点 */
    if(t < 0)   t = 0;
    if(t > 256) t = 256;

    /* 前半程（t 0..128）淡出，后半程（128..256）另一行浮现 */
    int32_t out_left = 256 - t * 2;          /* 静止行的剩余量：256 → 0 */
    if(out_left < 0) out_left = 0;
    int32_t in_amt = t * 2 - 256;            /* 到底行的显现量：0 → 256 */
    if(in_amt < 0) in_amt = 0;

    /* 禁用的 light 版式静止行压在 30%（稿 6683:34165，见 apply_enabled_look()）；
     * 这里每次都会重写 text_opa，所以那一档必须在这里落、不能只在 set_enabled 里设一次。 */
    const lv_opa_t rest_opa = (self->light && !self->enabled) ? static_cast<lv_opa_t>(LV_OPA_30)
                                                              : self->hint_opa;
    if(self->hint) {
        lv_obj_set_style_text_opa(self->hint,
            static_cast<lv_opa_t>(rest_opa * out_left / 256), 0);
    }
    if(self->arrows) {
        /* chevron 是三条 lv_line，静止态 line_opa 50%，跟着文字一起淡出 */
        const uint32_t n = lv_obj_get_child_count(self->arrows);
        for(uint32_t i = 0; i < n; ++i) {
            lv_obj_set_style_line_opa(lv_obj_get_child(self->arrows, static_cast<int32_t>(i)),
                static_cast<lv_opa_t>(LV_OPA_50 * out_left / 256), 0);
        }
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

/* 可用 / 禁用的外观。
 *
 * light 版式（浅色 / 墨色两档）照稿 6683:34165 的置灰开门滑块：轨道原样，只把滑块与静止
 * 提示字各压到 30%（稿上是那两组各自 opacity 0.3）。滑块的底色与投影一起压（建滑块时
 * 两档投影都是 LV_OPA_20）；图标是白的、叠 30% 之后仍是白，不动；提示字那一半在
 * apply_progress() 里落。都用分量级的 bg_opa / shadow_opa / text_opa 而不是对象级 opa，
 * 理由同 apply_progress()。
 *
 * 深色玻璃档没有对应的稿，维持早先整条轨道 70% 的做法。 */
static void apply_enabled_look(ui_slide_unlock_t *self)
{
    if(!self->light) {
        lv_obj_set_style_opa(self->track, self->enabled ? LV_OPA_COVER : LV_OPA_70, 0);
        return;
    }
    if(self->fill) {
        const int32_t shadow_opa = LV_OPA_20;
        lv_obj_set_style_bg_opa(self->fill, self->enabled ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_shadow_opa(self->fill,
            static_cast<lv_opa_t>(self->enabled ? shadow_opa : shadow_opa * 3 / 10), 0);
    }
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

/* 生成 3 个 chevron 箭头（">"）到 parent 下新建的 arrows 容器；点集存于 self 以保证生命周期。 */
static void build_chevrons(ui_slide_unlock_t *self, lv_obj_t *parent, int32_t track_h)
{
    int32_t cw = track_h * 7 / 96;   if(cw < 4) cw = 4;
    int32_t ch = track_h * 12 / 96;  if(ch < 8) ch = 8;
    int32_t lw = track_h * 3 / 96;   if(lw < 2) lw = 2;
    const int32_t gap = cw / 2 > 2 ? cw / 2 : 2;

    lv_obj_t *arrows = lv_obj_create(parent);
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
    self->light = (cfg->theme != UI_SLIDE_THEME_GLASS);
    self->ink   = (cfg->theme == UI_SLIDE_THEME_INK);
    self->hint_opa = self->light ? LV_OPA_COVER : LV_OPA_70;
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

    /* 提示行的右偏量：让它居中在**旋钮右边那段白区**里，即 [inset+knob_sz, track_w]
     * 的中心，减去轨道中心 track_w/2，化简得 (inset + knob_sz) / 2。
     * 三档算下来 66.5 / 44.5 / 35.5，1280 档与稿实测的 44.5（文字心 1050 − 轨心 1005.5）
     * 分毫不差。深色那四条滑块没有同口径的稿，给 0 维持整条轨道居中。
     * 文字不随拖动移动（只淡出），这个偏量从创建起就固定不变。 */
    const int32_t hint_off = self->light ? (inset + knob_sz) / 2 : 0;

    /* ---- 玻璃胶囊轨道 ---- */
    /* 半透明黑底让背后内容（首页设备照片 / 弹窗卡片）透出，营造通透玻璃感；
     * 细白描边勾勒边缘高光。LVGL 无原生背景模糊，通透感靠低不透明度近似。 */
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, track_w, track_h);
    /* 浅色档的轨道与滑块都是圆角矩形（稿 28 / 24），暗色档仍是全圆胶囊。
     * 轨道那 28 落不回 USER_LIGHT_RADIUS(24) 的梯度，按 7/6 折——三档算下来
     * 42 / 28 / 22，与稿的 ×1.5 / ×1 / ×0.8 都对得上。
     * 墨色档两个圆角都比浅色档小 4：轨道 24、旋钮 20（逐行拟合稿面：深色区拐角 22.6，
     * 加 1px 描边即外框 ≈24；旋钮 20.1）。轨道正好是 USER_LIGHT_RADIUS；旋钮按 5/6 折
     * 并四舍五入，得 30 / 20 / 16——1024 档截断会得 15，而稿 ×0.8 是 16。
     *
     * **圆角直接决定拐角处的间隙**：两段圆弧只有同心（旋钮圆角 = 轨道可见圆角 − 间隙）
     * 才处处等宽，否则拐角 45° 处是 d·√2 − (R − r)·(√2 − 1)。两档的稿都是「旋钮比同心值
     * 大 1」（浅色 27/24、墨色 23/20，间隙 4），拐角 ≈4.4、比直边略宽一点。墨色档早先
     * 照搬了浅色档的 28，可见圆角 27 配旋钮 20，拐角被挤到 ≈2.8，1024 屏上只剩 1.8。 */
    const int32_t track_r = self->ink   ? USER_LIGHT_RADIUS
                          : self->light ? USER_LIGHT_RADIUS * 7 / 6 : LV_RADIUS_CIRCLE;
    const int32_t knob_r  = self->ink   ? (USER_LIGHT_RADIUS * 5 + 3) / 6
                          : self->light ? USER_LIGHT_RADIUS : LV_RADIUS_CIRCLE;
    /* 墨色档那圈 1px 白边是深色区（well）往里缩、露出底下白卡形成的，不是 border，见下。 */
    const int32_t well_inset = (self->ink && !cfg->transparent_track) ? 1 : 0;
    lv_obj_set_style_radius(track, track_r, 0);
    if(!cfg->transparent_track) {
        if(self->ink) {
            /* 稿 paint0_linear：x 从轨道左缘到右缘，#5A5872 → #333145，两停、无 alpha。
             * 纯横向两停用 bg_grad_color + bg_grad_dir 就够，不必养一个 lv_grad_dsc_t。
             * 稿上没有投影，但**有一圈 1px 白描边**，与浅色档同：压在白卡上看不见，却把
             * 深色区每边收进 1px。逐像素量过：Cancel 占 x 24..424，深色区只有 25..423 /
             * y 136..228（398×92），旋钮 (29,140) 到深色区四边都是 4。漏了它，间隙
             * 就成了 5，1024 屏上宽出四分之一，一眼能看出来。
             *
             * 这圈白边**不用 LVGL 的 border 画**：深色区单独做成内缩 1px、圆角同心的子对象，
             * 外圈直接露出底下的白卡。LVGL 只在 border_width > 1 时才把 bg 缩进描边底下
             * （lv_draw_rect.c），1px 描边时 bg 与描边各自抗锯齿再叠加，圆角上覆盖一半的
             * 像素会漏出最多 25% 的深色——四个角外沿一圈灰点，实机上就是毛边。浅色档的 bg
             * 是 90% 白，漏出来也看不见，所以只有这里要绕开。
             * well 必须是轨道的第一个子对象，垫在 tint / 滑块 / 提示字下面。 */
            lv_obj_t *well = lv_obj_create(track);
            lv_obj_remove_style_all(well);
            lv_obj_set_size(well, track_w - 2 * well_inset, track_h - 2 * well_inset);
            lv_obj_set_pos(well, well_inset, well_inset);
            lv_obj_set_style_radius(well, track_r - well_inset, 0);
            lv_obj_set_style_bg_color(well, lv_color_hex(0x5A5872), 0);
            lv_obj_set_style_bg_grad_color(well, user_light_nav_on, 0);
            lv_obj_set_style_bg_grad_dir(well, LV_GRAD_DIR_HOR, 0);
            lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
            lv_obj_remove_flag(well, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(well, LV_OBJ_FLAG_SCROLLABLE);
        } else if(self->light) {
            /* 稿 6578:41426（首页）/ 6388:48980（放版弹窗）：#FCFCFC 实底 + 1px 白描边 +
             * 投影 0 2 6 #283184 @10%。
             * 底必须不透明：早先给的 90% 叠在 #FAFAFA 上，LVGL 混色是 >>8 截断，
             * (252×229 + 250×26) >> 8 正好落回 250，轨道与页面一模一样，只剩下沿一道影子。
             * 稿上那个 6 是 Figma 的 blur（stdDeviation 3），**不能直接当 shadow_width**：
             * LVGL 的阴影是两遍 width/2 的盒式模糊（SHADOW_ENHANCE），三角核的标准差约
             * width/4.9，照抄 6 只有稿上一半的晕、像贴边一道细线。σ 3 折成 15、下移 2，
             * 都按轨道高（稿 94）缩放并四舍五入：三档 23 / 15 / 12、3 / 2 / 2。 */
            lv_obj_set_style_bg_color(track, user_light_glass, 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(track, 1, 0);
            lv_obj_set_style_border_color(track, lv_color_white(), 0);
            lv_obj_set_style_border_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(track, (track_h * 15 + 47) / 94, 0);
            lv_obj_set_style_shadow_ofs_y(track, (track_h * 2 + 47) / 94, 0);
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

    /* LVGL 的子对象坐标与对齐都从父对象**描边内侧**起算（lv_obj_get_style_space_left()
     * = pad + border_width），下面摆子对象时要把它扣回来。透明轨道不设描边，墨色档的白边
     * 是 well 露出来的，两者读回来都是 0。 */
    const int32_t border_w = lv_obj_get_style_border_width(track, LV_PART_MAIN);

    /* ---- 末端红色氛围叠层（默认隐藏） ----
     * 只盖可见的轨道区：浅色档扣描边，墨色档扣 well 外圈——不然到底那一下，露白的那圈
     * 会被染出一道淡红边。 */
    lv_obj_t *tint = lv_obj_create(track);
    lv_obj_remove_style_all(tint);
    lv_obj_set_size(tint, track_w - 2 * (border_w + well_inset),
                          track_h - 2 * (border_w + well_inset));
    lv_obj_set_pos(tint, well_inset, well_inset);
    lv_obj_set_style_radius(tint, track_r, 0);
    lv_obj_set_style_bg_color(tint, self->accent, 0);
    lv_obj_set_style_bg_opa(tint, LV_OPA_10, 0);
    lv_obj_add_flag(tint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(tint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tint, LV_OBJ_FLAG_SCROLLABLE);
    self->tint = tint;

    /* ---- 提示行（文字 + 箭头）：位置固定，垫在滑块下面 ----
     * 拖动时只淡出、不移动（见 apply_progress()），滑块从它上面盖过去。**必须在 fill 之前
     * 创建**：字不再往轨道中心躲，旋钮会在字还没淡完时就碰到它的左缘（1280 档取件弹窗是
     * 约 1/5 行程、字还剩一半多；首页那条约 3/10 行程、还剩四成）。压在上层的话旋钮会从
     * 半透明的字底下穿过——墨色档白字压白旋钮，门图标上露出几截白色笔画；浅色档深色字
     * 会把旋钮上的白色门图标盖掉一截。 */
    lv_obj_t *hint_row = lv_obj_create(track);
    lv_obj_remove_style_all(hint_row);
    lv_obj_set_size(hint_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hint_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint_row, track_h * 10 / 96, 0);
    lv_obj_remove_flag(hint_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);

    /* 提示字的字号与字距，RELEASE TO OPEN 同用。
     * 浅色档照首页稿（1280 档大写高 18、SLIDE TO UNLOCK 宽 233）：user_font_body() + 字距 2。
     * 墨色档那两张弹窗稿的提示字小一号（大写高 15.5、SLIDE TO RETRIEVE 宽 190），而 Montserrat
     * 的大写比稿面字体宽，照搬浅色档会宽出一半——1024 屏上 233 对 152，旋钮右边那段几乎被
     * 字顶满。宽度与字高没法同时对上，这条滑条窄、宽度更显眼，按宽度取 user_font_title()、
     * 不加字距：1280 / 1024 档宽 199 / 159（稿 190 / 152），大写高 14 / 12（稿 15.5 / 12.4）。 */
    const lv_font_t *hint_font = self->ink   ? user_font_title()
                               : self->light ? user_font_body() : user_font_text();
    const int32_t    hint_ls   = (self->light && !self->ink) ? 2 : 0;   /* 浅色档：稿 tracking 1.92 */

    lv_obj_t *hint = lv_label_create(hint_row);
    lv_label_set_text(hint, self->rest_text);
    /* 墨色档字压在深色轨道上，回到白字；浅色档压在白玻璃上用深色。 */
    lv_obj_set_style_text_color(hint,
        (self->light && !self->ink) ? user_light_text : lv_color_white(), 0);
    lv_obj_set_style_text_font(hint, hint_font, 0);
    lv_obj_set_style_text_opa(hint, self->hint_opa, 0);
    if(hint_ls) {
        lv_obj_set_style_text_letter_space(hint, hint_ls, 0);
    }
    self->hint = hint;

    /* 三个 chevron 只有深色档有：浅色稿那条滑块上只有一行字，没有箭头。 */
    if(!self->light) {
        build_chevrons(self, hint_row, track_h);
    }

    lv_obj_align(hint_row, LV_ALIGN_CENTER, hint_off, 0);

    /* ---- 可伸长滑块（左端固定，宽度随拖动生长） ---- */
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, self->min_w, knob_sz);
    /* 竖直居中本来就在描边内侧做（content 高 = track_h − 2×border），落点正好是离外框
     * inset；横向偏移却会再被加上一个 border，所以这里扣掉，四边才都是 inset。
     * 稿上两档都是「外框到旋钮 5、白边内侧到旋钮 4」（1280），不扣的话左边多出 1px。
     * 墨色档没有 border（border_w = 0），偏移就是 inset 本身，well 的 1px 正好补上那个 4。
     * 拖到底的右端是 inset + max_w = track_w − inset，同样对称，max_w 不用动。 */
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, inset - border_w, 0);
    if(self->ink) {
        /* 墨色档是一块白色实底，外面一圈偏下的淡白光。拖动时整块白条往右长，把深色轨道
         * 一路盖掉。光晕按稿逐点量的衰减曲线（1280 档，离旋钮边的距离 → 混进的白）：
         *   左右  0.25px 9.7%、1.25 7.9%、2.25 6.1%、4.25 2.3%，约 8px 归零
         *   下方  0.25px 17.1%、1.25 15.9%、2.25 14.7%、3.75 12.4%（再往下就是白描边）
         *   上方  0.25px 2.9%、1.25 1.8%、2.25 1.2%
         * LVGL 的阴影是两遍盒式模糊（SHADOW_ENHANCE），边缘是半宽 = width/2 的三角核；
         * blur 16 / 下移 4 / 20% 白按这个核代进去，三个方向每个采样点都差不到 2%。
         * blur 与下移按 knob_sz/84 缩放（1024 档 12 / 3，1920 档 24 / 6），不透明度不缩。
         * 轨道开了 clip_corner，光不会漏到卡上。 */
        lv_obj_set_style_bg_color(fill, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(fill, knob_sz * 4 / 21, 0);
        lv_obj_set_style_shadow_ofs_y(fill, knob_sz / 21, 0);
        lv_obj_set_style_shadow_color(fill, lv_color_white(), 0);
        lv_obj_set_style_shadow_opa(fill, LV_OPA_20, 0);
    } else if(self->light) {
        /* 浅色档是稿上那块深色 #333145 实底，没有渐变也没有发光描边——它压在白轨道上，
         * 对比度本来就够，再加高光反而脏。
         * 投影稿值 0 5 10 #A1A1A1 @20%（stdDeviation 5），换算同轨道那段：σ 5 折成 24、
         * 下移 5，按 knob_sz/84 缩放：三档 36 / 24 / 19、8 / 5 / 4。轨道开了 clip_corner，
         * 晕出不了轨道（稿上旋钮正下方会漏出下沿一点，差别很小）。 */
        lv_obj_set_style_bg_color(fill, user_light_nav_on, 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(fill, knob_sz * 2 / 7, 0);
        lv_obj_set_style_shadow_ofs_y(fill, (knob_sz * 5 + 42) / 84, 0);
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
     * gen_home_metric_icons.py 的 ICON_SIZES["arrow_right"] 一致，1:1 不缩放）。
     * 墨色档的旋钮是白的，图标反过来染成 #333145，尺寸与浅色档同为 32/84。 */
    const lv_image_dsc_t *knob_src = cfg->knob_icon ? cfg->knob_icon : &ic_door_open;
    const int32_t knob_src_sz = (int32_t)knob_src->header.w;
    lv_obj_t *icon = lv_image_create(head);
    lv_image_set_src(icon, knob_src);
    lv_obj_set_style_image_recolor(icon, self->ink ? user_light_text : lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    const int32_t icon_target = self->light ? (knob_sz * 8 / 21)    /* 稿 32/84 */
                                            : (knob_sz * 11 / 20);  /* ~0.55 × 直径 */
    lv_image_set_pivot(icon, knob_src_sz / 2, knob_src_sz / 2);
    lv_image_set_scale(icon, (uint32_t)(256 * icon_target / knob_src_sz));
    lv_obj_center(icon);

    /* ---- 到底态提示「RELEASE TO OPEN」：独立一层，与提示行叠化 ----
     * 位置固定在整条轨道居中（拖动时不移），只改不透明度。**必须压在滑块上面**（在 fill
     * 之后创建）：到底时滑条几乎盖满整条轨道，垫在下面就看不见了。
     * 到底那帧它压在红条上（浅色档滑块也是深色），所以这两档取白；墨色档到底时整条
     * 轨道被白色滑条盖满，字得反过来取深色。 */
    lv_obj_t *release = lv_label_create(track);
    lv_label_set_text(release, "RELEASE TO OPEN");
    lv_obj_set_style_text_color(release, self->ink ? user_light_text : lv_color_white(), 0);
    lv_obj_set_style_text_font(release, hint_font, 0);
    lv_obj_set_style_text_opa(release, LV_OPA_TRANSP, 0);
    if(hint_ls) {
        lv_obj_set_style_text_letter_space(release, hint_ls, 0);   /* 与静止态那行同 tracking */
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
    apply_enabled_look(self);  /* reset 会把禁用态一并解除，外观跟着还原 */
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
    apply_enabled_look(self);
    if(enabled) {
        /* 重走一遍静止态，让提示字从禁用的 30% 回到 hint_opa */
        if(self->fill) {
            set_fill_w(self, lv_obj_get_width(self->fill));
        }
        return;
    }
    self->dragging = false;
    self->at_end = false;
    lv_anim_delete(self, anim_fill_w);
    set_fill_w(self, self->min_w);
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
