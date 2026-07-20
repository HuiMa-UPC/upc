#include "page_user_timelapse.hpp"
#include "video_player.hpp"
#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"

#include <cstdio>
#include <cstring>

/**
 * @file page_user_timelapse.cpp
 * @brief 延时摄影全屏播放器（对齐设计稿 3033/3268/3270）。
 *
 * 布局（FLOATING 叠加）：
 *   ┌──────────────────────────────────────────────┐
 *   │ [Timelapse / 时间]                  [⬇] [✕]  │  顶部
 *   │                                                │
 *   │                  (视频 cover)                  │
 *   │                    ( ▶ / ‖ )                   │  中央播放钮
 *   │                                                │
 *   │ 00:23 / 10:00  [========------------]          │  底部进度
 *   └──────────────────────────────────────────────┘
 *   点画面切换控件显隐；竖构图视频在横屏 cover（与预览一致）。
 */

namespace {

char g_path[256]  = {0};
char g_title[96]  = {0};
char g_sub[64]    = {0};

lv_obj_t *g_page      = nullptr;
lv_obj_t *g_video_img = nullptr;
lv_obj_t *g_overlay   = nullptr;   /**< 控件层（非 clickable，空白处点击穿透到视频） */
lv_obj_t *g_play_icon = nullptr;
lv_obj_t *g_slider    = nullptr;
lv_obj_t *g_time_lbl  = nullptr;
lv_obj_t *g_title_lbl = nullptr;
lv_obj_t *g_sub_lbl   = nullptr;

lv_obj_t *g_preview_box  = nullptr;  /**< 拖动预览浮层 */
lv_obj_t *g_preview_img  = nullptr;
lv_obj_t *g_preview_time = nullptr;

bool g_controls_visible = true;
bool g_dragging         = false;

/** 秒 → "M:SS" 或 "H:MM:SS" */
void fmt_time(double sec, char *buf, size_t n)
{
    if(sec < 0) sec = 0;
    const uint32_t t = static_cast<uint32_t>(sec + 0.5);
    const uint32_t h = t / 3600u, m = (t % 3600u) / 60u, s = t % 60u;
    if(h > 0) {
        std::snprintf(buf, n, "%u:%02u:%02u", h, m, s);
    } else {
        std::snprintf(buf, n, "%u:%02u", m, s);
    }
}

void set_play_icon(bool playing)
{
    if(g_play_icon) {
        lv_label_set_text(g_play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

void set_controls_visible(bool vis)
{
    g_controls_visible = vis;
    if(g_overlay) {
        if(vis) {
            lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- 事件 ---- */
void on_video_click(lv_event_t *)
{
    set_controls_visible(!g_controls_visible);
}

void on_play_click(lv_event_t *)
{
    ui::VideoPlayer::instance().toggle();
    set_play_icon(ui::VideoPlayer::instance().is_playing());
}

void on_close_click(lv_event_t *)
{
    /* 切回历史页；本页 on_exit 会 close 播放器 */
    ui::PageManager::instance().switch_to("user_history");
}

void on_download_click(lv_event_t *)
{
    /* TODO：拷贝到 U 盘（需后端配合），当前占位 */
    ui_toast_show("Download coming soon", user_color_accent, 1500);
}

/** 把预览浮层定位到旋钮上方（overlay 全屏，坐标与屏幕一致） */
void position_preview(int32_t value)
{
    if(!g_preview_box || !g_slider || !g_page) {
        return;
    }
    lv_obj_update_layout(g_slider);
    lv_area_t a;
    lv_obj_get_coords(g_slider, &a);
    const int32_t sw = lv_area_get_width(&a);
    const int32_t knob_x = a.x1 + static_cast<int32_t>(
        static_cast<int64_t>(sw) * value / 1000);

    lv_obj_update_layout(g_preview_box);
    const int32_t bw = lv_obj_get_width(g_preview_box);
    const int32_t bh = lv_obj_get_height(g_preview_box);
    int32_t x = knob_x - bw / 2;
    const int32_t maxx = lv_obj_get_width(g_page) - bw;
    if(x < 0) x = 0;
    if(x > maxx) x = maxx;
    const int32_t y = a.y1 - bh - USER_PAD_SM;
    lv_obj_set_pos(g_preview_box, x, y);
}

/** 拖动到某 value：请求预览小图 + 更新时间 + 定位显示 */
void preview_at(int32_t value)
{
    const double dur = ui::VideoPlayer::instance().duration();
    const double sec = (dur > 0.0) ? dur * value / 1000.0 : 0.0;
    ui::VideoPlayer::instance().request_preview(sec);
    if(g_preview_time) {
        char buf[16];
        fmt_time(sec, buf, sizeof(buf));
        lv_label_set_text(g_preview_time, buf);
    }
    position_preview(value);
    if(g_preview_box) {
        lv_obj_remove_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_preview_box);
    }
}

void on_slider_pressed(lv_event_t *)
{
    g_dragging = true;
    if(g_slider) {
        preview_at(lv_slider_get_value(g_slider));
    }
}

void on_slider_value_changed(lv_event_t *)
{
    if(g_dragging && g_slider) {
        preview_at(lv_slider_get_value(g_slider));
    }
}

void on_slider_released(lv_event_t *)
{
    if(g_preview_box) {
        lv_obj_add_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN);
    }
    if(!g_slider) {
        g_dragging = false;
        return;
    }
    const double dur = ui::VideoPlayer::instance().duration();
    if(dur > 0.0) {
        const int32_t v = lv_slider_get_value(g_slider);
        ui::VideoPlayer::instance().seek(dur * v / 1000.0);
    }
    g_dragging = false;
}

/** 预览小图解码完成回调（主线程） */
void on_preview(const lv_image_dsc_t *dsc, double /*sec*/)
{
    if(g_preview_img && dsc) {
        lv_image_set_src(g_preview_img, dsc);
        lv_obj_invalidate(g_preview_img);
    }
}

/** 播放器进度回调（主线程 lv_timer 内） */
void on_progress(double pos, double dur)
{
    if(g_slider && !g_dragging) {
        const int32_t v = (dur > 0.0)
            ? static_cast<int32_t>(pos / dur * 1000.0 + 0.5) : 0;
        lv_slider_set_value(g_slider, v, LV_ANIM_OFF);
    }
    if(g_time_lbl) {
        char cur[16], tot[16], line[40];
        fmt_time(pos, cur, sizeof(cur));
        fmt_time(dur, tot, sizeof(tot));
        std::snprintf(line, sizeof(line), "%s / %s", cur, tot);
        lv_label_set_text(g_time_lbl, line);
    }
    set_play_icon(ui::VideoPlayer::instance().is_playing());
}

void on_ended()
{
    set_play_icon(false);
    set_controls_visible(true);
}

/* ---- 构建 ---- */
lv_obj_t *make_round_btn(lv_obj_t *parent, const char *sym, int32_t sz,
                         lv_event_cb_t cb, lv_obj_t **out_icon)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
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

} /* namespace */

void page_user_timelapse_set_source(const char *video_abs_path,
                                    const char *title,
                                    const char *subtitle)
{
    std::snprintf(g_path, sizeof(g_path), "%s", video_abs_path ? video_abs_path : "");
    std::snprintf(g_title, sizeof(g_title), "%s", title ? title : "Timelapse");
    std::snprintf(g_sub, sizeof(g_sub), "%s", subtitle ? subtitle : "");
}

void page_user_timelapse_refresh(bool /*force*/)
{
    /* 每次进入：更新标题并开始播放待播视频 */
    if(g_page == nullptr) {
        return;
    }
    set_controls_visible(true);
    g_dragging = false;

    if(g_title_lbl) {
        lv_label_set_text(g_title_lbl, g_title[0] ? g_title : "Timelapse");
    }
    if(g_sub_lbl) {
        lv_label_set_text(g_sub_lbl, g_sub);
    }

    if(g_preview_box) {
        lv_obj_add_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN);
    }

    ui::VideoPlayer& vp = ui::VideoPlayer::instance();
    vp.set_progress_cb(on_progress);
    vp.set_ended_cb(on_ended);
    vp.set_preview_cb(on_preview);
    if(g_path[0] && g_video_img) {
        if(vp.open(g_path, g_video_img)) {
            set_play_icon(true);
        }
    }
}

void page_user_timelapse_exit(void)
{
    ui::VideoPlayer::instance().close();
}

lv_obj_t *page_user_timelapse_create(lv_obj_t *parent)
{
    /* 建到 LVGL 顶层图层：覆盖整屏（含左侧导航栏），而非仅内容区。
     * SimplePage 生命周期只按 root 的 HIDDEN 标志显隐，与挂载层无关。 */
    (void)parent;
    lv_obj_t *page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page = page;

    /* 视频画面：铺满、cover 裁切、可点击（切换控件显隐） */
    lv_obj_t *img = lv_image_create(page);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, on_video_click, LV_EVENT_CLICKED, nullptr);
    g_video_img = img;

    /* 控件层：非 clickable，空白处点击穿透到视频 */
    lv_obj_t *ov = lv_obj_create(page);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    g_overlay = ov;

    /* 顶部左：标题 + 时间 */
    lv_obj_t *title_box = lv_obj_create(ov);
    lv_obj_remove_style_all(title_box);
    lv_obj_set_size(title_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(title_box, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(title_box, 2, 0);
    lv_obj_set_style_bg_color(title_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_50, 0);
    lv_obj_set_style_radius(title_box, USER_RADIUS / 2, 0);
    lv_obj_add_flag(title_box, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(title_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(title_box, LV_ALIGN_TOP_LEFT, USER_PAD, USER_PAD);

    lv_obj_t *tl = lv_label_create(title_box);
    lv_label_set_text(tl, g_title[0] ? g_title : "Timelapse");
    lv_obj_set_style_text_color(tl, user_color_text, 0);
    lv_obj_set_style_text_font(tl, user_font_text(), 0);
    g_title_lbl = tl;

    lv_obj_t *sub = lv_label_create(title_box);
    lv_label_set_text(sub, g_sub);
    lv_obj_set_style_text_color(sub, user_color_text_dim, 0);
    lv_obj_set_style_text_font(sub, user_font_caption(), 0);
    g_sub_lbl = sub;

    /* 顶部右：下载(占位) + 关闭 */
    const int32_t btn_sz = USER_BACK_BTN_H;
    lv_obj_t *close = make_round_btn(ov, LV_SYMBOL_CLOSE, btn_sz, on_close_click, nullptr);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);
    lv_obj_t *dl = make_round_btn(ov, LV_SYMBOL_DOWNLOAD, btn_sz, on_download_click, nullptr);
    lv_obj_align(dl, LV_ALIGN_TOP_RIGHT, -(USER_PAD * 2 + btn_sz), USER_PAD);

    /* 中央播放/暂停 */
    const int32_t play_sz = USER_BACK_BTN_H * 2;
    lv_obj_t *play = make_round_btn(ov, LV_SYMBOL_PAUSE, play_sz, on_play_click, &g_play_icon);
    lv_obj_set_style_text_font(g_play_icon, user_font_value(), 0);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 0);

    /* 底部：时间文本 + 进度条 */
    lv_obj_t *bottom = lv_obj_create(ov);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_height(bottom, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bottom, USER_PAD, 0);
    lv_obj_set_style_pad_column(bottom, USER_PAD, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(bottom, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -USER_PAD);

    lv_obj_t *time_lbl = lv_label_create(bottom);
    lv_label_set_text(time_lbl, "0:00 / 0:00");
    lv_obj_set_style_text_color(time_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(time_lbl, user_font_caption(), 0);
    g_time_lbl = time_lbl;

    lv_obj_t *slider = lv_slider_create(bottom);
    lv_obj_set_flex_grow(slider, 1);
    /* 触控优化：放大交互热区与 knob，提升拖动命中率与跟手性。 */
    const int32_t slider_touch_h = (USER_PROGRESS_H * 2 > 24) ? USER_PROGRESS_H * 2 : 24;
    const int32_t knob_sz = (USER_PROGRESS_H * 3 > 28) ? USER_PROGRESS_H * 3 : 28;
    lv_obj_set_height(slider, slider_touch_h);
    lv_slider_set_range(slider, 0, 1000);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, user_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, user_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, user_color_text, LV_PART_KNOB);
    lv_obj_set_style_width(slider, knob_sz, LV_PART_KNOB);
    lv_obj_set_style_height(slider, knob_sz, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_slider_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(slider, on_slider_value_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_PRESS_LOST, nullptr);
    g_slider = slider;

    /* 拖动预览浮层：小图 + 时间戳，默认隐藏，拖动时跟随旋钮 */
    const int32_t pv_w = USER_SLIDE_W * 3 / 5;
    const int32_t pv_h = pv_w * 4 / 5;
    lv_obj_t *pv = lv_obj_create(ov);
    lv_obj_remove_style_all(pv);
    lv_obj_set_size(pv, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pv, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(pv, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_row(pv, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_color(pv, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(pv, LV_OPA_70, 0);
    lv_obj_set_style_radius(pv, USER_RADIUS / 2, 0);
    lv_obj_add_flag(pv, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(pv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pv, LV_OBJ_FLAG_HIDDEN);
    g_preview_box = pv;

    lv_obj_t *pimg = lv_image_create(pv);
    lv_obj_set_size(pimg, pv_w, pv_h);
    lv_obj_set_style_bg_color(pimg, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(pimg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pimg, USER_RADIUS / 2, 0);
    lv_obj_set_style_clip_corner(pimg, true, 0);
    lv_image_set_inner_align(pimg, LV_IMAGE_ALIGN_COVER);
    g_preview_img = pimg;

    lv_obj_t *ptime = lv_label_create(pv);
    lv_label_set_text(ptime, "0:00");
    lv_obj_set_style_text_color(ptime, user_color_text, 0);
    lv_obj_set_style_text_font(ptime, user_font_caption(), 0);
    g_preview_time = ptime;

    return page;
}
