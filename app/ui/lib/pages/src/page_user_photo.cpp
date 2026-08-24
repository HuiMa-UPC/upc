#include "page_user_photo.hpp"

#include "album_index.hpp"
#include "page_manager.hpp"
#include "photo_decoder.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

/**
 * @file page_user_photo.cpp
 * @brief 相册全屏浏览器（隐藏页 user_photo，历史页 Album 缩略图进入）。
 *
 * 结构（与延时摄影播放器同一套 chrome，建在 lv_layer_top() 上盖住侧栏与常驻条）：
 *   ┌──────────────────────────────────────────────────────┐
 *   │ ┌ photo_xxx.png ─┐                              ( X )│
 *   │ │ 21:00:13 …  3/42│                                  │
 *   │ └─────────────────┘                                  │
 *   │ ( ‹ )        [ 照片 CONTAIN 居中 ]           ( › )    │
 *   └──────────────────────────────────────────────────────┘
 *
 * 三条翻页途径等价，都走 show_relative()：左右滑动、两侧箭头、首末循环回绕。
 * 单击主图切换控件层显隐。
 *
 * 画面分两拍上：先贴 .rthm 缩略图顶住（几乎零成本），再由 ui::PhotoDecoder 后台
 * 解出贴合屏幕的 RGB565 大图换上——原因见 photo_decoder.hpp 的类注释。
 *
 * 竖构图照片在横屏上 CONTAIN 后左右各留约 1/4 屏宽黑边，两侧箭头正落在黑边上，
 * 不压照片；这是设计选择，不是没铺满。
 */

namespace {

/* LVGL POSIX FS 盘符（lv_conf: LV_FS_POSIX_LETTER 'A'） */
constexpr const char* kFsDrive = "A:";

/** 起拖阈值：小于它仍算点击（与历史页左滑同口径，避免手抖把单击吃掉） */
constexpr int32_t kSwipeStartPx = 12;

/** 翻页判定：横向位移超过屏宽的 1/kSwipeCommitDiv 才换图，否则弹回 */
constexpr int32_t kSwipeCommitDiv = 4;

std::vector<ui::album::PhotoEntry> g_photos;
size_t g_index = 0;
char g_start_path[512] = {0};

lv_obj_t* g_page = nullptr;
lv_obj_t* g_img = nullptr;
lv_obj_t* g_overlay = nullptr;
lv_obj_t* g_title_lbl = nullptr;
lv_obj_t* g_sub_lbl = nullptr;
lv_obj_t* g_prev_btn = nullptr;
lv_obj_t* g_next_btn = nullptr;
lv_obj_t* g_hint_lbl = nullptr;  /**< 「Loading…」/「No photos」占位，挂页根故不随控件层隐藏 */

bool g_controls_visible = true;

/* 跟手滑动的按压期状态，每次 PRESSED 重置 */
int32_t g_press_x = 0;
int32_t g_press_y = 0;
bool g_dragging = false;
bool g_drag_locked = false;
bool g_just_dragged = false;

/** 每次换图自增：把后台解码回调对齐到「当前该显示的那张」，过期的直接丢 */
uint64_t g_show_token = 0;

void show_index(size_t idx);

std::string lvgl_path(const std::string& abs)
{
    return std::string(kFsDrive) + abs;
}

std::string filename_of(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/** 解码目标框 = 当前 display 分辨率（主题分三档，屏尺寸只能运行时问） */
void display_box(int32_t& w, int32_t& h)
{
    lv_display_t* d = lv_display_get_default();
    w = d ? lv_display_get_horizontal_resolution(d) : 1024;
    h = d ? lv_display_get_vertical_resolution(d) : 600;
    if(w <= 0) {
        w = 1024;
    }
    if(h <= 0) {
        h = 600;
    }
}

/** 副标题：拍摄时间 + 「第几 / 共几」。相册无独立元数据，时间取文件 mtime。 */
void format_subtitle(const std::string& path, size_t idx, size_t total,
                     char* buf, size_t n)
{
    struct stat st {};
    if(stat(path.c_str(), &st) != 0) {
        std::snprintf(buf, n, "%zu / %zu", idx + 1u, total);
        return;
    }
    const std::time_t t = static_cast<std::time_t>(st.st_mtime);
    std::tm tm {};
    localtime_r(&t, &tm);
    char time_buf[16];
    char date_buf[24];
    /* 与历史页 format_date / format_time_range 同口径，两页观感一致 */
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
    std::strftime(date_buf, sizeof(date_buf), "%b %d, %Y", &tm);
    std::snprintf(buf, n, "%s  %s   %zu / %zu",
                  time_buf, date_buf, idx + 1u, total);
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

void set_hint(const char* text)
{
    if(!g_hint_lbl) {
        return;
    }
    if(text && text[0]) {
        lv_label_set_text(g_hint_lbl, text);
        lv_obj_remove_flag(g_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 只有一张（或没有）照片时藏掉翻页钮：回绕到自己没有意义 */
void update_arrows(void)
{
    const bool many = g_photos.size() > 1u;
    if(g_prev_btn) {
        if(many) {
            lv_obj_remove_flag(g_prev_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_prev_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_next_btn) {
        if(many) {
            lv_obj_remove_flag(g_next_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_next_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void reset_img_shift(void)
{
    if(g_img) {
        lv_obj_set_style_translate_x(g_img, 0, 0);
    }
}

/** 后台大图就绪（主线程回调）。image 为 nullptr = 后台解不出来。 */
void on_photo_ready(const lv_image_dsc_t* image, uint64_t token)
{
    if(token != g_show_token || !g_img || g_photos.empty()) {
        return; /* 已经翻到别的照片，这张过期了 */
    }
    if(!image) {
        /* 后台只解 PNG（libpng）。jpg/bmp 交给 LVGL 内置 TJPGD / BMP 同步解一次；
         * 先问一遍 decoder 能不能认，认不出来（如 webp 当前无解码器）就给提示，
         * 而不是留一屏黑让人以为卡住了。 */
        const std::string p = lvgl_path(g_photos[g_index].path);
        lv_image_header_t hdr {};
        if(lv_image_decoder_get_info(p.c_str(), &hdr) == LV_RESULT_OK) {
            lv_image_set_src(g_img, p.c_str());
            set_hint(nullptr);
        } else {
            lv_image_set_src(g_img, nullptr);
            set_hint("Cannot display this photo");
        }
        lv_obj_invalidate(g_img);
        return;
    }
    lv_image_set_src(g_img, image);
    lv_obj_invalidate(g_img);
    set_hint(nullptr);
}

void show_index(size_t idx)
{
    if(g_photos.empty()) {
        return;
    }
    if(idx >= g_photos.size()) {
        idx = 0;
    }
    g_index = idx;
    const ui::album::PhotoEntry& e = g_photos[idx];

    if(g_title_lbl) {
        lv_label_set_text(g_title_lbl, filename_of(e.path).c_str());
    }
    if(g_sub_lbl) {
        char buf[80];
        format_subtitle(e.path, idx, g_photos.size(), buf, sizeof(buf));
        lv_label_set_text(g_sub_lbl, buf);
    }
    reset_img_shift();

    /* 自增即让上一张的在途解码作废（PhotoDecoder 自己也按世代号丢弃，双保险） */
    ++g_show_token;

    if(g_img) {
        if(!e.thumb.empty()) {
            lv_image_set_src(g_img, lvgl_path(e.thumb).c_str());
            set_hint(nullptr);
        } else {
            /* 无 .rthm：不在主线程抢着解原图（那正是要避开的几百毫秒卡顿），
             * 留个提示等后台解完换上。 */
            lv_image_set_src(g_img, nullptr);
            set_hint("Loading...");
        }
        lv_obj_invalidate(g_img);
    }

    int32_t box_w = 0;
    int32_t box_h = 0;
    display_box(box_w, box_h);
    ui::PhotoDecoder::instance().request(e.path, box_w, box_h, g_show_token);
}

/** 相对翻页（回绕）：三条翻页途径都汇到这里 */
void show_relative(int delta)
{
    const size_t n = g_photos.size();
    if(n == 0) {
        return;
    }
    if(n == 1) {
        reset_img_shift();
        return;
    }
    const size_t next = (delta > 0) ? (g_index + 1u) % n
                                    : (g_index + n - 1u) % n;
    show_index(next);
}

/* ============================================================================
 *  跟手左右滑动
 *
 *  不用 LV_EVENT_GESTURE，理由与历史页左滑删除一致（见 page_user_history.cpp
 *  「左滑删除」一节）：手写 PRESSED/PRESSING/RELEASED 才能跟手、才能做方向锁定，
 *  也才能靠 just_dragged 吃掉松手时那一下 CLICKED。
 * ========================================================================= */

void img_press_cb(lv_event_t* /*e*/)
{
    /* 每次新按压都清干净：上一次若以 PRESS_LOST 收场（没有 CLICKED 来消费），
     * just_dragged 会残留下来把下一次单击吃掉。 */
    g_dragging = false;
    g_drag_locked = false;
    g_just_dragged = false;

    lv_indev_t* indev = lv_indev_active();
    if(!indev || g_photos.size() <= 1u) {
        g_drag_locked = true; /* 单张照片无处可翻，整次按压只当单击 */
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    g_press_x = p.x;
    g_press_y = p.y;
}

void img_pressing_cb(lv_event_t* /*e*/)
{
    if(g_drag_locked || !g_img) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if(!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int32_t dx = p.x - g_press_x;
    const int32_t dy = p.y - g_press_y;

    if(!g_dragging) {
        if(LV_ABS(dx) < kSwipeStartPx) {
            return; /* 还没够起拖，先当单击 */
        }
        if(LV_ABS(dy) > LV_ABS(dx)) {
            g_drag_locked = true; /* 方向锁定：竖向在本页没有用途，不翻页 */
            return;
        }
        g_dragging = true;
    }
    lv_obj_set_style_translate_x(g_img, dx, 0);
}

/** 松手（或按压被抢走）：过阈值翻页，否则弹回 */
void img_release_cb(lv_event_t* /*e*/)
{
    if(!g_dragging) {
        return;
    }
    g_dragging = false;
    g_just_dragged = true;

    const int32_t dx = g_img
                           ? lv_obj_get_style_translate_x(g_img, LV_PART_MAIN)
                           : 0;
    const int32_t commit = g_page
                               ? lv_obj_get_width(g_page) / kSwipeCommitDiv
                               : 0;
    if(commit > 0 && dx <= -commit) {
        show_relative(1);
    } else if(commit > 0 && dx >= commit) {
        show_relative(-1);
    } else {
        reset_img_shift();
    }
}

void img_click_cb(lv_event_t* /*e*/)
{
    if(g_just_dragged) {
        g_just_dragged = false;
        return; /* 刚滑过：这一下 CLICKED 不该切控件层 */
    }
    set_controls_visible(!g_controls_visible);
}

void on_prev_click(lv_event_t* /*e*/)
{
    show_relative(-1);
}

void on_next_click(lv_event_t* /*e*/)
{
    show_relative(1);
}

void on_close_click(lv_event_t* /*e*/)
{
    ui::PageManager::instance().switch_to("user_history");
}

} // namespace

void page_user_photo_set_source(const char* photo_abs_path)
{
    std::snprintf(g_start_path, sizeof(g_start_path), "%s",
                  photo_abs_path ? photo_abs_path : "");
}

void page_user_photo_refresh(bool)
{
    if(!g_page) {
        return;
    }
    set_controls_visible(true);
    g_dragging = false;
    g_drag_locked = false;
    g_just_dragged = false;
    reset_img_shift();
    /* 作废上一次进页遗留的在途回调 */
    ++g_show_token;

    g_photos = ui::album::scan();
    size_t idx = ui::album::index_of(g_photos, g_start_path);
    if(idx == ui::album::kInvalidIndex) {
        idx = 0; /* 落点没了（列表期间被删）→ 退到最新一张 */
    }

    update_arrows();

    if(g_photos.empty()) {
        if(g_img) {
            lv_image_set_src(g_img, nullptr);
            lv_obj_invalidate(g_img);
        }
        if(g_title_lbl) {
            lv_label_set_text(g_title_lbl, "Album");
        }
        if(g_sub_lbl) {
            lv_label_set_text(g_sub_lbl, "");
        }
        set_hint("No photos");
        return;
    }
    show_index(idx);
}

void page_user_photo_exit(void)
{
    ++g_show_token;
    g_dragging = false;
    g_drag_locked = false;
    g_just_dragged = false;

    /* 顺序要紧：先断开控件对解码缓冲的引用，再释放缓冲。反了的话控件下一帧
     * 会读到已释放的内存。页面对象由 PageManager 惰性建出后永不销毁，所以这几
     * MB 必须在离页时还掉，否则常驻到进程结束。 */
    if(g_img) {
        lv_image_set_src(g_img, nullptr);
        lv_obj_set_style_translate_x(g_img, 0, 0);
    }
    ui::PhotoDecoder::instance().release();

    g_photos.clear();
    g_photos.shrink_to_fit();
}

lv_obj_t* page_user_photo_create(lv_obj_t* parent)
{
    (void)parent;
    lv_obj_t* page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page = page;

    /* 主图：CONTAIN（不是播放器那种 COVER）——照片是竖构图，COVER 会把上下切掉一半 */
    lv_obj_t* img = lv_image_create(page);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, img_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(img, img_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(img, img_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(img, img_release_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(img, img_release_cb, LV_EVENT_PRESS_LOST, nullptr);
    g_img = img;

    /* 提示挂页根而非控件层：加载中/无照片这类信息不该被单击一并收起 */
    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, "");
    lv_obj_set_style_text_color(hint, user_color_text_dim, 0);
    lv_obj_set_style_text_font(hint, user_font_text(), 0);
    lv_obj_center(hint);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    g_hint_lbl = hint;

    lv_obj_t* ov = lv_obj_create(page);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    /* 不可点：空白处的点击穿透到主图去切控件层显隐 */
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
    lv_label_set_text(tl, "Album");
    lv_obj_set_style_text_color(tl, user_color_text, 0);
    lv_obj_set_style_text_font(tl, user_font_text(), 0);
    /* 文件名可能很长，夹住并锁成一行才出得来省略号（与历史页卡片同理） */
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(tl, LV_PCT(60), 0);
    lv_obj_set_height(tl, lv_font_get_line_height(user_font_text()));
    g_title_lbl = tl;

    lv_obj_t* sub = lv_label_create(title_box);
    lv_label_set_text(sub, "");
    lv_obj_set_style_text_color(sub, user_color_text_dim, 0);
    lv_obj_set_style_text_font(sub, user_font_caption(), 0);
    g_sub_lbl = sub;

    const int32_t btn_sz = USER_BACK_BTN_H;
    lv_obj_t* close = ui_user_make_overlay_round_btn(ov, LV_SYMBOL_CLOSE, btn_sz,
                                                     on_close_click, nullptr);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);

    /* 两侧箭头正落在竖构图照片的黑边上，不压画面 */
    g_prev_btn = ui_user_make_overlay_round_btn(ov, LV_SYMBOL_LEFT, btn_sz,
                                                on_prev_click, nullptr);
    lv_obj_align(g_prev_btn, LV_ALIGN_LEFT_MID, USER_PAD, 0);

    g_next_btn = ui_user_make_overlay_round_btn(ov, LV_SYMBOL_RIGHT, btn_sz,
                                                on_next_click, nullptr);
    lv_obj_align(g_next_btn, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);

    ui::PhotoDecoder::instance().set_ready_cb(on_photo_ready);

    return page;
}
