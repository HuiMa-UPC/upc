#include "page_user_timelapse.hpp"

#include "page_manager.hpp"
#include "png_scrub_preview.hpp"
#include "ui_feedback.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "video_player.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class PlaybackMode : uint8_t {
    None = 0,
    PngSequence,
    Mp4,
};

constexpr uint32_t kPlayIntervalMs = 200u; /* 序列回放默认 5fps */
constexpr const char* kFsDrive = "A:";
/** JPEG 序列拖动预览节流；MP4 拖动中不 seek，仅松手 settle。 */
constexpr uint64_t kPreviewReqMinIntervalMs = 120;

/* 进度条命中区向外扩多少（px）。轨道视觉上只有 24~40px 高（随 profile），
 * 全屏播放时它是唯一的定位手段，而且是拖动而非点击，起手落偏一点就丢事件。
 * 取 UI_USER_MIN_TOUCH 这个量级：轨道上方多出整整一指的可抓范围。
 * 生效要同时满足两处——bottom 的 pad_top 撑开父容器、slider 的
 * ext_click_area 放大自身判定，原因见各自注释。 */
constexpr int32_t kScrubTouchPad = UI_USER_MIN_TOUCH;

char g_seq_dir[256] = {0};
char g_mp4_path[256] = {0};
char g_title[96] = {0};
char g_sub[64] = {0};

std::vector<std::string> g_frames; /* LVGL 路径 A:/... */
size_t g_cur_index = 0;
bool g_playing = false;
bool g_controls_visible = true;
bool g_dragging = false;
bool g_resume_after_drag = false;
PlaybackMode g_mode = PlaybackMode::None;
int64_t g_mp4_pos_ms = 0;
int64_t g_mp4_dur_ms = 0;
int64_t g_last_preview_req_idx = -1;
uint64_t g_last_preview_req_tick_ms = 0;
bool g_preview_chrome_shown = false;

/** 按下时缓存轨道/预览框几何，拖动中禁止 update_layout / move_foreground。 */
struct ScrubGeom {
    bool valid = false;
    int32_t slider_x1 = 0;
    int32_t slider_w = 0;
    int32_t box_w = 0;
    int32_t box_h = 0;
    int32_t box_y = 0;
    int32_t page_w = 0;
};
ScrubGeom g_scrub_geom;

lv_obj_t* g_page = nullptr;
lv_obj_t* g_video_img = nullptr;
lv_obj_t* g_overlay = nullptr;
lv_obj_t* g_play_icon = nullptr;
lv_obj_t* g_slider = nullptr;
lv_obj_t* g_time_lbl = nullptr;
lv_obj_t* g_title_lbl = nullptr;
lv_obj_t* g_sub_lbl = nullptr;
lv_obj_t* g_preview_box = nullptr;
lv_obj_t* g_preview_img = nullptr;
lv_obj_t* g_preview_time = nullptr;
lv_timer_t* g_play_timer = nullptr;
int32_t g_preview_box_w = 160;
int32_t g_preview_box_h = 128;

void play_timer_cb(lv_timer_t*);

uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string lvgl_to_abs_path(const std::string& src)
{
    if(src.size() >= 2 && src[0] == 'A' && src[1] == ':') {
        return src.substr(2);
    }
    return src;
}

size_t slider_to_png_index(int32_t slider_value)
{
    if(g_frames.empty()) {
        return 0;
    }
    const size_t max_idx = g_frames.size() - 1u;
    return static_cast<size_t>(
        (static_cast<int64_t>(slider_value) * static_cast<int64_t>(max_idx) + 500) /
        1000);
}

bool parse_png_index(const char* name, uint32_t& out)
{
    if(!name || !name[0]) {
        return false;
    }
    size_t i = 0;
    while(name[i] && std::isdigit(static_cast<unsigned char>(name[i]))) {
        ++i;
    }
    if(i == 0 || std::strcmp(name + i, ".jpg") != 0) {
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

void set_slider_enabled(bool enabled)
{
    if(!g_slider) {
        return;
    }
    if(enabled) {
        lv_obj_clear_state(g_slider, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g_slider, LV_STATE_DISABLED);
    }
}

/** 页面只 create 一次；exit 不得删 timer，否则再进只跑 refresh，序列再也走不动。 */
void ensure_play_timer()
{
    if(!g_play_timer) {
        g_play_timer = lv_timer_create(play_timer_cb, kPlayIntervalMs, nullptr);
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

void format_ms(int64_t ms, char* out, size_t n)
{
    if(!out || n == 0) {
        return;
    }
    const uint32_t total = static_cast<uint32_t>(std::max<int64_t>(0, ms) / 1000);
    const uint32_t h = total / 3600u;
    const uint32_t m = (total % 3600u) / 60u;
    const uint32_t s = total % 60u;
    if(h > 0) {
        std::snprintf(out, n, "%u:%02u:%02u", h, m, s);
    } else {
        std::snprintf(out, n, "%u:%02u", m, s);
    }
}

void format_png_frame_time(uint32_t frame_index, char* out, size_t n)
{
    const uint32_t total_sec = static_cast<uint32_t>(
        (static_cast<uint64_t>(frame_index) * kPlayIntervalMs) / 1000u);
    format_ms(static_cast<int64_t>(total_sec) * 1000, out, n);
}

void cache_scrub_geometry()
{
    g_scrub_geom.valid = false;
    if(!g_preview_box || !g_slider || !g_page) {
        return;
    }
    lv_obj_update_layout(g_slider);
    lv_area_t slider_area;
    lv_obj_get_coords(g_slider, &slider_area);
    lv_obj_update_layout(g_preview_box);
    g_scrub_geom.slider_x1 = slider_area.x1;
    g_scrub_geom.slider_w = lv_area_get_width(&slider_area);
    g_scrub_geom.box_w = lv_obj_get_width(g_preview_box);
    g_scrub_geom.box_h = lv_obj_get_height(g_preview_box);
    g_scrub_geom.page_w = lv_obj_get_width(g_page);
    g_scrub_geom.box_y =
        slider_area.y1 - g_scrub_geom.box_h - USER_PAD_SM;
    g_scrub_geom.valid = (g_scrub_geom.slider_w > 0 && g_scrub_geom.box_w > 0);
}

void position_preview_box(int32_t slider_value, bool allow_layout)
{
    if(!g_preview_box || !g_slider || !g_page) {
        return;
    }
    if(allow_layout || !g_scrub_geom.valid) {
        cache_scrub_geometry();
    }
    if(!g_scrub_geom.valid) {
        return;
    }
    const int32_t knob_x = g_scrub_geom.slider_x1 + static_cast<int32_t>(
        static_cast<int64_t>(g_scrub_geom.slider_w) * slider_value / 1000);
    int32_t x = knob_x - g_scrub_geom.box_w / 2;
    const int32_t max_x = g_scrub_geom.page_w - g_scrub_geom.box_w;
    if(x < 0) {
        x = 0;
    }
    if(max_x >= 0 && x > max_x) {
        x = max_x;
    }
    /* 拖动中只改 x：VALUE_CHANGED 里 update_layout / set_pos 全量改会重入布局。 */
    if(allow_layout) {
        lv_obj_set_pos(g_preview_box, x, g_scrub_geom.box_y);
    } else {
        lv_obj_set_x(g_preview_box, x);
    }
}

void show_preview_chrome(int32_t slider_value, const char* time_text)
{
    if(g_preview_time && time_text) {
        lv_label_set_text(g_preview_time, time_text);
    }
    const bool first_show = !g_preview_chrome_shown;
    position_preview_box(slider_value, first_show);
    if(g_preview_box) {
        if(first_show) {
            lv_obj_remove_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_preview_box);
            g_preview_chrome_shown = true;
        } else if(lv_obj_has_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN);
            g_preview_chrome_shown = true;
        }
    }
}

void hide_preview_chrome()
{
    /* 先摘掉预览图源，再藏盒子：避免 LVGL 仍引用解码线程即将复用/释放的缓冲。 */
    if(g_preview_img) {
        lv_image_set_src(g_preview_img, nullptr);
    }
    if(g_preview_box) {
        lv_obj_add_flag(g_preview_box, LV_OBJ_FLAG_HIDDEN);
    }
    g_preview_chrome_shown = false;
    g_scrub_geom.valid = false;
}

void update_progress_ui_png()
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
    if(g_slider && !g_dragging && total > 1) {
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

void update_progress_ui_mp4(int64_t cur_ms, int64_t dur_ms)
{
    g_mp4_pos_ms = std::max<int64_t>(0, cur_ms);
    g_mp4_dur_ms = std::max<int64_t>(0, dur_ms);
    if(g_slider && !g_dragging && g_mp4_dur_ms > 0) {
        const int32_t v = static_cast<int32_t>(
            std::clamp<int64_t>(g_mp4_pos_ms * 1000 / g_mp4_dur_ms, 0, 1000));
        lv_slider_set_value(g_slider, v, LV_ANIM_OFF);
    }
    if(g_time_lbl && !g_dragging) {
        char cur_buf[16];
        char dur_buf[16];
        format_ms(g_mp4_pos_ms, cur_buf, sizeof(cur_buf));
        format_ms(g_mp4_dur_ms, dur_buf, sizeof(dur_buf));
        char text[40];
        std::snprintf(text, sizeof(text), "%s / %s", cur_buf, dur_buf);
        lv_label_set_text(g_time_lbl, text);
    }
}

void update_drag_time_png(int32_t slider_value, size_t idx)
{
    char tbuf[16];
    format_png_frame_time(static_cast<uint32_t>(idx), tbuf, sizeof(tbuf));
    show_preview_chrome(slider_value, tbuf);
    if(g_time_lbl) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%zu / %zu",
                      idx + 1u, g_frames.size());
        lv_label_set_text(g_time_lbl, buf);
    }
}

void update_drag_time_mp4(int32_t slider_value, int64_t target_ms)
{
    char tbuf[16];
    format_ms(target_ms, tbuf, sizeof(tbuf));
    show_preview_chrome(slider_value, tbuf);
    if(g_time_lbl) {
        char dur_buf[16];
        format_ms(g_mp4_dur_ms, dur_buf, sizeof(dur_buf));
        char text[40];
        std::snprintf(text, sizeof(text), "%s / %s", tbuf, dur_buf);
        lv_label_set_text(g_time_lbl, text);
    }
}

void on_png_scrub_preview(const lv_image_dsc_t* image, size_t /*frame_index*/)
{
    if(g_mode != PlaybackMode::PngSequence || !g_dragging || !image || !g_preview_img) {
        return;
    }
    lv_image_set_src(g_preview_img, image);
    lv_obj_invalidate(g_preview_img);
}

void request_drag_preview_png(int32_t slider_value, bool force_request = false)
{
    if(g_frames.empty()) {
        return;
    }
    const size_t idx = slider_to_png_index(slider_value);
    g_cur_index = idx;
    update_drag_time_png(slider_value, idx);

    const uint64_t now_ms = monotonic_ms();
    const bool first_req = g_last_preview_req_idx < 0;
    const bool interval_ok =
        now_ms - g_last_preview_req_tick_ms >= kPreviewReqMinIntervalMs;
    const bool index_changed =
        first_req ||
        static_cast<int64_t>(idx) != g_last_preview_req_idx;
    if(!(force_request || (index_changed && (first_req || interval_ok)))) {
        return;
    }

    g_last_preview_req_idx = static_cast<int64_t>(idx);
    g_last_preview_req_tick_ms = now_ms;

#if defined(UPC_UI_HAS_LIBPNG) && UPC_UI_HAS_LIBPNG
    ui::PngScrubPreview::instance().request(
        idx, lvgl_to_abs_path(g_frames[idx]));
#else
    /* 无 libpng 时回退：仅刷新小预览框，主画面仍等松手 settle。 */
    if(g_preview_img) {
        lv_image_set_src(g_preview_img, g_frames[idx].c_str());
        lv_obj_invalidate(g_preview_img);
    }
#endif
}

void request_drag_preview_mp4(int32_t slider_value, bool /*force_request*/ = false)
{
    const int64_t dur_ms = g_mp4_dur_ms;
    if(dur_ms <= 0) {
        return;
    }
    const int64_t target_ms = std::clamp<int64_t>(
        (static_cast<int64_t>(slider_value) * dur_ms) / 1000, 0, dur_ms);
    /* 拖动只挪预览框与时间；连续 H264 seek 很重，且易与 LVGL 布局重入叠崩。
     * 主画面定位留给松手时的 seek()。 */
    update_drag_time_mp4(slider_value, target_ms);
    (void)target_ms;
}

void render_current_png_frame()
{
    if(!g_video_img || g_frames.empty()) {
        return;
    }
    if(g_cur_index >= g_frames.size()) {
        g_cur_index = g_frames.size() - 1u;
    }
    lv_image_set_src(g_video_img, g_frames[g_cur_index].c_str());
    lv_obj_invalidate(g_video_img);
    update_progress_ui_png();
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

void on_mp4_progress(int64_t current_ms, int64_t duration_ms)
{
    if(g_mode != PlaybackMode::Mp4 || g_dragging) {
        return;
    }
    update_progress_ui_mp4(current_ms, duration_ms);
}

void on_mp4_ended()
{
    if(g_mode != PlaybackMode::Mp4) {
        return;
    }
    g_playing = false;
    set_play_icon(false);
    set_controls_visible(true);
}

void on_mp4_preview(const lv_image_dsc_t* image, int64_t position_ms)
{
    if(g_mode != PlaybackMode::Mp4 || !g_dragging || !image || !g_preview_img) {
        return;
    }
    lv_image_set_src(g_preview_img, image);
    lv_obj_invalidate(g_preview_img);
    if(g_preview_time) {
        char tbuf[16];
        format_ms(position_ms, tbuf, sizeof(tbuf));
        lv_label_set_text(g_preview_time, tbuf);
    }
}

bool load_mp4_playback()
{
    if(g_mp4_path[0] == '\0' || !g_video_img) {
        return false;
    }
    ui::VideoPlayer& player = ui::VideoPlayer::instance();
    player.set_progress_cb(on_mp4_progress);
    player.set_ended_cb(on_mp4_ended);
    player.set_preview_cb(on_mp4_preview);
    player.set_scrubbing(false);
    if(!player.open(g_video_img, g_mp4_path)) {
        return false;
    }
    g_mode = PlaybackMode::Mp4;
    g_mp4_dur_ms = player.duration_ms();
    g_mp4_pos_ms = 0;
    update_progress_ui_mp4(0, g_mp4_dur_ms);
    const bool started = player.play();
    g_playing = started;
    set_play_icon(started);
    set_slider_enabled(g_mp4_dur_ms > 0);
    return started;
}

void on_video_click(lv_event_t*)
{
    set_controls_visible(!g_controls_visible);
}

void on_play_click(lv_event_t*)
{
    if(g_mode == PlaybackMode::Mp4) {
        auto& player = ui::VideoPlayer::instance();
        if(!player.is_open()) {
            return;
        }
        if(g_playing) {
            player.pause();
            g_playing = false;
            set_play_icon(false);
        } else {
            /* 播完后 position 已贴 duration；必须先 seek(0) 再 play，否则下一帧立刻 EOF。 */
            const int64_t dur_ms = g_mp4_dur_ms > 0 ? g_mp4_dur_ms : player.duration_ms();
            const int64_t pos_ms = player.position_ms();
            if(dur_ms > 0 && pos_ms + 1 >= dur_ms) {
                g_mp4_pos_ms = 0;
                (void)player.seek(0);
            }
            g_playing = player.play();
            set_play_icon(g_playing);
        }
        return;
    }

    if(g_mode == PlaybackMode::PngSequence && !g_frames.empty()) {
        ensure_play_timer();
        /* 停在末帧时重新点播放要先回到开头，否则 play_timer_cb 下一拍就撞上
         * 结束判定又把 g_playing 关掉，表现为点了没反应。与上面 MP4 分支到
         * 末尾先 seek(0) 同理。 */
        if(!g_playing && g_cur_index + 1u >= g_frames.size()) {
            g_cur_index = 0;
            render_current_png_frame();
        }
        g_playing = !g_playing;
        set_play_icon(g_playing);
    }
}

void on_close_click(lv_event_t*)
{
    ui::PageManager::instance().switch_to("user_history");
}

void on_slider_pressed(lv_event_t*)
{
    g_dragging = true;
    g_resume_after_drag = g_playing;
    g_playing = false;
    set_play_icon(false);
    g_last_preview_req_idx = -1;
    g_last_preview_req_tick_ms = 0;
    /* 按下时一次性量几何；后续 VALUE_CHANGED 复用，禁止再 update_layout。 */
    cache_scrub_geometry();

    if(g_mode == PlaybackMode::Mp4) {
        auto& player = ui::VideoPlayer::instance();
        player.pause();
        player.set_scrubbing(true);
        if(g_slider) {
            request_drag_preview_mp4(lv_slider_get_value(g_slider), true);
        }
        return;
    }

    if(g_mode == PlaybackMode::PngSequence && g_slider) {
        ui::PngScrubPreview::instance().set_scrubbing(true);
        request_drag_preview_png(lv_slider_get_value(g_slider), true);
    }
}

void on_slider_value_changed(lv_event_t*)
{
    if(!g_dragging || !g_slider) {
        return;
    }
    const int32_t v = lv_slider_get_value(g_slider);
    if(g_mode == PlaybackMode::Mp4) {
        request_drag_preview_mp4(v);
        return;
    }
    if(g_mode == PlaybackMode::PngSequence && !g_frames.empty()) {
        /* 拖动中只刷预览框，主画面等松手 settle。 */
        request_drag_preview_png(v);
    }
}

void on_slider_released(lv_event_t*)
{
    hide_preview_chrome();

    if(g_mode == PlaybackMode::Mp4 && g_slider) {
        ui::VideoPlayer::instance().set_scrubbing(false);
        const int64_t dur_ms = g_mp4_dur_ms > 0 ? g_mp4_dur_ms
                                                : ui::VideoPlayer::instance().duration_ms();
        const int32_t v = lv_slider_get_value(g_slider);
        const int64_t target_ms = dur_ms > 0
            ? std::clamp<int64_t>((static_cast<int64_t>(v) * dur_ms) / 1000, 0, dur_ms)
            : 0;
        g_mp4_pos_ms = target_ms;
        update_progress_ui_mp4(target_ms, dur_ms);
        (void)ui::VideoPlayer::instance().seek(target_ms);
        /* 播完后拖回中间也应续播；停在终点则保持暂停。 */
        const bool should_play =
            g_resume_after_drag || (dur_ms > 0 && target_ms + 1 < dur_ms);
        if(should_play) {
            g_playing = ui::VideoPlayer::instance().play();
            set_play_icon(g_playing);
        }
    } else if(g_mode == PlaybackMode::PngSequence && g_slider && !g_frames.empty()) {
        ui::PngScrubPreview::instance().set_scrubbing(false);
        g_cur_index = slider_to_png_index(lv_slider_get_value(g_slider));
        render_current_png_frame();
        ensure_play_timer();
        /* 同上：末帧拖回前面要接着播，否则只能再点一次播放键。 */
        const bool should_play =
            g_resume_after_drag || (g_cur_index + 1u < g_frames.size());
        if(should_play) {
            g_playing = true;
            set_play_icon(true);
        }
    }

    g_dragging = false;
    g_resume_after_drag = false;
}

void play_timer_cb(lv_timer_t*)
{
    if(g_mode != PlaybackMode::PngSequence || !g_playing || g_dragging || g_frames.empty()) {
        return;
    }
    if(g_cur_index + 1u >= g_frames.size()) {
        g_playing = false;
        set_play_icon(false);
        set_controls_visible(true);
        return;
    }
    ++g_cur_index;
    render_current_png_frame();
}

} // namespace

void page_user_timelapse_set_source(const char* sequence_dir_abs,
                                    const char* mp4_abs_path,
                                    const char* title,
                                    const char* subtitle)
{
    std::snprintf(g_seq_dir, sizeof(g_seq_dir), "%s", sequence_dir_abs ? sequence_dir_abs : "");
    std::snprintf(g_mp4_path, sizeof(g_mp4_path), "%s", mp4_abs_path ? mp4_abs_path : "");
    std::snprintf(g_title, sizeof(g_title), "%s", title ? title : "Timelapse");
    std::snprintf(g_sub, sizeof(g_sub), "%s", subtitle ? subtitle : "");
}

void page_user_timelapse_refresh(bool)
{
    if(!g_page) {
        return;
    }
    /* 隐藏页再进只走 refresh，不重跑 create：补建 timer，控件指针仍指向未销毁对象。 */
    ensure_play_timer();
    set_controls_visible(true);
    g_dragging = false;
    g_resume_after_drag = false;
    g_playing = false;
    g_mode = PlaybackMode::None;
    g_mp4_pos_ms = 0;
    g_mp4_dur_ms = 0;
    g_last_preview_req_idx = -1;
    g_last_preview_req_tick_ms = 0;
    g_preview_chrome_shown = false;
    g_scrub_geom.valid = false;
    g_frames.clear();
    g_cur_index = 0;

    ui::PngScrubPreview::instance().cancel();
    auto& player = ui::VideoPlayer::instance();
    player.set_scrubbing(false);
    player.pause();
    player.close();

    if(g_title_lbl) {
        lv_label_set_text(g_title_lbl, g_title[0] ? g_title : "Timelapse");
    }
    if(g_sub_lbl) {
        lv_label_set_text(g_sub_lbl, g_sub);
    }

    /* 历史页在 UPC_TIMELAPSE_PLAYBACK_PNG 下对延时不传 mp4；打印录像仍传 print_h264.mp4。 */
    if(load_mp4_playback()) {
        hide_preview_chrome();
        return;
    } else if(g_mp4_path[0] != '\0') {
        ui_toast_show("MP4 open failed, fallback JPEG", UI_TOAST_INFORM, 1200);
    }

    if(!load_sequence_frames()) {
        if(g_video_img) {
            lv_image_set_src(g_video_img, nullptr);
            lv_obj_invalidate(g_video_img);
        }
        hide_preview_chrome();
        if(g_time_lbl) {
            lv_label_set_text(g_time_lbl, "0 / 0");
        }
        set_slider_enabled(false);
        set_play_icon(false);
        return;
    }
    g_mode = PlaybackMode::PngSequence;
    ui::PngScrubPreview::instance().set_target_size(g_preview_box_w, g_preview_box_h);
    ui::PngScrubPreview::instance().set_preview_cb(on_png_scrub_preview);
    set_slider_enabled(g_frames.size() > 1u);
    render_current_png_frame();
    g_playing = true;
    set_play_icon(true);
}

void page_user_timelapse_exit(void)
{
    /* 页面只是 HIDDEN，控件与 play_timer 仍存活；清指针/删 timer 会导致下次
     * refresh 无法刷主画面、JPEG 序列不再推进（点播放像没反应）。 */
    g_playing = false;
    g_dragging = false;
    g_mode = PlaybackMode::None;
    hide_preview_chrome();
    ui::PngScrubPreview::instance().set_preview_cb(nullptr);
    ui::PngScrubPreview::instance().cancel();
    ui::VideoPlayer::instance().set_preview_cb(nullptr);
    ui::VideoPlayer::instance().set_progress_cb(nullptr);
    ui::VideoPlayer::instance().set_ended_cb(nullptr);
    ui::VideoPlayer::instance().set_scrubbing(false);
    ui::VideoPlayer::instance().pause();
    ui::VideoPlayer::instance().close();
    g_frames.clear();
    g_cur_index = 0;
    g_mp4_pos_ms = 0;
    g_mp4_dur_ms = 0;
    g_last_preview_req_idx = -1;
    g_last_preview_req_tick_ms = 0;
    g_preview_chrome_shown = false;
    g_scrub_geom.valid = false;
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

    /* 主画面 1:1：原图像素不缩放（竖图 1080×1280 / 1024×1280）。
     * CENTER 居中铺在全屏黑底上，左右留黑边；屏高不足时由控件裁到屏幕，
     * 不把整图等比缩小（不再用 CONTAIN）。 */
    lv_obj_t* img = lv_image_create(page);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(img, LV_SCALE_NONE);
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

    /* 圆圈放大后图标字号得跟着提：helper 内部固定用 user_font_text()，只放大
     * 圆圈的话图标会浮在一片空底上（播放键同理，见下）。 */
    const int32_t btn_sz = user_media_btn_size();
    lv_obj_t* close_icon = nullptr;
    lv_obj_t* close = ui_user_make_overlay_round_btn(ov, LV_SYMBOL_CLOSE, btn_sz,
                                                    on_close_click, &close_icon);
    if(close_icon) {
        lv_obj_set_style_text_font(close_icon, user_font_icon(), 0);
    }
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);

    /* 播放钮走独立的主题字段，不跟着关闭钮的放大一起涨（见 ui_theme.hpp） */
    const int32_t play_sz = user_media_play_size();
    lv_obj_t* play = ui_user_make_overlay_round_btn(ov, LV_SYMBOL_PAUSE, play_sz,
                                                   on_play_click, &g_play_icon);
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
    /* 顶部留白把这条（透明的）容器往上撑高，纯粹给进度条让出命中空间。
     * 必须撑父容器：lv_indev_search_obj 递归进子节点的判据是父节点的原始
     * coords，不含 ext_click_area，只给 slider 外扩的话上方那片区域根本
     * 递归不下来。容器是 BOTTOM_MID 对齐 + SIZE_CONTENT，加 pad_top 只让
     * 盒子朝上长，里面的时间标签和进度条位置分毫不动。 */
    lv_obj_set_style_pad_top(bottom, kScrubTouchPad, 0);
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
    /* 与上面 bottom 的 pad_top 配套：父容器负责让点能递归下来，这里负责让
     * 滑块自己认这些点。两者缺一不可，取同一个值。
     * 四边等量外扩：左右伸到时间标签和边距下面（都不可点，不抢事件），
     * 向下伸出屏幕边缘（无害），向上正好落在刚撑出来的那片留白里。 */
    lv_obj_set_ext_click_area(slider, kScrubTouchPad);

    /* knob 比轨道高（3H vs 2H），上下各溢出 (knob_sz - slider_touch_h)/2；容器是
     * SIZE_CONTENT，高度只跟着轨道走，溢出的那两截会被父容器裁掉——表现为本该是
     * 正圆的 knob 渲染成上下削平的横向胶囊（实机 1024 档：36×36 被压成 36×24）。
     * 补一段下边距把 knob 整个圈进来（上方由 pad_top 那 48px 兜住），再多留
     * USER_PAD_SM 与屏幕边缘拉开距离。容器 BOTTOM_MID 对齐，加下边距等于把
     * 里面的内容整体上抬。 */
    const int32_t knob_overflow = (knob_sz - slider_touch_h + 1) / 2;
    lv_obj_set_style_pad_bottom(bottom, knob_overflow + USER_PAD_SM, 0);

    lv_obj_add_event_cb(slider, on_slider_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(slider, on_slider_value_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_PRESS_LOST, nullptr);
    g_slider = slider;

    g_preview_box_w = USER_SLIDE_W * 3 / 5;
    g_preview_box_h = g_preview_box_w * 4 / 5;
    lv_obj_t* pv = lv_obj_create(ov);
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

    lv_obj_t* pimg = lv_image_create(pv);
    lv_obj_set_size(pimg, g_preview_box_w, g_preview_box_h);
    lv_obj_set_style_bg_color(pimg, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(pimg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pimg, USER_RADIUS / 2, 0);
    lv_obj_set_style_clip_corner(pimg, true, 0);
    /* 拖动预览小窗：整图等比塞进框（可缩），不裁切；主画面才是 1:1。 */
    lv_image_set_inner_align(pimg, LV_IMAGE_ALIGN_CONTAIN);
    g_preview_img = pimg;

    lv_obj_t* ptime = lv_label_create(pv);
    lv_label_set_text(ptime, "0:00");
    lv_obj_set_style_text_color(ptime, user_color_text, 0);
    lv_obj_set_style_text_font(ptime, user_font_caption(), 0);
    g_preview_time = ptime;

    ui::PngScrubPreview::instance().set_target_size(g_preview_box_w, g_preview_box_h);
    ui::PngScrubPreview::instance().set_preview_cb(on_png_scrub_preview);

    ensure_play_timer();

    return page;
}
