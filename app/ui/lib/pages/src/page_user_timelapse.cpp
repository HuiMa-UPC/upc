#include "page_user_timelapse.hpp"

#include "page_manager.hpp"
#include "ui_theme.hpp"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kPlayIntervalMs = 200u; /* 5fps */
constexpr const char* kFsDrive = "A:";

char g_seq_dir[256] = {0};
char g_title[96]    = {0};
char g_sub[64]      = {0};

std::vector<std::string> g_frames;
size_t g_cur_index = 0;
bool g_playing = false;
bool g_controls_visible = true;
bool g_dragging = false;

lv_obj_t* g_page = nullptr;
lv_obj_t* g_video_img = nullptr;
lv_obj_t* g_overlay = nullptr;
lv_obj_t* g_play_icon = nullptr;
lv_obj_t* g_slider = nullptr;
lv_obj_t* g_time_lbl = nullptr;
lv_obj_t* g_title_lbl = nullptr;
lv_obj_t* g_sub_lbl = nullptr;
lv_timer_t* g_play_timer = nullptr;

bool parse_png_index(const char* name, uint32_t& out)
{
    if(!name || !name[0]) {
        return false;
    }
    size_t i = 0;
    while(name[i] && std::isdigit(static_cast<unsigned char>(name[i]))) {
        ++i;
    }
    if(i == 0 || std::strcmp(name + i, ".png") != 0) {
        return false;
    }
    out = static_cast<uint32_t>(std::strtoul(name, nullptr, 10));
    return out > 0;
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
    if(!g_overlay) {
        return;
    }
    if(vis) {
        lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void update_progress_ui()
{
    const size_t total = g_frames.size();
    if(total == 0) {
        if(g_slider) {
            lv_slider_set_value(g_slider, 0, LV_ANIM_OFF);
        }
        if(g_time_lbl) {
            lv_label_set_text(g_time_lbl, "0 / 0");
        }
        return;
    }

    const size_t cur = std::min(g_cur_index + 1u, total);
    if(g_slider && total > 1) {
        const int32_t v = static_cast<int32_t>(
            (static_cast<int64_t>(g_cur_index) * 1000) / static_cast<int64_t>(total - 1));
        lv_slider_set_value(g_slider, v, LV_ANIM_OFF);
    }
    if(g_time_lbl) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%zu / %zu", cur, total);
        lv_label_set_text(g_time_lbl, buf);
    }
}

void render_current_frame()
{
    if(!g_video_img || g_frames.empty()) {
        return;
    }
    if(g_cur_index >= g_frames.size()) {
        g_cur_index = g_frames.size() - 1u;
    }
    lv_image_set_src(g_video_img, g_frames[g_cur_index].c_str());
    lv_obj_invalidate(g_video_img);
    update_progress_ui();
}

bool load_sequence_frames()
{
    g_frames.clear();
    g_cur_index = 0;
    if(g_seq_dir[0] == '\0') {
        return false;
    }

    DIR* d = opendir(g_seq_dir);
    if(!d) {
        return false;
    }

    std::vector<std::pair<uint32_t, std::string>> indexed;
    while(const dirent* ent = readdir(d)) {
        uint32_t index = 0;
        if(!parse_png_index(ent->d_name, index)) {
            continue;
        }
        indexed.emplace_back(index,
                             std::string(kFsDrive) + g_seq_dir + "/" + ent->d_name);
    }
    closedir(d);

    if(indexed.empty()) {
        return false;
    }
    std::sort(indexed.begin(), indexed.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    g_frames.reserve(indexed.size());
    for(auto& item : indexed) {
        g_frames.push_back(std::move(item.second));
    }
    return true;
}

void on_video_click(lv_event_t*)
{
    set_controls_visible(!g_controls_visible);
}

void on_play_click(lv_event_t*)
{
    if(g_frames.empty()) {
        return;
    }
    g_playing = !g_playing;
    set_play_icon(g_playing);
}

void on_close_click(lv_event_t*)
{
    ui::PageManager::instance().switch_to("user_history");
}

void on_slider_pressed(lv_event_t*)
{
    g_dragging = true;
    g_playing = false;
    set_play_icon(false);
}

void on_slider_value_changed(lv_event_t*)
{
    if(!g_dragging || !g_slider || g_frames.empty()) {
        return;
    }
    const int32_t v = lv_slider_get_value(g_slider);
    const size_t max_idx = g_frames.size() - 1u;
    g_cur_index = static_cast<size_t>(
        (static_cast<int64_t>(v) * static_cast<int64_t>(max_idx) + 500) / 1000);
    render_current_frame();
}

void on_slider_released(lv_event_t*)
{
    g_dragging = false;
}

void play_timer_cb(lv_timer_t*)
{
    if(!g_playing || g_dragging || g_frames.empty()) {
        return;
    }
    if(g_cur_index + 1u >= g_frames.size()) {
        g_playing = false;
        set_play_icon(false);
        set_controls_visible(true);
        return;
    }
    ++g_cur_index;
    render_current_frame();
}

lv_obj_t* make_round_btn(lv_obj_t* parent, const char* sym, int32_t sz,
                         lv_event_cb_t cb, lv_obj_t** out_icon)
{
    lv_obj_t* btn = lv_button_create(parent);
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
    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_color(icon, user_color_text, 0);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);
    lv_obj_center(icon);
    if(out_icon) {
        *out_icon = icon;
    }
    return btn;
}

} // namespace

void page_user_timelapse_set_source(const char* sequence_dir_abs,
                                    const char* title,
                                    const char* subtitle)
{
    std::snprintf(g_seq_dir, sizeof(g_seq_dir), "%s", sequence_dir_abs ? sequence_dir_abs : "");
    std::snprintf(g_title, sizeof(g_title), "%s", title ? title : "Timelapse");
    std::snprintf(g_sub, sizeof(g_sub), "%s", subtitle ? subtitle : "");
}

void page_user_timelapse_refresh(bool)
{
    if(!g_page) {
        return;
    }
    set_controls_visible(true);
    g_dragging = false;
    g_playing = false;

    if(g_title_lbl) {
        lv_label_set_text(g_title_lbl, g_title[0] ? g_title : "Timelapse");
    }
    if(g_sub_lbl) {
        lv_label_set_text(g_sub_lbl, g_sub);
    }

    if(!load_sequence_frames()) {
        if(g_video_img) {
            lv_image_set_src(g_video_img, nullptr);
            lv_obj_invalidate(g_video_img);
        }
        if(g_time_lbl) {
            lv_label_set_text(g_time_lbl, "0 / 0");
        }
        set_play_icon(false);
        return;
    }
    render_current_frame();
    g_playing = true;
    set_play_icon(true);
}

void page_user_timelapse_exit(void)
{
    g_playing = false;
    g_frames.clear();
    g_cur_index = 0;
}

lv_obj_t* page_user_timelapse_create(lv_obj_t* parent)
{
    (void)parent;
    lv_obj_t* page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page = page;

    lv_obj_t* img = lv_image_create(page);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, on_video_click, LV_EVENT_CLICKED, nullptr);
    g_video_img = img;

    lv_obj_t* ov = lv_obj_create(page);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    g_overlay = ov;

    lv_obj_t* title_box = lv_obj_create(ov);
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

    lv_obj_t* tl = lv_label_create(title_box);
    lv_label_set_text(tl, g_title[0] ? g_title : "Timelapse");
    lv_obj_set_style_text_color(tl, user_color_text, 0);
    lv_obj_set_style_text_font(tl, user_font_text(), 0);
    g_title_lbl = tl;

    lv_obj_t* sub = lv_label_create(title_box);
    lv_label_set_text(sub, g_sub);
    lv_obj_set_style_text_color(sub, user_color_text_dim, 0);
    lv_obj_set_style_text_font(sub, user_font_caption(), 0);
    g_sub_lbl = sub;

    const int32_t btn_sz = USER_BACK_BTN_H;
    lv_obj_t* close = make_round_btn(ov, LV_SYMBOL_CLOSE, btn_sz, on_close_click, nullptr);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);

    const int32_t play_sz = USER_BACK_BTN_H * 2;
    lv_obj_t* play = make_round_btn(ov, LV_SYMBOL_PAUSE, play_sz, on_play_click, &g_play_icon);
    lv_obj_set_style_text_font(g_play_icon, user_font_value(), 0);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* bottom = lv_obj_create(ov);
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

    lv_obj_t* time_lbl = lv_label_create(bottom);
    lv_label_set_text(time_lbl, "0 / 0");
    lv_obj_set_style_text_color(time_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(time_lbl, user_font_caption(), 0);
    g_time_lbl = time_lbl;

    lv_obj_t* slider = lv_slider_create(bottom);
    lv_obj_set_flex_grow(slider, 1);
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

    if(!g_play_timer) {
        g_play_timer = lv_timer_create(play_timer_cb, kPlayIntervalMs, nullptr);
    }

    return page;
}
