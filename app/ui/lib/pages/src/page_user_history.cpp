#include "page_user_history.hpp"
#include "page_user_timelapse.hpp"
#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "print_history_manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @file page_user_history.cpp
 * @brief 打印历史页（对齐设计稿 2956-4436），紧凑版适配 1024×600。
 *
 * 布局：
 *   ┌──────────────────────────────────────────────────────┐
 *   │ [WiFi] SNOWPOD A1 ● Ready                  (header)    │
 *   ├────────────────────────────────────────────────┬──────┤
 *   │ 22:13  May 26, 2026                             │  ▲   │
 *   │ ┌────────────────────────────────┬───────────┐  │      │
 *   │ │ nekoo_0_2026_06_23  ✓ COMPLETE │ [缩略图 ▶]│  │      │
 *   │ │ ─────────────────────────────  │           │  │      │
 *   │ │ 🕐 TIME COST   🗄 CONSUMABLES   │           │  │  ▼   │
 *   │ │    1:28           —            │           │  │      │
 *   │ └────────────────────────────────┴───────────┘  │      │
 *   │ ...（纵向滚动）                                   │      │
 *   └──────────────────────────────────────────────────┴──────┘
 *
 * 数据源：/home/video/manifest.json（common/fs_manifest/print_history_manifest）。
 * 缩略图：/home/video/{thumb}（.rthm 原始 RGB565，自定义 decoder），
 *          cover 居中裁切（竖构图相机图铺满横槽）。
 * 视频播放（第二期）：点缩略图进入播放器隐藏子页，当前为占位。
 */

namespace {

/* LVGL POSIX FS 盘符（lv_conf: LV_FS_POSIX_LETTER 'A'） */
constexpr const char *kThumbDrive = "A:";

lv_obj_t *g_page        = nullptr;  /**< 页根：翻页悬浮层挂此（整屏定位） */
lv_obj_t *g_list        = nullptr;  /**< 历史卡片纵向滚动容器 */
lv_obj_t *g_scroll_btns = nullptr;  /**< 右侧上下翻页悬浮层 */
lv_obj_t *g_empty       = nullptr;  /**< 空列表占位 */

/** 每张卡片的运行时上下文（删除动作 + 左滑状态） */
struct HistoryCard {
    std::string frames_dir;         /**< manifest 相对目录名（删除主键） */
    std::string thumb;
    std::string thumb_src;          /**< LVGL FS 路径，如 A:/home/video/x/thumb.rthm */
    std::string title;              /**< 播放器标题（作业名） */
    std::string subtitle;          /**< 播放器副标题（时间 日期） */
    lv_obj_t   *wrap    = nullptr;  /**< 整条容器（删除时删它） */
    lv_obj_t   *card    = nullptr;  /**< 磨砂卡片（左滑平移对象） */
    lv_obj_t   *del_btn = nullptr;  /**< 红色删除圆钮 */
    bool        open    = false;    /**< 左滑展开态 */
};

std::vector<HistoryCard *> g_cards;

/* 前向声明（定义顺序晚于调用点） */
lv_obj_t *make_card(lv_obj_t *parent);

/* ---- 格式化 ---- */

/** start_unix → "HH:MM"（24 小时制）+ "Mon DD, YYYY" */
void format_time_date(int64_t unix_sec, char *time_buf, size_t tn,
                      char *date_buf, size_t dn)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(time_buf, tn, "%H:%M", &tm);
    std::strftime(date_buf, dn, "%b %d, %Y", &tm);
}

/** duration_sec → "H:MM:SS"（≥1h）或 "M:SS" */
void format_duration(uint32_t sec, char *buf, size_t n)
{
    const uint32_t h = sec / 3600u;
    const uint32_t m = (sec % 3600u) / 60u;
    const uint32_t s = sec % 60u;
    if(h > 0) {
        std::snprintf(buf, n, "%u:%02u:%02u", h, m, s);
    } else {
        std::snprintf(buf, n, "%u:%02u", m, s);
    }
}

/** material_g → "X.XX G"；0（切片查不到）显示 "--"（ASCII，避免字库缺字） */
void format_material(float g, char *buf, size_t n)
{
    if(g > 0.01f) {
        std::snprintf(buf, n, "%.2f G", static_cast<double>(g));
    } else {
        std::snprintf(buf, n, "%s", "--");
    }
}

/* ---- 左滑删除 ---- */

int32_t swipe_reveal_px(void)
{
    return USER_BTN_PRIMARY_H + USER_PAD * 2;
}

void close_card(HistoryCard *c)
{
    if(!c || !c->open) {
        return;
    }
    c->open = false;
    if(c->card) {
        lv_obj_set_style_translate_x(c->card, 0, 0);
    }
    if(c->del_btn) {
        lv_obj_add_flag(c->del_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void reset_all_cards(void)
{
    for(HistoryCard *c : g_cards) {
        close_card(c);
    }
}

void open_card(HistoryCard *c)
{
    if(!c || c->open) {
        return;
    }
    reset_all_cards(); /* 仅允许一张展开 */
    c->open = true;
    if(c->card) {
        lv_obj_set_style_translate_x(c->card, -swipe_reveal_px(), 0);
    }
    if(c->del_btn) {
        lv_obj_remove_flag(c->del_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void card_gesture_cb(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if(dir == LV_DIR_LEFT) {
        open_card(c);
    } else if(dir == LV_DIR_RIGHT) {
        close_card(c);
    }
}

/**
 * 删除单张卡片（延迟到事件处理之外执行，避免删中卡片时 UAF）。
 * 只移除该条容器，不整表重建：避免闪屏，也不会在原位重建可点击缩略图导致误触发播放。
 */
void delete_card_async(void *user)
{
    HistoryCard *c = static_cast<HistoryCard *>(user);
    if(!c) {
        return;
    }
    if(c->wrap) {
        lv_obj_del(c->wrap);  /* 删整条容器（含 card / del / thumbnail 等子对象） */
    }
    auto it = std::find(g_cards.begin(), g_cards.end(), c);
    if(it != g_cards.end()) {
        g_cards.erase(it);
    }
    delete c;

    /* 删空后显示占位、隐藏列表与翻页钮 */
    if(g_cards.empty()) {
        if(g_empty)       lv_obj_remove_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
        if(g_list)        lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);
        if(g_scroll_btns) lv_obj_add_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
    }
}

void del_click_cb(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(!c || c->frames_dir.empty()) {
        return;
    }

    const std::string dir = UPC_PRINT_VIDEO_DIR;
    std::error_code ec;
    (void)std::filesystem::remove_all(dir + "/" + c->frames_dir, ec);

    /* 从 manifest 摘除该条并原子重写（tmp + rename） */
    std::vector<upc::PrintHistoryEntry> entries;
    if(upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        const std::string target = c->frames_dir;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                [&](const upc::PrintHistoryEntry &en) {
                    return en.frames_dir == target;
                }),
            entries.end());
        const std::string tmp = std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
        if(upc::print_history_manifest_save(tmp, entries)) {
            (void)std::rename(tmp.c_str(), UPC_PRINT_VIDEO_MANIFEST);
        }
    }

    ui_toast_show("Deleted", user_color_accent, 1200);
    lv_async_call(delete_card_async, c);
}

/* ---- 缩略图点击 → 延时摄影播放器 ---- */
void on_thumb_click(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(!c || c->frames_dir.empty()) {
        return;
    }
    char path[256];
    std::snprintf(path, sizeof(path), "%s/%s",
                  UPC_PRINT_VIDEO_DIR, c->frames_dir.c_str());
    page_user_timelapse_set_source(path, c->title.c_str(), c->subtitle.c_str());
    ui::PageManager::instance().switch_to("user_timelapse");
}

/* ---- 小构件 ---- */

/** 右侧竖排圆形翻页按钮（与设置页一致） */
lv_obj_t *make_scroll_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    const int32_t sz = USER_BACK_BTN_H * 5 / 4;
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, user_color_text, 0);
    lv_obj_set_style_text_font(l, user_font_text(), 0);
    lv_obj_center(l);
    return btn;
}

void on_scroll_up(lv_event_t *)
{
    if(g_list) {
        lv_obj_scroll_by_bounded(g_list, 0, lv_obj_get_height(g_list) * 7 / 10,
                                 LV_ANIM_ON);
    }
}

void on_scroll_down(lv_event_t *)
{
    if(g_list) {
        lv_obj_scroll_by_bounded(g_list, 0, -lv_obj_get_height(g_list) * 7 / 10,
                                 LV_ANIM_ON);
    }
}

/** 状态徽标：完成=绿✓ / 终止=橙⚠ */
void make_status_badge(lv_obj_t *parent, bool complete)
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

    const lv_color_t col = complete ? user_color_status_ok : lv_color_hex(0xFF9800);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, complete ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(icon, col, 0);
    lv_obj_set_style_text_font(icon, user_font_caption(), 0);

    lv_obj_t *txt = lv_label_create(row);
    lv_label_set_text(txt, complete ? "PRINT COMPLETE" : "PRINT TERMINATED");
    lv_obj_set_style_text_color(txt, col, 0);
    lv_obj_set_style_text_font(txt, user_font_caption(), 0);
}

/** 单个指标块：图标+标签（caption）一行 + 大数值 */
void make_metric(lv_obj_t *parent, const char *icon_sym, const char *caption,
                 const char *value)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap_row = lv_obj_create(col);
    lv_obj_remove_style_all(cap_row);
    lv_obj_set_size(cap_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cap_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cap_row, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(cap_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cap_row, LV_OBJ_FLAG_SCROLLABLE);

    /* 图标可选：LVGL 默认无"时钟"符号，无合适图标时传 nullptr 跳过，
     * 后续可像 settings_icons 那样补自定义 A8 资源。 */
    if(icon_sym && icon_sym[0]) {
        lv_obj_t *icon = lv_label_create(cap_row);
        lv_label_set_text(icon, icon_sym);
        lv_obj_set_style_text_color(icon, user_color_text_dim, 0);
        lv_obj_set_style_text_font(icon, user_font_caption(), 0);
    }

    lv_obj_t *cap = lv_label_create(cap_row);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *val = lv_label_create(col);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_value(), 0);
}

/** 缩略图槽：图片（cover 裁切）+ 中央播放钮 + "延时摄影"角标 */
void make_thumbnail(lv_obj_t *parent, const HistoryCard *c)
{
    const int32_t th = USER_FILE_THUMB_H;
    const int32_t tw = th * 3 / 2; /* 横向槽（竖图 cover 裁切） */

    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, tw, th);
    /* 直角 + 不裁圆角：clip_corner 每帧建掩膜层，是列表滚动卡顿主因，去掉提速 */
    lv_obj_set_style_radius(slot, 0, 0);
    lv_obj_set_style_bg_color(slot, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, on_thumb_click, LV_EVENT_CLICKED,
                        const_cast<HistoryCard *>(c));

    if(!c->thumb.empty()) {
        lv_obj_t *img = lv_image_create(slot);
        lv_image_set_src(img, c->thumb_src.c_str());
        lv_obj_set_size(img, tw, th);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        /* cover：等比铺满槽，多出部分居中裁切（与相机预览同方案） */
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    }

    /* 中央半透明播放钮 */
    const int32_t play_sz = th * 2 / 5;
    lv_obj_t *play = lv_obj_create(slot);
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, play_sz, play_sz);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(play, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(play, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *play_icon = lv_label_create(play);
    lv_label_set_text(play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(play_icon, user_color_text, 0);
    lv_obj_set_style_text_font(play_icon, user_font_caption(), 0);
    lv_obj_center(play_icon);

    /* 左上角标 "延时摄影"（英文 Timelapse，避免依赖 CJK 字体） */
    lv_obj_t *tag = lv_label_create(slot);
    lv_label_set_text(tag, "Timelapse");
    lv_obj_set_style_text_color(tag, user_color_text, 0);
    lv_obj_set_style_text_font(tag, user_font_caption(), 0);
    lv_obj_set_style_bg_color(tag, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(tag, USER_PAD_SM / 2, 0);
    lv_obj_set_style_radius(tag, USER_RADIUS / 2, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, USER_PAD_SM / 2, USER_PAD_SM / 2);
}

/** 一条历史记录：时间/日期行 + 磨砂卡片 + 左滑删除钮 */
void build_entry(lv_obj_t *parent, const upc::PrintHistoryEntry &e)
{
    HistoryCard *ctx = new HistoryCard();
    ctx->frames_dir = e.frames_dir;
    ctx->thumb = e.thumb;
    if(!e.thumb.empty()) {
        ctx->thumb_src = std::string(kThumbDrive) + UPC_PRINT_VIDEO_DIR + "/" + e.thumb;
    }
    g_cards.push_back(ctx);

    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    ctx->wrap = wrap;

    /* 时间 / 日期行（卡片上方） */
    lv_obj_t *time_row = lv_obj_create(wrap);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(time_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);

    char time_buf[16], date_buf[32], dur_buf[16], mat_buf[24];
    format_time_date(e.start_unix, time_buf, sizeof(time_buf),
                     date_buf, sizeof(date_buf));
    format_duration(e.duration_sec, dur_buf, sizeof(dur_buf));
    format_material(e.material_g, mat_buf, sizeof(mat_buf));

    /* 播放器标题/副标题 */
    ctx->title    = e.job_name.empty() ? "Timelapse" : e.job_name;
    ctx->subtitle = std::string(time_buf) + "  " + date_buf;

    lv_obj_t *time_lbl = lv_label_create(time_row);
    lv_label_set_text(time_lbl, time_buf);
    lv_obj_set_style_text_color(time_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(time_lbl, user_font_value(), 0);

    lv_obj_t *date_lbl = lv_label_create(time_row);
    lv_label_set_text(date_lbl, date_buf);
    lv_obj_set_style_text_color(date_lbl, user_color_text_dim, 0);
    lv_obj_set_style_text_font(date_lbl, user_font_caption(), 0);

    /* 删除钮：悬浮在 wrap 右侧、卡片下层，默认隐藏，左滑时露出 */
    lv_obj_t *del = lv_button_create(wrap);
    lv_obj_remove_style_all(del);
    const int32_t del_sz = USER_BTN_PRIMARY_H;
    lv_obj_set_size(del, del_sz, del_sz);
    lv_obj_add_flag(del, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(del, user_color_status_err, 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -USER_PAD, USER_PAD_SM * 2);
    lv_obj_add_flag(del, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(del, del_click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *del_icon = lv_label_create(del);
    lv_label_set_text(del_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(del_icon, user_color_text, 0);
    lv_obj_set_style_text_font(del_icon, user_font_text(), 0);
    lv_obj_center(del_icon);
    ctx->del_btn = del;

    /* 磨砂卡片：左信息栏 + 右缩略图 */
    lv_obj_t *card = make_card(wrap);
    ctx->card = card;
    /* 清除卡片默认的 GESTURE_BUBBLE：LVGL 基础对象默认带此标志，会把手势事件
     * 一路上抛越过卡片；清掉后手势遍历停在卡片，card_gesture_cb 才能收到左滑。 */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(card, card_gesture_cb, LV_EVENT_GESTURE, ctx);
    /* 清除卡片默认的 GESTURE_BUBBLE：LVGL 手势会沿带此标志的对象一路上抛，
     * 基础对象默认都带（lv_obj.c 构造），不清除则手势越过卡片抛到列表，
     * card_gesture_cb 收不到。清除后遍历停在卡片 → 左滑手势可达。 */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *info = lv_obj_create(card);
    lv_obj_remove_style_all(info);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶行：文件名 + 状态徽标 */
    lv_obj_t *top = lv_obj_create(info);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(top);
    lv_label_set_text(name, e.job_name.empty() ? "print" : e.job_name.c_str());
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);

    make_status_badge(top, e.status == upc::PrintHistoryStatus::Complete);

    ui_user_make_divider_line(info, LV_PCT(100));

    /* 指标行：TIME COST + CONSUMABLES */
    lv_obj_t *metrics = lv_obj_create(info);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_width(metrics, LV_PCT(100));
    lv_obj_set_height(metrics, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(metrics, USER_PAD, 0);
    lv_obj_set_style_bg_opa(metrics, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);

    make_metric(metrics, nullptr, "TIME COST", dur_buf);
    make_metric(metrics, nullptr, "CONSUMABLES", mat_buf);

    make_thumbnail(card, ctx);
}

/** 磨砂卡片（行布局） */
lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    /* 历史卡改不透明背景：半透明每帧要与背景混合，滚动时开销大。仅本页覆盖，
     * 不改共享组件（设置页等仍用磨砂半透明）。 */
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, USER_PAD, 0);
    return card;
}

/* ---- 顶栏 ---- */
lv_obj_t *build_header(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_TOPBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_column(bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wifi = lv_label_create(bar);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi, user_color_text, 0);
    lv_obj_set_style_text_font(wifi, user_font_text(), 0);

    lv_obj_t *name = lv_label_create(bar);
    lv_label_set_text(name, "SNOWPOD A1");
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);

    lv_obj_t *dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, USER_STATUS_DOT_SZ, USER_STATUS_DOT_SZ);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, user_color_status_ok, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(dot, USER_PAD_SM, 0);

    lv_obj_t *status = lv_label_create(bar);
    lv_label_set_text(status, "Ready");
    lv_obj_set_style_text_color(status, user_color_status_ok, 0);
    lv_obj_set_style_text_font(status, user_font_text(), 0);
    return bar;
}

void clear_cards(void)
{
    for(HistoryCard *c : g_cards) {
        delete c;
    }
    g_cards.clear();
}

} /* namespace */

void page_user_history_refresh(bool /*force*/)
{
    if(!g_list) {
        return;
    }

    lv_obj_clean(g_list);
    clear_cards();

    std::vector<upc::PrintHistoryEntry> entries;
    (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);

    /* 时间倒序（最新在上） */
    std::sort(entries.begin(), entries.end(),
        [](const upc::PrintHistoryEntry &a, const upc::PrintHistoryEntry &b) {
            return a.start_unix > b.start_unix;
        });

    if(entries.empty()) {
        if(g_empty)        lv_obj_remove_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
        if(g_list)         lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);
        if(g_scroll_btns)  lv_obj_add_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if(g_empty)       lv_obj_add_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
    if(g_list)        lv_obj_remove_flag(g_list, LV_OBJ_FLAG_HIDDEN);
    if(g_scroll_btns) lv_obj_remove_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);

    for(const upc::PrintHistoryEntry &e : entries) {
        build_entry(g_list, e);
    }
}

void page_user_history_exit(void)
{
    reset_all_cards();
}

lv_obj_t *page_user_history_create(lv_obj_t *parent)
{
    g_page        = nullptr;
    g_list        = nullptr;
    g_scroll_btns = nullptr;
    g_empty       = nullptr;
    clear_cards();

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page = page;

    build_header(page);

    /* 列表容器（纵向滚动） */
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(list, USER_PAD, 0);
    lv_obj_set_style_pad_row(list, USER_PAD, 0);
    /* 给右侧悬浮翻页钮留位 */
    lv_obj_set_style_pad_right(list, USER_BACK_BTN_H * 5 / 4 + USER_PAD * 2, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    /* 恢复边界回弹，保留惯性关闭以控制滚动抖动。 */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    g_list = list;

    /* 空列表占位 */
    lv_obj_t *empty = lv_label_create(page);
    lv_label_set_text(empty, "No print history yet");
    lv_obj_set_style_text_color(empty, user_color_text_dim, 0);
    lv_obj_set_style_text_font(empty, user_font_text(), 0);
    lv_obj_add_flag(empty, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(empty, LV_OBJ_FLAG_HIDDEN);
    g_empty = empty;

    /* 右侧竖排翻页悬浮层（整屏右侧居中） */
    lv_obj_t *col = lv_obj_create(page);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(col, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(col, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);
    make_scroll_btn(col, LV_SYMBOL_UP, on_scroll_up);
    make_scroll_btn(col, LV_SYMBOL_DOWN, on_scroll_down);
    g_scroll_btns = col;

    page_user_history_refresh(true);
    return page;
}
