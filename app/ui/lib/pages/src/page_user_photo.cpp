#include "page_user_photo.hpp"

#include "album_index.hpp"
#include "page_manager.hpp"
#include "photo_decoder.hpp"
#include "raw_thumb_format.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
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
 *   │          ▭ ▭ ▭ ▣ ▭ ▭ ▭  ← filmstrip（当前格描边）      │
 *   └──────────────────────────────────────────────────────┘
 *
 * 翻页途径等价，都走 show_index()：左右滑动、两侧箭头、点 filmstrip 某格；
 * 滑动与箭头首末循环回绕。单击主图切换控件层显隐（filmstrip 一并收起）。
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

/** 翻页判定之一：横向位移超过屏宽的 1/kSwipeCommitDiv 就换图 */
constexpr int32_t kSwipeCommitDiv = 5;

/**
 * 翻页判定之二：轻扫（flick）。
 * 快速短扫是最自然的手势，只看距离的话必须老老实实拖过 1/5 屏才翻页，手感发滞。
 */
constexpr int32_t kFlickPxPerSec = 600;
/** flick 的最小位移，挡住原地手抖被当成轻扫 */
constexpr int32_t kFlickMinPx = 24;

/** commit / 弹回的滑动动画时长 */
constexpr uint32_t kSlideMs = 180;

/** 缩略图槽底色（与历史页卡片缩略图同色，稿 4435:6119 的 #202020） */
constexpr uint32_t kColorThumbBg = 0x202020;

/** 单张 `.rthm` 的体量上限：正常约 136KB（240 宽封顶），留足余量挡坏文件 */
constexpr uint32_t kMaxThumbBytes = 1u * 1024u * 1024u;

/** filmstrip 格数上下限（实际按屏宽算，并强制取奇数好让当前格居中） */
constexpr size_t kStripMinCells = 3;
constexpr size_t kStripMaxCells = 15;

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
lv_obj_t* g_strip = nullptr;     /**< filmstrip 容器 */

/**
 * filmstrip 的一格。
 *
 * `.rthm` 像素**常驻内存**而不是让 lv_image 指文件路径：本工程
 * LV_CACHE_DEF_SIZE = 0（图片缓存关闭），指路径的话每帧重绘都要重新
 * 「读 136KB 文件 + lv_draw_buf_create」，横向一排格子同时重绘就是几十 MB/s 的
 * 无谓 IO。读进内存后滚动/重绘零 IO，只剩 LVGL 那点缩放。
 */
struct StripCell {
    lv_obj_t* box = nullptr;   /**< 格子：点击热区 + 选中描边 */
    lv_obj_t* img = nullptr;
    size_t index = ui::album::kInvalidIndex; /**< 当前绑定的照片下标 */
    std::vector<uint8_t> data;               /**< RGB565 原始像素 */
    lv_image_dsc_t dsc {};
};

/**
 * 固定数量的格子，建一次后只换绑定（回收），不随照片数增长。
 * 用 unique_ptr 是因为 lv_image 持有 `&dsc` 的裸地址，vector 扩容搬家会让它悬空。
 */
std::vector<std::unique_ptr<StripCell>> g_cells;

bool g_controls_visible = true;

/* 跟手滑动的按压期状态，每次 PRESSED 重置 */
int32_t g_press_x = 0;
int32_t g_press_y = 0;
uint32_t g_press_tick = 0;  /**< 按下时刻，用于算轻扫速度 */
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

void anim_shift_exec(void* obj, int32_t v)
{
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), v, 0);
}

/** 停掉在途的位移动画：跟手拖动、换图、离页都要先把它掐掉，否则会与新位移互相拉扯 */
void stop_shift_anim(void)
{
    if(g_img) {
        (void)lv_anim_delete(g_img, anim_shift_exec);
    }
}

void reset_img_shift(void)
{
    stop_shift_anim();
    if(g_img) {
        lv_obj_set_style_translate_x(g_img, 0, 0);
    }
}

/** 位移滑到目标值（ease-out）：翻页后新图滑入、以及不够阈值时的弹回都用它 */
void animate_shift_to(int32_t from, int32_t to)
{
    if(!g_img) {
        return;
    }
    stop_shift_anim();
    lv_obj_set_style_translate_x(g_img, from, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_img);
    lv_anim_set_exec_cb(&a, anim_shift_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, kSlideMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ============================================================================
 *  filmstrip
 * ========================================================================= */

/** 格子几何（无设计稿）：沿用历史页缩略图的 8:5，高取其卡高的 2/5 */
int32_t strip_cell_h(void) { return USER_HIST_CARD_H * 2 / 5; }
int32_t strip_cell_w(void) { return strip_cell_h() * 8 / 5; }
int32_t strip_gap(void)    { return USER_PAD_SM; }

/** 读 `.rthm` 到内存（格式见 common/fs_manifest/raw_thumb_format.hpp） */
bool read_rthm(const std::string& path, std::vector<uint8_t>& out,
               int32_t& w, int32_t& h, uint32_t& stride)
{
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if(!fp) {
        return false;
    }
    upc::RawThumbHeader hdr {};
    const bool head_ok =
        std::fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr) &&
        upc::raw_thumb_header_valid(hdr) &&
        hdr.data_size <= kMaxThumbBytes;
    if(!head_ok) {
        std::fclose(fp);
        return false;
    }
    out.resize(hdr.data_size);
    const size_t got = std::fread(out.data(), 1, hdr.data_size, fp);
    std::fclose(fp);
    if(got != hdr.data_size) {
        out.clear();
        return false;
    }
    w = static_cast<int32_t>(hdr.width);
    h = static_cast<int32_t>(hdr.height);
    stride = hdr.stride_bytes;
    return true;
}

/** 把某格绑到某张照片；无 `.rthm`（非 PNG）就留纯色底 */
void bind_cell(StripCell& c, const ui::album::PhotoEntry& e)
{
    /* 先断开引用再动缓冲：顺序反了 LVGL 下一帧会读到已释放内存 */
    if(c.img) {
        lv_image_set_src(c.img, nullptr);
    }
    c.data.clear();
    std::memset(&c.dsc, 0, sizeof(c.dsc));

    int32_t w = 0;
    int32_t h = 0;
    uint32_t stride = 0;
    if(e.thumb.empty() || !read_rthm(e.thumb, c.data, w, h, stride)) {
        c.data.clear();
        c.data.shrink_to_fit();
        return;
    }
    c.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    c.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    c.dsc.header.w = w;
    c.dsc.header.h = h;
    c.dsc.header.stride = stride;
    c.dsc.data = c.data.data();
    c.dsc.data_size = c.data.size();
    if(c.img) {
        lv_image_set_src(c.img, &c.dsc);
    }
}

void set_cell_selected(StripCell& c, bool sel)
{
    if(!c.box) {
        return;
    }
    lv_obj_set_style_border_width(c.box, sel ? 2 : 0, 0);
    lv_obj_set_style_border_color(c.box, user_color_accent, 0);
    lv_obj_set_style_border_opa(c.box, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/**
 * 把窗口挪到以 center 为中心，并刷新各格绑定。
 *
 * 窗口在两端**夹住而不回绕**（虽然翻页本身回绕）：在列表头把末尾几张拼到左边会
 * 看着像跳接，而此刻用户并不知道自己已经在头上。
 */
void update_strip(size_t center)
{
    const size_t n = g_photos.size();
    if(!g_strip || g_cells.empty()) {
        return;
    }
    /* 只有一张（或没有）时整条藏掉：一格的 filmstrip 没有意义 */
    if(n <= 1u) {
        lv_obj_add_flag(g_strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(g_strip, LV_OBJ_FLAG_HIDDEN);

    const size_t count = g_cells.size();
    size_t start = 0;
    if(n > count) {
        const size_t half = count / 2u;
        start = (center > half) ? (center - half) : 0u;
        if(start + count > n) {
            start = n - count;
        }
    }

    for(size_t i = 0; i < count; ++i) {
        StripCell& c = *g_cells[i];
        const size_t idx = start + i;
        if(idx >= n) {
            /* 照片比格子少：多出来的藏掉（flex 会跳过隐藏对象，整条自动变短） */
            if(c.box) {
                lv_obj_add_flag(c.box, LV_OBJ_FLAG_HIDDEN);
            }
            if(c.index != ui::album::kInvalidIndex) {
                bind_cell(c, ui::album::PhotoEntry {});
                c.index = ui::album::kInvalidIndex;
            }
            continue;
        }
        if(c.box) {
            lv_obj_remove_flag(c.box, LV_OBJ_FLAG_HIDDEN);
        }
        if(c.index != idx) {
            c.index = idx;
            bind_cell(c, g_photos[idx]); /* 只在绑定变了才重读，这就是回收的收益 */
        }
        set_cell_selected(c, idx == center);
    }
}

/** 离页时把 filmstrip 占的内存还掉（页面对象永不销毁，不还就常驻） */
void release_strip(void)
{
    for(auto& up : g_cells) {
        StripCell& c = *up;
        if(c.img) {
            lv_image_set_src(c.img, nullptr);
        }
        c.data.clear();
        c.data.shrink_to_fit();
        std::memset(&c.dsc, 0, sizeof(c.dsc));
        c.index = ui::album::kInvalidIndex;
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

    update_strip(idx);
}

/** filmstrip 某格被点：跳到那张。格子自己消费点击，不会冒到主图去切控件层。 */
void strip_cell_click_cb(lv_event_t* e)
{
    auto* c = static_cast<StripCell*>(lv_event_get_user_data(e));
    if(!c || c->index == ui::album::kInvalidIndex || c->index == g_index) {
        return;
    }
    /* 滑入方向跟随被点的格子在条上的左右关系（g_index 会被 show_index 改，先取） */
    const bool forward = (c->index > g_index);
    show_index(c->index);
    const int32_t w = g_page ? lv_obj_get_width(g_page) : 0;
    if(w > 0) {
        animate_shift_to(forward ? w : -w, 0);
    }
}

/**
 * 相对翻页（回绕）：滑动、箭头两条途径都汇到这里。
 * @param animate 让新图从手指离开的方向滑入；箭头点击也用它做反馈
 */
void show_relative(int delta, bool animate)
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

    if(animate) {
        /* 只有一个 lv_image，做不出「旧图滑出 + 新图滑入」的双图轮播；
         * 退一步只做滑入——方向跟手，观感已经连贯，且不必多养一份解码缓冲。 */
        const int32_t w = g_page ? lv_obj_get_width(g_page) : 0;
        if(w > 0) {
            animate_shift_to(delta > 0 ? w : -w, 0);
        }
    }
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
    g_press_tick = lv_tick_get();
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
        stop_shift_anim(); /* 上一次翻页的滑入动画可能还在跑，先掐掉再跟手 */
    }
    /* 扣掉起拖阈值，让位移从 0 开始长：直接用 dx 的话，判定成立那一帧画面会突然
     * 跳 kSwipeStartPx。 */
    const int32_t shift = (dx > 0) ? (dx - kSwipeStartPx) : (dx + kSwipeStartPx);
    lv_obj_set_style_translate_x(g_img, shift, 0);
}

/**
 * 松手（或按压被抢走）：位移够远**或**扫得够快就翻页，否则滑回原位。
 *
 * 两个判据缺一不可：只看距离的话快速短扫会被判成没动（手感发滞），只看速度的话
 * 慢慢拖过半屏反而翻不了页。
 *
 * 速度取的是「按下到松手」的平均值，不是松手瞬时速度——轻扫本来就是一气呵成的
 * 短手势，两者接近；要真做瞬时速度得留一小段位置历史，不值当。
 */
void img_release_cb(lv_event_t* /*e*/)
{
    if(!g_dragging) {
        return;
    }
    g_dragging = false;
    g_just_dragged = true;

    const int32_t shift = g_img
                              ? lv_obj_get_style_translate_x(g_img, LV_PART_MAIN)
                              : 0;
    const int32_t abs_shift = LV_ABS(shift);
    const int32_t w = g_page ? lv_obj_get_width(g_page) : 0;
    const int32_t dist_commit = (w > 0) ? (w / kSwipeCommitDiv) : 0;

    const uint32_t elapsed_ms = lv_tick_elaps(g_press_tick);
    const int32_t speed_px_s =
        (elapsed_ms > 0)
            ? static_cast<int32_t>(static_cast<int64_t>(abs_shift) * 1000 /
                                   elapsed_ms)
            : 0;

    const bool by_dist = (dist_commit > 0 && abs_shift >= dist_commit);
    const bool by_flick =
        (abs_shift >= kFlickMinPx && speed_px_s >= kFlickPxPerSec);

    if(shift != 0 && (by_dist || by_flick)) {
        show_relative(shift < 0 ? 1 : -1, true);
    } else {
        animate_shift_to(shift, 0); /* 不够阈值：滑回去，不要瞬间归位 */
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
    show_relative(-1, true);
}

void on_next_click(lv_event_t* /*e*/)
{
    show_relative(1, true);
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
        release_strip();
        if(g_strip) {
            lv_obj_add_flag(g_strip, LV_OBJ_FLAG_HIDDEN);
        }
        set_hint("No photos");
        return;
    }
    /* 列表可能已变（期间删过照片），旧绑定一律作废后重建 */
    release_strip();
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
    reset_img_shift(); /* 顺带停掉在途的滑入动画 */
    if(g_img) {
        lv_image_set_src(g_img, nullptr);
    }
    ui::PhotoDecoder::instance().release();
    release_strip();

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

    /* ---- filmstrip：格数按屏宽算（主题分三档），建一次后只换绑定 ---- */
    const int32_t cell_w = strip_cell_w();
    const int32_t cell_h = strip_cell_h();
    const int32_t gap = strip_gap();
    int32_t screen_w = 0;
    int32_t screen_h = 0;
    display_box(screen_w, screen_h);
    size_t cell_count =
        static_cast<size_t>(std::max<int32_t>(1, (screen_w * 4 / 5) / (cell_w + gap)));
    if(cell_count % 2u == 0u) {
        --cell_count; /* 取奇数，当前格才落在正中 */
    }
    cell_count = std::clamp(cell_count, kStripMinCells, kStripMaxCells);

    lv_obj_t* strip = lv_obj_create(ov);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(strip, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(strip, gap, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_50, 0);
    lv_obj_set_style_radius(strip, USER_RADIUS / 2, 0);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* 条本身不可点：格子之间的缝隙点下去穿透到主图切控件层 */
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, -USER_PAD);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    g_strip = strip;

    g_cells.clear();
    g_cells.reserve(cell_count);
    for(size_t i = 0; i < cell_count; ++i) {
        auto cell = std::make_unique<StripCell>();

        lv_obj_t* box = lv_obj_create(strip);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, cell_w, cell_h);
        /* 直角 + 不裁圆角：clip_corner 每帧建掩膜层，与历史页缩略图同一取舍 */
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(kColorThumbBg), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        /* 选中描边必须画在缩略图之上：LVGL 默认在 DRAW_MAIN 阶段连底一起画边框，
         * 会被铺满整格的子图片盖掉；border_post 把它挪到子对象之后（lv_obj.c 的
         * LV_EVENT_DRAW_POST 分支）。 */
        lv_obj_set_style_border_post(box, true, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(box, strip_cell_click_cb, LV_EVENT_CLICKED,
                            cell.get());
        cell->box = box;

        lv_obj_t* thumb = lv_image_create(box);
        lv_obj_set_size(thumb, cell_w, cell_h);
        lv_obj_center(thumb);
        lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_COVER);
        /* 点在图上也要算点在格子上 */
        lv_obj_add_flag(thumb, LV_OBJ_FLAG_EVENT_BUBBLE);
        cell->img = thumb;

        g_cells.push_back(std::move(cell));
    }

    ui::PhotoDecoder::instance().set_ready_cb(on_photo_ready);

    return page;
}
