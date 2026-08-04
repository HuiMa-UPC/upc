#include "page_user_history.hpp"
#include "page_user_timelapse.hpp"
#include "page_manager.hpp"
#include "control.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "print_history_manifest.hpp"
#include "msg_port.hpp"
#include "upc_contract.hpp"
#include "home_metric_icons.h"
#include "home_btn_icons.h"

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
 * @brief 打印历史页（对齐设计稿 4366:5400 一套六态），紧凑版适配 1024×600。
 *
 * 六个态与对应稿：
 *   列表常态      4366:5400
 *   管理(多选)态  4470:6682
 *   左滑删除      4470:6970
 *   空态          4531:6938
 *   存储将满      4536:7333
 *   存储已满      4550:8303
 *
 * 布局（顶部那条设备信息由外壳的常驻条 ui_device_bar 画，本页不再自建 header；
 * content_panel_ 的 pad_top 已为它让出位置。Storage 一条是浮层，用负 y 偏移
 * 回到常驻条那一行的右侧，视觉上与设备名同排）：
 *   ┌ ─ ─ 常驻条：wifi 设备名 状态 ─ ─ ─ ─ ─ ─ ─  Storage ▮▮▮▯ 0.7M/200M ┐
 *   │ Print History  Timelapse  Album                          [ Manage ] │
 *   │ ──────────────────────────────────────────────────────────────────  │
 *   │ ⚠ Storage almost full…（≥90% 才出现）                                │
 *   │ May 26, 2026                                              ┌────┐    │
 *   │ ┌──────────┬──────────────────────────────────────────┐   │ ▲  │    │
 *   │ │[Timelapse│ 21:00:13 - 22:03:59                      │   └────┘    │
 *   │ │  缩略图  │ Modular_Gear_Box.3mf      (Completed)     │   ┌────┐    │
 *   │ │       ⤢ │ 🕐 Duration 4h 12m │ 🛢 Filament 89.2g │ ≡ │   │ ▼  │   │
 *   │ └──────────┴──────────────────────────────────────────┘   └────┘    │
 *   │ ...（纵向滚动，按自然日分组）                                          │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * 管理模式：Tab 行右侧换成「N selected + Delete + Select All + Cancel」，每张卡片
 * 左侧插入复选框（flex 会跳过隐藏对象，所以显隐复选框天然带出卡片的缩进）；
 * 期间禁用左滑删除，点卡片任意位置切换勾选。
 *
 * 数据源：/home/media/video/config.json（common/fs_manifest/print_history_manifest）。
 * 缩略图：/home/media/video/{thumb}（.rthm 原始 RGB565，自定义 decoder），
 *          cover 居中裁切（竖构图相机图铺满横槽）。
 *
 * 三处等后端补数据（其余字段 manifest 已齐）：
 *   1. Layers   —— manifest 无层数字段，暂显示 "--"（Camera 侧可由换层 Snap 的
 *                  最大 layer_index 落盘，见 PrintVideoLayerSnapPayload）。
 *   2. Storage  —— 无容量上报接口，用本文件顶部的 kStorageUsedMB/kStorageTotalMB
 *                  占位；接上后端后只需改 storage_used_mb()/storage_total_mb()。
 *   3. Storage 目前仍为占位常量，待后端容量上报接口接入。
 */

namespace {

/* LVGL POSIX FS 盘符（lv_conf: LV_FS_POSIX_LETTER 'A'） */
constexpr const char *kThumbDrive = "A:";
constexpr const char *kPhotoDir   = UPC_PHOTO_DIR;

/* ---- 设计稿里项目 token 没覆盖的两个色 ---- */
/** Completed 胶囊字色（稿 4478:7265）；比 user_color_status_ok 稍暗，是稿里的绿 */
constexpr uint32_t kColorCompleted = 0x37E47B;
/** Interrupted 胶囊字色 + 存储将满横幅色（稿 4536:7433 / 4550:8299） */
constexpr uint32_t kColorWarn      = 0xFF7843;
/** 缩略图槽底色（稿 4435:6119 的 #202020），也是「视频正在生成」占位底 */
constexpr uint32_t kColorThumbBg   = 0x202020;

/* ---- 存储容量：默认占位；进历史页后向 Media 请求真实值并刷新 ---- */
constexpr float kStorageUsedMBFallback  = 0.7f;
constexpr float kStorageTotalMBFallback = 200.0f;
constexpr int   kStorageWarnPct  = 90;

float g_storage_used_mb = kStorageUsedMBFallback;
float g_storage_total_mb = kStorageTotalMBFallback;

float storage_used_mb(void)  { return g_storage_used_mb; }
float storage_total_mb(void) { return g_storage_total_mb; }

/* ---- Tab ---- */
enum class HistTab : uint8_t {
    History   = 0,   /**< 打印录像 */
    Timelapse = 1,   /**< 延时摄影 */
    Album     = 2,   /**< 相册 */
};
constexpr int kTabCount = 3;
constexpr HistTab kTabOrder[kTabCount] = {
    HistTab::Timelapse,
    HistTab::History,
    HistTab::Album,
};
constexpr const char *kTabTitles[kTabCount] = { "Timelapse", "Print History", "Album" };

/** 一个 Tab 项：文字 + 下方主色下划线（未选中时下划线隐藏） */
struct TabItem {
    lv_obj_t *root  = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *mark  = nullptr;
};

lv_obj_t *g_page        = nullptr;  /**< 页根：翻页/空态/Storage 悬浮层挂此 */
lv_obj_t *g_list        = nullptr;  /**< 历史卡片纵向滚动容器 */
lv_obj_t *g_scroll_btns = nullptr;  /**< 右侧上下翻页悬浮层 */
lv_obj_t *g_empty       = nullptr;  /**< 空列表占位（图标 + 双行文案 + 按钮） */
lv_obj_t *g_empty_title = nullptr;
lv_obj_t *g_empty_sub   = nullptr;
lv_obj_t *g_empty_btn   = nullptr;

TabItem   g_tabs[kTabCount];
lv_obj_t *g_btn_manage   = nullptr; /**< 常态右侧「Manage」 */
lv_obj_t *g_manage_bar   = nullptr; /**< 管理态右侧「N selected + 三钮」 */
lv_obj_t *g_lbl_sel      = nullptr; /**< 「N selected」 */
lv_obj_t *g_btn_del      = nullptr; /**< 管理态「Delete」 */

lv_obj_t *g_storage_bar  = nullptr; /**< 顶部 Storage 浮层（整条） */
lv_obj_t *g_storage_fill = nullptr; /**< 进度条填充块（宽度按占比） */
lv_obj_t *g_storage_lbl  = nullptr; /**< 「0.7M/200M」 */
lv_obj_t *g_banner       = nullptr; /**< 存储告警横幅 */
lv_obj_t *g_banner_lbl   = nullptr;
lv_obj_t *g_banner_icon  = nullptr;

HistTab g_tab           = HistTab::Timelapse;
bool    g_manage        = false;
bool    g_history_active   = false;
bool    g_history_subscribed = false;
std::vector<middleware::Subscription> g_history_subs;
ui_modal_dialog_t *g_del_dlg = nullptr;

/** 每张卡片的运行时上下文（删除动作 + 左滑状态 + 多选状态） */
struct HistoryCard {
    enum class Kind : uint8_t {
        PrintVideo = 0,
        Timelapse  = 1,
        AlbumPhoto = 2,
    };
    Kind        kind = Kind::Timelapse;
    std::string frames_dir;         /**< manifest 相对目录名（删除主键） */
    std::string photo_abs_path;     /**< Album：绝对路径 */
    std::string thumb;
    std::string print_thumb;
    std::string video;
    std::string thumb_src;          /**< LVGL FS 路径，如 A:/home/media/video/x/thumb.rthm */
    std::string title;              /**< 播放器标题（作业名） */
    std::string subtitle;           /**< 播放器副标题（时间 日期） */
    bool        video_ready   = false;
    bool        video_pending = false;
    lv_obj_t   *wrap    = nullptr;  /**< 整条容器：复选框 + 卡片架（删除时删它） */
    lv_obj_t   *check   = nullptr;  /**< 管理模式复选框（常态隐藏，flex 会跳过） */
    lv_obj_t   *check_mark = nullptr; /**< 复选框内的对勾（未选中时隐藏） */
    lv_obj_t   *card    = nullptr;  /**< 卡片（左滑平移对象） */
    lv_obj_t   *del_btn = nullptr;  /**< 红色删除圆钮 */
    bool        open      = false;  /**< 左滑展开态 */
    bool        selected  = false;  /**< 管理模式勾选态 */
};

std::vector<HistoryCard *> g_cards;

/* 前向声明（定义顺序晚于调用点） */
void set_manage_mode(bool on);
void apply_manage_ui(void);
void rebuild_list(void);
void show_empty(bool empty);
void apply_empty_copy(void);

bool card_has_delete_target(const HistoryCard* c)
{
    if(!c) {
        return false;
    }
    if(c->kind == HistoryCard::Kind::AlbumPhoto) {
        return !c->photo_abs_path.empty();
    }
    return !c->frames_dir.empty();
}

HistTab tab_from_index(int idx)
{
    if(idx < 0 || idx >= kTabCount) {
        return HistTab::Timelapse;
    }
    return kTabOrder[idx];
}

/** 右侧悬浮翻页钮直径（与设置页同档） */
int32_t scroll_btn_size(void)
{
    return USER_BACK_BTN_H * 5 / 4;
}

/**
 * 内容区右侧内缩 = 翻页钮直径 + 左右各一个边距。Tab 行、告警横幅、卡片列表、
 * Storage 条四处共用同一个值，右缘才对得齐（改翻页钮尺寸时只需动这里）。
 */
int32_t content_right_pad(void)
{
    return scroll_btn_size() + USER_PAD * 2;
}

/** Tab 行右侧按钮组的上抬量（稿上按钮比 Tab 文字略高一点） */
int32_t manage_btn_lift(void)
{
    return USER_PAD_SM / 2;
}

/* ============================================================================
 *  格式化
 * ========================================================================= */

/** start_unix → "Mon DD, YYYY"（日期分组标题 / 播放器副标题都用它） */
void format_date(int64_t unix_sec, char *buf, size_t n)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%b %d, %Y", &tm);
}

/** 起止时间段 → "21:00:13 - 22:03:59"（稿 4438:6127；结束点由起点 + 时长算出） */
void format_time_range(int64_t start_unix, uint32_t duration_sec,
                       char *buf, size_t n)
{
    std::time_t t0 = static_cast<std::time_t>(start_unix);
    std::time_t t1 = static_cast<std::time_t>(start_unix + duration_sec);
    std::tm tm0{}, tm1{};
    localtime_r(&t0, &tm0);
    localtime_r(&t1, &tm1);
    char a[16], b[16];
    std::strftime(a, sizeof(a), "%H:%M:%S", &tm0);
    std::strftime(b, sizeof(b), "%H:%M:%S", &tm1);
    std::snprintf(buf, n, "%s - %s", a, b);
}

/** duration_sec → "4h 12m"（≥1h）/ "12m"（≥1min）/ "45s"（稿 4443:6155） */
void format_duration(uint32_t sec, char *buf, size_t n)
{
    const uint32_t h = sec / 3600u;
    const uint32_t m = (sec % 3600u) / 60u;
    if(h > 0) {
        std::snprintf(buf, n, "%uh %02um", h, m);
    } else if(m > 0) {
        std::snprintf(buf, n, "%um", m);
    } else {
        std::snprintf(buf, n, "%us", sec);
    }
}

/** material_g → "89.2g"；0（切片查不到）显示 "--"（ASCII，避免字库缺字） */
void format_material(float g, char *buf, size_t n)
{
    if(g > 0.01f) {
        std::snprintf(buf, n, "%.1fg", static_cast<double>(g));
    } else {
        std::snprintf(buf, n, "%s", "--");
    }
}

/** 容量数值 → "0.7M" / "190M"（<10 保一位小数，≥10 取整，与稿一致） */
void format_size_mb(float mb, char *buf, size_t n)
{
    if(mb < 10.0f) {
        std::snprintf(buf, n, "%.1fM", static_cast<double>(mb));
    } else {
        std::snprintf(buf, n, "%.0fM", static_cast<double>(mb));
    }
}

/* ============================================================================
 *  存储容量 / 告警横幅
 * ========================================================================= */

int storage_percent(void)
{
    const float total = storage_total_mb();
    if(total <= 0.0f) {
        return 0;
    }
    int pct = static_cast<int>((storage_used_mb() * 100.0f) / total + 0.5f);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

/** 正常=主色蓝 / 将满=稿里的橙 / 已满=危险红 */
lv_color_t storage_color(void)
{
    const int pct = storage_percent();
    if(pct >= 100) {
        return user_color_status_err;
    }
    if(pct >= kStorageWarnPct) {
        return lv_color_hex(kColorWarn);
    }
    return user_color_accent;
}

/** 刷新顶部容量条 + 告警横幅显隐（两者共用同一套阈值与配色） */
void refresh_storage(void)
{
    const int pct = storage_percent();
    const lv_color_t col = storage_color();

    if(g_storage_fill) {
        lv_obj_set_width(g_storage_fill, LV_PCT(pct < 2 ? 2 : pct)); /* 保底可见 */
        lv_obj_set_style_bg_color(g_storage_fill, col, 0);
    }
    if(g_storage_lbl) {
        char used[16], total[16], txt[40];
        format_size_mb(storage_used_mb(), used, sizeof(used));
        format_size_mb(storage_total_mb(), total, sizeof(total));
        std::snprintf(txt, sizeof(txt), "%s/%s", used, total);
        lv_label_set_text(g_storage_lbl, txt);
        lv_obj_set_style_text_color(g_storage_lbl,
                                    pct >= kStorageWarnPct ? col : user_color_text, 0);
    }

    if(!g_banner) {
        return;
    }
    if(pct < kStorageWarnPct) {
        lv_obj_add_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_banner_lbl,
                      pct >= 100 ? "Storage is full. Delete unused files to free space."
                                 : "Storage almost full. Delete unused files to free space.");
    lv_obj_set_style_text_color(g_banner_lbl, col, 0);
    /* 稿：底色同色 5%、描边同色 20%（4550:8296） */
    lv_obj_set_style_bg_color(g_banner, col, 0);
    lv_obj_set_style_border_color(g_banner, col, 0);
    if(g_banner_icon) {
        lv_obj_set_style_image_recolor(g_banner_icon, col, 0);
    }
}

/* ============================================================================
 *  左滑删除
 * ========================================================================= */

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
    if(g_manage) {
        return; /* 管理模式下删除走顶部 Delete，左滑让位给勾选 */
    }
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if(dir == LV_DIR_LEFT) {
        open_card(c);
    } else if(dir == LV_DIR_RIGHT) {
        close_card(c);
    }
}

/* ============================================================================
 *  删除（单条左滑 / 管理模式批量共用同一套落盘逻辑）
 *
 *  Timelapse：只删延时资产（*.png / thumb.rthm / timelapse.mp4），保留打印录像。
 *  Print History：只删打印录像（print_h264.mp4 / print_thumb / print.h264），保留延时。
 *  两侧都空后：移除清单条目；目录空则删目录。
 *  Album：删 PNG 原图及其配对 `.rthm` 缩略图。
 * ========================================================================= */

bool remove_file_if_exists(const std::string& abs_path)
{
    if(abs_path.empty()) {
        return true;
    }
    std::error_code ec;
    if(!std::filesystem::exists(abs_path, ec)) {
        return true;
    }
    return std::filesystem::remove(abs_path, ec);
}

/** PNG 旁路 RTHM：/home/media/photo/foo.png → …/foo.rthm */
std::string album_companion_rthm(const std::string& photo_abs_path)
{
    if(photo_abs_path.empty()) {
        return {};
    }
    const std::filesystem::path p(photo_abs_path);
    const std::string ext = p.extension().string();
    if(ext != ".png" && ext != ".PNG") {
        return {};
    }
    return p.parent_path().string() + "/" + p.stem().string() + ".rthm";
}

std::string session_abs_dir(const std::string& frames_dir)
{
    return std::string(UPC_PRINT_VIDEO_DIR) + "/" + frames_dir;
}

bool remove_empty_session_dir(const std::string& frames_dir)
{
    if(frames_dir.empty() || frames_dir == UPC_VIDEO_TMP_NAME) {
        return true;
    }
    const std::string abs_dir = session_abs_dir(frames_dir);
    std::error_code ec;
    if(!std::filesystem::is_directory(abs_dir, ec)) {
        return true;
    }
    if(!std::filesystem::is_empty(abs_dir, ec)) {
        return true;
    }
    return std::filesystem::remove(abs_dir, ec);
}

/** 删延时摄影：层间 PNG + thumb + timelapse.mp4 */
bool remove_timelapse_assets(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return false;
    }
    const std::string abs_dir = session_abs_dir(frames_dir);
    std::error_code ec;
    if(std::filesystem::is_directory(abs_dir, ec)) {
        for(const auto& de : std::filesystem::directory_iterator(abs_dir, ec)) {
            if(ec) {
                break;
            }
            if(!de.is_regular_file(ec) || ec) {
                continue;
            }
            const std::string name = de.path().filename().string();
            const std::string ext = de.path().extension().string();
            const bool is_png =
                (ext == ".png" || ext == ".PNG");
            const bool is_thumb = (name == "thumb.rthm");
            const bool is_tl_mp4 = (name == "timelapse.mp4");
            if(is_png || is_thumb || is_tl_mp4) {
                if(!remove_file_if_exists(de.path().string())) {
                    return false;
                }
            }
        }
    }
    /* 清单相对路径也可能指向已搬迁文件名，再兜底删一次 */
    (void)remove_file_if_exists(abs_dir + "/timelapse.mp4");
    (void)remove_file_if_exists(abs_dir + "/thumb.rthm");
    return true;
}

/** 删打印录像：print_h264.mp4 / print_thumb / 残留 print.h264 */
bool remove_print_video_assets(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return false;
    }
    const std::string abs_dir = session_abs_dir(frames_dir);
    (void)remove_file_if_exists(abs_dir + "/print_h264.mp4");
    (void)remove_file_if_exists(abs_dir + "/print_thumb.rthm");
    (void)remove_file_if_exists(abs_dir + "/print.h264");
    return true;
}

/** 会话目录是否至少有一张编号层间 PNG（1.png…） */
bool session_has_png_frames(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return false;
    }
    const std::string abs_dir = session_abs_dir(frames_dir);
    std::error_code ec;
    if(!std::filesystem::is_directory(abs_dir, ec) || ec) {
        return false;
    }
    for(const auto& de : std::filesystem::directory_iterator(abs_dir, ec)) {
        if(ec) {
            break;
        }
        if(!de.is_regular_file(ec) || ec) {
            continue;
        }
        const std::string name = de.path().filename().string();
        if(name.size() < 5) {
            continue;
        }
        /* 要求「纯数字 + .png」，与播放器 parse_png_index 一致 */
        size_t i = 0;
        while(i < name.size() && name[i] >= '0' && name[i] <= '9') {
            ++i;
        }
        if(i == 0) {
            continue;
        }
        if(name.compare(i, std::string::npos, ".png") == 0 ||
           name.compare(i, std::string::npos, ".PNG") == 0) {
            return true;
        }
    }
    return false;
}

bool session_has_timelapse_thumb(const upc::PrintHistoryEntry& e)
{
    if(e.thumb.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(
        std::string(UPC_PRINT_VIDEO_DIR) + "/" + e.thumb, ec);
}

bool entry_has_timelapse(const upc::PrintHistoryEntry& e)
{
    if(e.video_pending || !e.video.empty() || !e.thumb.empty()) {
        return true;
    }
#if UPC_TIMELAPSE_PLAYBACK_PNG
    return session_has_png_frames(e.frames_dir);
#else
    return false;
#endif
}

bool entry_has_print_video(const upc::PrintHistoryEntry& e)
{
    return !e.print_video.empty() || e.print_video_pending || !e.print_thumb.empty();
}

bool entry_media_empty(const upc::PrintHistoryEntry& e)
{
    return !entry_has_timelapse(e) && !entry_has_print_video(e);
}

upc::PrintHistoryEntry* find_entry_by_frames_dir(
    std::vector<upc::PrintHistoryEntry>& entries, const std::string& frames_dir)
{
    for(auto& e : entries) {
        if(e.frames_dir == frames_dir) {
            return &e;
        }
    }
    return nullptr;
}

bool remove_cards_with_rollback(const std::vector<HistoryCard*>& cards)
{
    if(cards.empty()) {
        return true;
    }

    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        return false;
    }

    bool touched_manifest = false;
    for(const HistoryCard* c : cards) {
        if(!c) {
            continue;
        }
        if(c->kind == HistoryCard::Kind::AlbumPhoto) {
            if(!remove_file_if_exists(c->photo_abs_path)) {
                return false;
            }
            (void)remove_file_if_exists(album_companion_rthm(c->photo_abs_path));
            continue;
        }
        if(c->frames_dir.empty()) {
            return false;
        }

        upc::PrintHistoryEntry* hit =
            find_entry_by_frames_dir(entries, c->frames_dir);

        if(c->kind == HistoryCard::Kind::Timelapse) {
            if(!remove_timelapse_assets(c->frames_dir)) {
                return false;
            }
            if(hit) {
                hit->video.clear();
                hit->thumb.clear();
                hit->video_pending = false;
                hit->clean_first_png = 0;
                hit->clean_last_png = 0;
                touched_manifest = true;
            }
        } else if(c->kind == HistoryCard::Kind::PrintVideo) {
            if(!remove_print_video_assets(c->frames_dir)) {
                return false;
            }
            if(hit) {
                hit->print_video.clear();
                hit->print_thumb.clear();
                hit->print_video_pending = false;
                touched_manifest = true;
            }
        }

        if(hit && entry_media_empty(*hit)) {
            const std::string dir = hit->frames_dir;
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [&](const upc::PrintHistoryEntry& e) {
                        return e.frames_dir == dir;
                    }),
                entries.end());
            (void)remove_empty_session_dir(dir);
            /* 若目录仍非空（残留杂文件），整目录清掉以免孤儿 */
            {
                const std::string abs = session_abs_dir(dir);
                std::error_code ec;
                if(std::filesystem::is_directory(abs, ec) && !ec) {
                    (void)std::filesystem::remove_all(abs, ec);
                }
            }
            touched_manifest = true;
        } else if(hit) {
            (void)remove_empty_session_dir(c->frames_dir);
        }
    }

    if(!touched_manifest) {
        return true; /* 仅 Album，或不影响清单 */
    }

    const std::string tmp = std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp, entries)) {
        (void)std::remove(tmp.c_str());
        return false;
    }
    if(std::rename(tmp.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        (void)std::remove(tmp.c_str());
        return false;
    }
    return true;
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

    /* 删空后走统一入口：show_empty() 里会按当前 Tab 刷空态文案与 Start Print 显隐。
     * 早先这里自己拼了一份简化版（只显示占位盒子），逛过 Timelapse/Album 后
     * Start Print 身上的 HIDDEN 不会被清掉，删空就只剩文案没有按钮。 */
    if(g_cards.empty()) {
        show_empty(true);
        apply_manage_ui();   /* 无卡片则收掉 Manage */
    }
}

void del_click_cb(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    /* 生成中的条目不让删：Camera 正在往这个目录里 remux，删了会打断落盘。
     * 「视频不可用」的条目允许删——否则坏条目会永远留在列表里。 */
    if(!c || !card_has_delete_target(c) || c->video_pending) {
        return;
    }
    if(!remove_cards_with_rollback({ c })) {
        ui_toast_show("Delete failed, retry later", user_color_status_err, 1500);
        return;
    }
    ui_toast_show("Deleted", user_color_accent, 1200);
    lv_async_call(delete_card_async, c);
}

void rebuild_list_async(void * /*user*/)
{
    rebuild_list();
}

void close_del_dlg(void)
{
    if(g_del_dlg) {
        ui_modal_dialog_close(g_del_dlg);
        g_del_dlg = nullptr;
    }
}

void on_batch_del_cancel(lv_event_t * /*e*/)
{
    close_del_dlg();
}

void on_batch_del_confirm(lv_event_t * /*e*/)
{
    close_del_dlg();

    std::vector<HistoryCard*> selected_cards;
    for(HistoryCard *c : g_cards) {
        if(c->selected) {
            selected_cards.push_back(c);
        }
    }
    if(selected_cards.empty()) {
        return;
    }
    if(!remove_cards_with_rollback(selected_cards)) {
        ui_toast_show("Delete failed, retry later", user_color_status_err, 1500);
        return;
    }
    ui_toast_show("Deleted", user_color_accent, 1200);
    /* 批量删完退出管理模式，列表整表重建（延后到事件之外，避免删中卡片时 UAF） */
    set_manage_mode(false);
    lv_async_call(rebuild_list_async, nullptr);
}

/** 批量删除二次确认（与选文件页删除、终止打印同一套弹窗样式） */
void open_batch_del_confirm(int count)
{
    if(g_del_dlg || count <= 0) {
        return;
    }
    char title[48];
    if(count > 1) {
        std::snprintf(title, sizeof(title), "Delete %d items?", count);
    } else {
        std::snprintf(title, sizeof(title), "%s", "Delete this item?");
    }

    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(title, USER_DLG_W, true);
    g_del_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_del_dlg) {
        return;
    }
    lv_obj_t *body = ui_user_make_body_label(g_del_dlg->body, "This cannot be undone.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);

    ui_user_make_confirm_footer_btn(g_del_dlg->footer, "Cancel", false,
                                    user_color_panel_alt, on_batch_del_cancel, nullptr);
    ui_user_make_confirm_footer_btn(g_del_dlg->footer, "Delete", true,
                                    ui_color_danger, on_batch_del_confirm, nullptr);
}

/* ============================================================================
 *  管理（多选）模式
 * ========================================================================= */

int selected_count(void)
{
    int n = 0;
    for(const HistoryCard *c : g_cards) {
        if(c->selected) {
            ++n;
        }
    }
    return n;
}

/** 同步单张卡片的勾选外观（红底白勾 / 空框）与卡片选中底色 */
void apply_card_selection(HistoryCard *c)
{
    if(!c || !c->check) {
        return;
    }
    if(c->selected) {
        lv_obj_set_style_bg_color(c->check, ui_color_danger, 0);
        lv_obj_set_style_bg_opa(c->check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(c->check, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(c->check_mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_opa(c->check, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(c->check, LV_OPA_40, 0);
        lv_obj_add_flag(c->check_mark, LV_OBJ_FLAG_HIDDEN);
    }
    if(c->card) {
        /* 选中的卡片底色抬一档，和稿里高亮那张一致 */
        lv_obj_set_style_bg_color(c->card,
            c->selected ? user_color_panel_alt : user_color_panel, 0);
    }
}

/** 刷新「N selected」计数与 Delete 可用态（未选中时置灰不可点） */
void refresh_sel_count(void)
{
    const int n = selected_count();
    if(g_lbl_sel) {
        char txt[32];
        std::snprintf(txt, sizeof(txt), "%d selected", n);
        lv_label_set_text(g_lbl_sel, txt);
    }
    if(g_btn_del) {
        if(n > 0) {
            lv_obj_clear_state(g_btn_del, LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(g_btn_del, LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(g_btn_del, LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(g_btn_del, LV_OPA_40, 0);
        }
    }
}

void toggle_card_selection(HistoryCard *c)
{
    /* 生成中的条目不参与多选：它删不掉（见 del_click_cb） */
    if(!c || c->video_pending || !card_has_delete_target(c)) {
        return;
    }
    c->selected = !c->selected;
    apply_card_selection(c);
    refresh_sel_count();
}

void card_click_cb(lv_event_t *e)
{
    if(!g_manage) {
        return; /* 常态下卡片信息区不响应，播放入口只在缩略图上 */
    }
    toggle_card_selection(static_cast<HistoryCard *>(lv_event_get_user_data(e)));
}

/** Tab 行右侧按钮组切换：常态只有 Manage，管理态是计数 + 三颗按钮 */
void apply_manage_ui(void)
{
    const bool has_cards = !g_cards.empty();
    if(g_btn_manage) {
        if(!g_manage && has_cards) {
            lv_obj_remove_flag(g_btn_manage, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_btn_manage, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_manage_bar) {
        if(g_manage) {
            lv_obj_remove_flag(g_manage_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_manage_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for(HistoryCard *c : g_cards) {
        if(!c->check) {
            continue;
        }
        if(g_manage && !c->video_pending && card_has_delete_target(c)) {
            lv_obj_remove_flag(c->check, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c->check, LV_OBJ_FLAG_HIDDEN);
        }
    }
    refresh_sel_count();
}

void set_manage_mode(bool on)
{
    g_manage = on;
    if(!on) {
        for(HistoryCard *c : g_cards) {
            c->selected = false;
            apply_card_selection(c);
        }
    } else {
        reset_all_cards(); /* 进管理模式先收起左滑 */
    }
    apply_manage_ui();
}

void on_manage_click(lv_event_t * /*e*/)
{
    set_manage_mode(true);
}

void on_manage_cancel(lv_event_t * /*e*/)
{
    set_manage_mode(false);
}

void on_manage_select_all(lv_event_t * /*e*/)
{
    /* 已经全选（可选项都勾上）则再点一次取消全选，符合稿上「全选」按钮的常规预期 */
    int selectable = 0;
    int selected   = 0;
    for(const HistoryCard *c : g_cards) {
        if(c->video_pending || !card_has_delete_target(c)) {
            continue;
        }
        ++selectable;
        if(c->selected) {
            ++selected;
        }
    }
    const bool select_all = (selected < selectable);
    for(HistoryCard *c : g_cards) {
        if(c->video_pending || !card_has_delete_target(c)) {
            continue;
        }
        c->selected = select_all;
        apply_card_selection(c);
    }
    refresh_sel_count();
}

void on_manage_delete(lv_event_t * /*e*/)
{
    const int n = selected_count();
    if(n <= 0) {
        return;
    }
    open_batch_del_confirm(n);
}

/* ============================================================================
 *  缩略图点击 → 延时摄影播放器
 * ========================================================================= */

void on_thumb_click(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(!c) {
        return;
    }
    if(g_manage) {
        toggle_card_selection(c);   /* 管理模式下缩略图也是勾选热区 */
        return;
    }
    if(c->kind == HistoryCard::Kind::AlbumPhoto) {
        return;
    }
    if(c->frames_dir.empty() || !c->video_ready) {
        return;
    }
    char seq_path[256];
    std::snprintf(seq_path, sizeof(seq_path), "%s/%s",
                  UPC_PRINT_VIDEO_DIR, c->frames_dir.c_str());
    char video_path[256];
    video_path[0] = '\0';
    /* Print History 始终走 print_h264.mp4；延时在 PNG 宏下不传 mp4。 */
    const bool pass_mp4 =
        (c->kind == HistoryCard::Kind::PrintVideo)
#if !UPC_TIMELAPSE_PLAYBACK_PNG
        || (c->kind == HistoryCard::Kind::Timelapse)
#endif
        ;
    if(pass_mp4 && !c->video.empty()) {
        std::snprintf(video_path, sizeof(video_path), "%s/%s",
                      UPC_PRINT_VIDEO_DIR, c->video.c_str());
    }
    page_user_timelapse_set_source(seq_path,
                                   video_path[0] ? video_path : nullptr,
                                   c->title.c_str(),
                                   c->subtitle.c_str());
    ui::PageManager::instance().switch_to("user_timelapse");
}

std::string make_thumb_src_fallback(const upc::PrintHistoryEntry& e,
                                    HistoryCard::Kind kind)
{
    const std::string base = std::string(UPC_PRINT_VIDEO_DIR) + "/";
    const std::string thumb_rel =
        (kind == HistoryCard::Kind::PrintVideo && !e.print_thumb.empty())
            ? e.print_thumb
            : e.thumb;
    if(!thumb_rel.empty()) {
        const std::string abs_thumb = base + thumb_rel;
        std::error_code ec;
        if(std::filesystem::exists(abs_thumb, ec)) {
            return std::string(kThumbDrive) + abs_thumb;
        }
    }
    if(!e.frames_dir.empty()) {
        const std::string abs_first_png = base + e.frames_dir + "/1.png";
        std::error_code ec;
        if(std::filesystem::exists(abs_first_png, ec)) {
            return std::string(kThumbDrive) + abs_first_png;
        }
    }
    return {};
}

/* ============================================================================
 *  小构件
 * ========================================================================= */

/**
 * 放开页根的子对象裁剪：Storage 那条要抬到页顶之上（回到常驻条那一行），而 LVGL 默认
 * 把子对象裁在父对象自身区域内。OVERFLOW_VISIBLE 只是改成按「区域 + ext_draw_size」裁，
 * 所以还得在这个事件里把额外绘制区撑开一条常驻条的高度，两者缺一不可。
 */
void page_ext_draw_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, USER_TOPBAR_H);
}

/** 同上，但只需给 Tab 行右侧按钮组的上抬量留出绘制余量 */
void tab_row_ext_draw_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, manage_btn_lift());
}

/** 右侧竖排圆形翻页按钮（与设置页一致） */
lv_obj_t *make_scroll_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    const int32_t sz = scroll_btn_size();
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

/** Tab 行右侧胶囊按钮（稿 edit-btn 100×40）：danger=true 时是红色 Delete */
lv_obj_t *make_action_btn(lv_obj_t *parent, const char *text, bool danger,
                          lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, USER_HIST_BTN_W, USER_HIST_TAB_H);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if(danger) {
        lv_obj_set_style_bg_color(btn, ui_color_danger, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    } else {
        /* 稿是半透明白胶囊：白底压到 10%，按下抬到 20% */
        lv_obj_set_style_bg_color(btn, user_color_text, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_10, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    }
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
    lv_obj_center(lbl);
    return btn;
}

void apply_tab_style(void)
{
    for(int i = 0; i < kTabCount; ++i) {
        const bool on = (g_tab == kTabOrder[i]);
        if(g_tabs[i].label) {
            lv_obj_set_style_text_color(g_tabs[i].label,
                on ? user_color_text : user_color_text_dim, 0);
        }
        if(g_tabs[i].mark) {
            if(on) {
                lv_obj_remove_flag(g_tabs[i].mark, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_tabs[i].mark, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void on_tab_click(lv_event_t *e)
{
    const int idx = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if(idx < 0 || idx >= kTabCount) {
        return;
    }
    const HistTab next = tab_from_index(idx);
    if(next == g_tab) {
        return;
    }
    g_tab = next;
    apply_tab_style();
    set_manage_mode(false);   /* 切 Tab 退出管理模式 */
    rebuild_list();
}

/** 一个 Tab 项：文字贴顶 + 主色下划线贴底（下划线宽度取文字的一半，与稿一致） */
void make_tab_item(lv_obj_t *parent, int idx)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, on_tab_click, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(idx)));

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, kTabTitles[idx]);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 0);

    /* 量出文字宽度后把项宽写死：SIZE_CONTENT 的容器里放百分比宽的子对象会互相
     * 依赖（下划线要按文字宽算，文字宽又决定容器宽），量一次直接给定值最省心。 */
    lv_obj_update_layout(item);
    const int32_t txt_w = lv_obj_get_width(lbl);
    lv_obj_set_width(item, txt_w);

    lv_obj_t *mark = lv_obj_create(item);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, txt_w / 2, 2);   /* 稿里下划线是文字宽的一半、居中 */
    lv_obj_set_style_bg_color(mark, user_color_accent, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, 0);

    g_tabs[idx].root  = item;
    g_tabs[idx].label = lbl;
    g_tabs[idx].mark  = mark;
}

/** 顶部 Tab 行：左侧三个 Tab + 右侧 Manage / 多选操作组 */
lv_obj_t *build_tab_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, USER_HIST_TAB_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, USER_PAD, 0);
    /* 右侧留出与列表一致的边距（列表要给悬浮翻页钮让位），按钮才跟卡片右缘对齐 */
    lv_obj_set_style_pad_right(row, content_right_pad(), 0);
    lv_obj_set_style_pad_column(row, USER_PAD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* 右侧按钮组要上抬，而按钮高度本就等于行高，不放开裁剪会被切掉顶部一条。
     * 抬出去的几个像素落在常驻设备条那条空白带里（透明不可点，页根也已放开裁剪）。 */
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(row, tab_row_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);

    for(int i = 0; i < kTabCount; ++i) {
        make_tab_item(row, i);
    }

    /* 弹簧：把右侧按钮组推到行尾 */
    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    g_btn_manage = make_action_btn(row, "Manage", false, on_manage_click);
    lv_obj_set_style_translate_y(g_btn_manage, -manage_btn_lift(), 0);

    /* 管理态操作组：N selected + Delete + Select All + Cancel（稿 4470:6682） */
    g_manage_bar = lv_obj_create(row);
    lv_obj_remove_style_all(g_manage_bar);
    lv_obj_set_size(g_manage_bar, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_flow(g_manage_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_manage_bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_manage_bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(g_manage_bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_manage_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_manage_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(g_manage_bar, -manage_btn_lift(), 0);  /* 与 Manage 同高 */

    g_lbl_sel = lv_label_create(g_manage_bar);
    lv_label_set_text(g_lbl_sel, "0 selected");
    lv_obj_set_style_text_color(g_lbl_sel, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_lbl_sel, user_font_caption(), 0);
    lv_obj_set_style_margin_right(g_lbl_sel, USER_PAD_SM, 0);

    g_btn_del = make_action_btn(g_manage_bar, "Delete", true, on_manage_delete);
    make_action_btn(g_manage_bar, "Select All", false, on_manage_select_all);
    make_action_btn(g_manage_bar, "Cancel", false, on_manage_cancel);

    return row;
}

/**
 * 顶部 Storage 容量条（稿 4547:7972）：标签 + 轨道 + "0.7M/200M"。
 * 它在稿里与设备名同排，而页面内容区从常驻条下沿才开始，所以做成浮层并用负 y
 * 偏移抬回那一行（常驻条透明且不可点，不会遮住本条）。
 */
lv_obj_t *build_storage_bar(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(row, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, "Storage");
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *track = lv_obj_create(row);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, USER_HIST_BAR_W, USER_HIST_BAR_H);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    g_storage_fill = lv_obj_create(track);
    lv_obj_remove_style_all(g_storage_fill);
    lv_obj_set_height(g_storage_fill, LV_PCT(100));
    lv_obj_set_style_radius(g_storage_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_storage_fill, LV_OPA_COVER, 0);
    lv_obj_align(g_storage_fill, LV_ALIGN_LEFT_MID, 0, 0);

    g_storage_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(g_storage_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(g_storage_lbl, user_font_caption(), 0);

    /* 行心对齐到常驻条行心：页顶在屏幕 USER_TOPBAR_H 处，故上抬半条 + 半行。
     * 右缘用内容区的内缩量而非常驻条的边距，才能与 Manage 按钮、卡片右缘三者齐平。 */
    lv_obj_update_layout(row);
    const int32_t h = lv_obj_get_height(row);
    lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -content_right_pad(),
                 -(USER_TOPBAR_H + h) / 2);
    return row;
}

/** 存储告警横幅（稿 4550:8296）：三角图标 + 一句提示，纯提示不拦截操作 */
lv_obj_t *build_banner(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_HIST_BANNER_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, USER_PAD_SM, 0);
    lv_obj_set_style_pad_column(bar, USER_PAD_SM, 0);
    lv_obj_set_style_radius(bar, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_10, 0);   /* 稿 5%，LVGL 最近一档 */
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_20, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = ui_user_make_icon_img(bar, &ic_alert, USER_PAD_SM * 5 / 4,
                                          lv_color_hex(kColorWarn));
    g_banner_icon = lv_obj_get_child(box, 0);

    g_banner_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(g_banner_lbl, user_font_caption(), 0);
    lv_obj_set_flex_grow(g_banner_lbl, 1);
    return bar;
}

/** 状态胶囊：Completed=绿 / Interrupted=橙（稿 4478:7264 / 4536:7432） */
void make_status_pill(lv_obj_t *parent, bool complete)
{
    const lv_color_t col = complete ? lv_color_hex(kColorCompleted)
                                    : lv_color_hex(kColorWarn);
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, col, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_10, 0);   /* 稿 10% 同色底 */
    lv_obj_set_style_pad_hor(pill, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_ver(pill, 2, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, complete ? "Completed" : "Interrupted");
    lv_obj_set_style_text_color(lbl, col, 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
}

/** 卡片底部一个指标：图标 + 标签 + 数值（稿 4443:6154 一组） */
void make_metric(lv_obj_t *parent, const lv_image_dsc_t *icon,
                 const char *caption, const char *value)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(group, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(group, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* A8 图标按 24px 原尺寸 1:1 贴（见 gen_home_metric_icons.py 的 clock/cube/layers 档） */
    if(icon) {
        ui_user_make_icon_img(group, icon, static_cast<int32_t>(icon->header.w),
                              user_color_text_dim);
    }

    lv_obj_t *cap = lv_label_create(group);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);

    lv_obj_t *val = lv_label_create(group);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, user_color_text, 0);
    lv_obj_set_style_text_font(val, user_font_caption(), 0);
    lv_obj_set_style_margin_left(val, USER_PAD_SM / 2, 0);
}

/** 缩略图槽：图片（cover 裁切）+ "Timelapse" 角标 + 右下角全屏角标 */
void make_thumbnail(lv_obj_t *parent, const HistoryCard *c)
{
    const int32_t th = USER_HIST_CARD_H;
    const int32_t tw = th * 8 / 5;   /* 稿 256×160 = 8:5，竖构图相机图 cover 裁切 */

    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, tw, th);
    /* 直角 + 不裁圆角：clip_corner 每帧建掩膜层，是列表滚动卡顿主因，去掉提速。
     * 代价是卡片左侧两个圆角被缩略图切成直角，与稿有一点出入，这里选性能。 */
    lv_obj_set_style_radius(slot, 0, 0);
    lv_obj_set_style_bg_color(slot, lv_color_hex(kColorThumbBg), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, on_thumb_click, LV_EVENT_CLICKED,
                        const_cast<HistoryCard *>(c));

    /* 视频还没生成好：稿里是纯色底 + 一句提示，不铺缩略图 */
    if(!c->video_ready) {
        lv_obj_t *tips = lv_label_create(slot);
        lv_label_set_text(tips, c->video_pending
#if UPC_TIMELAPSE_PLAYBACK_PNG
                                              ? "Preparing..."
#else
                                              ? "Generating video..."
#endif
                                                : "Video unavailable");
        lv_obj_set_style_text_color(tips, user_color_text_dim, 0);
        lv_obj_set_style_text_font(tips, user_font_caption(), 0);
        lv_obj_center(tips);
    } else if(!c->thumb_src.empty()) {
        lv_obj_t *img = lv_image_create(slot);
        lv_image_set_src(img, c->thumb_src.c_str());
        lv_obj_set_size(img, tw, th);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        /* cover：等比铺满槽，多出部分居中裁切（与相机预览同方案） */
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* 左上角标（按分类显示） */
    lv_obj_t *tag = lv_label_create(slot);
    const char* tag_text = "Timelapse";
    if(c->kind == HistoryCard::Kind::PrintVideo) {
        tag_text = "Print Video";
    } else if(c->kind == HistoryCard::Kind::AlbumPhoto) {
        tag_text = "Photo";
    }
    lv_label_set_text(tag, tag_text);
    lv_obj_set_style_text_color(tag, user_color_text, 0);
    lv_obj_set_style_text_font(tag, user_font_caption(), 0);
    lv_obj_set_style_bg_color(tag, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(tag, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_ver(tag, USER_PAD_SM / 4, 0);
    lv_obj_set_style_radius(tag, USER_BTN_RADIUS / 2, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, USER_PAD_SM / 2, USER_PAD_SM / 2);

    /* 右下角全屏角标：仅提示可点开，点击热区仍是整块缩略图 */
    if(c->video_ready && c->kind != HistoryCard::Kind::AlbumPhoto) {
        lv_obj_t *badge = lv_obj_create(slot);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_50, 0);
        lv_obj_set_style_radius(badge, USER_BTN_RADIUS / 2, 0);
        lv_obj_set_style_pad_all(badge, USER_PAD_SM / 4, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_SM / 2, -USER_PAD_SM / 2);
        ui_user_make_icon_img(badge, &ic_expand, USER_PAD_SM, user_color_text);
    }
}

/** 一条历史记录：[复选框] + 卡片（左缩略图 + 右信息区）+ 左滑删除钮 */
void build_entry(lv_obj_t *parent, const upc::PrintHistoryEntry &e)
{
    HistoryCard *ctx = new HistoryCard();
    ctx->kind = (g_tab == HistTab::History)
        ? HistoryCard::Kind::PrintVideo
        : HistoryCard::Kind::Timelapse;
    ctx->frames_dir = e.frames_dir;
    ctx->thumb = e.thumb;
    ctx->print_thumb = e.print_thumb;
    ctx->video = (ctx->kind == HistoryCard::Kind::PrintVideo) ? e.print_video : e.video;
    ctx->thumb_src = make_thumb_src_fallback(e, ctx->kind);
    {
        const std::string base = std::string(UPC_PRINT_VIDEO_DIR) + "/";
        bool media_ready = false;
        if(ctx->kind == HistoryCard::Kind::PrintVideo) {
            if(!ctx->video.empty()) {
                std::error_code ec;
                media_ready = std::filesystem::exists(base + ctx->video, ec);
            }
        } else {
#if UPC_TIMELAPSE_PLAYBACK_PNG
            /* 可播必须有层间 PNG（仅有 thumb 点进去是空序列）。
             * thumb 用于列表展示；「两个都要」里的 thumb 条件作增强：有图才算完整可播。 */
            media_ready = session_has_png_frames(e.frames_dir) &&
                          session_has_timelapse_thumb(e);
#else
            if(!ctx->video.empty()) {
                std::error_code ec;
                media_ready = std::filesystem::exists(base + ctx->video, ec);
            }
#endif
        }
        /* Print History 只跟 print_video_pending；Timelapse 跟 video_pending。
         * pending 期间一律显示 Generating，不可点；过完开门 finalize 后再认 ready。 */
        const bool pending = (ctx->kind == HistoryCard::Kind::PrintVideo)
            ? e.print_video_pending
            : e.video_pending;
        ctx->video_pending = pending;
        ctx->video_ready = media_ready && !pending;
    }
    g_cards.push_back(ctx);

    char range_buf[40], date_buf[32], dur_buf[16], mat_buf[24];
    format_time_range(e.start_unix, e.duration_sec, range_buf, sizeof(range_buf));
    format_date(e.start_unix, date_buf, sizeof(date_buf));
    format_duration(e.duration_sec, dur_buf, sizeof(dur_buf));
    format_material(e.material_g, mat_buf, sizeof(mat_buf));

    /* 播放器标题/副标题 */
    ctx->title    = e.job_name.empty()
                  ? (ctx->kind == HistoryCard::Kind::PrintVideo ? "Print Video" : "Timelapse")
                  : e.job_name;
    ctx->subtitle = std::string(range_buf) + "  " + date_buf;

    /* 整条：管理模式下第一格是复选框（隐藏时 flex 跳过，卡片自动顶到最左） */
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, USER_HIST_CARD_H);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrap, USER_PAD, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    ctx->wrap = wrap;

    /* 复选框（稿 4470:6958）：空框 / 红底白勾，常态隐藏 */
    lv_obj_t *check = lv_obj_create(wrap);
    lv_obj_remove_style_all(check);
    lv_obj_set_size(check, USER_HIST_CHECK_SZ, USER_HIST_CHECK_SZ);
    lv_obj_set_style_radius(check, USER_BTN_RADIUS / 2, 0);
    lv_obj_set_style_border_width(check, 1, 0);
    lv_obj_set_style_border_color(check, user_color_text, 0);
    lv_obj_set_style_border_opa(check, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(check, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(check, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(check, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(check, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(check, card_click_cb, LV_EVENT_CLICKED, ctx);
    ctx->check = check;

    lv_obj_t *mark = lv_label_create(check);
    lv_label_set_text(mark, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(mark, user_color_text, 0);
    lv_obj_set_style_text_font(mark, user_font_caption(), 0);
    lv_obj_center(mark);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    ctx->check_mark = mark;

    /* 卡片架：删除圆钮浮在卡片下层，卡片左滑平移后露出 */
    lv_obj_t *holder = lv_obj_create(wrap);
    lv_obj_remove_style_all(holder);
    lv_obj_set_height(holder, LV_PCT(100));
    lv_obj_set_flex_grow(holder, 1);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *del = lv_button_create(holder);
    lv_obj_remove_style_all(del);
    const int32_t del_sz = USER_BTN_PRIMARY_H;
    lv_obj_set_size(del, del_sz, del_sz);
    lv_obj_add_flag(del, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(del, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);
    lv_obj_add_flag(del, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(del, del_click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *del_icon = lv_label_create(del);
    lv_label_set_text(del_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(del_icon, user_color_text, 0);
    lv_obj_set_style_text_font(del_icon, user_font_text(), 0);
    lv_obj_center(del_icon);
    ctx->del_btn = del;

    /* 卡片本体：左缩略图 + 右信息区（稿 4448:6193） */
    lv_obj_t *card = lv_obj_create(holder);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(card, USER_BTN_RADIUS, 0);
    /* 稿是 4% 白，这里用等效的不透明底：半透明每帧要与背景混合，滚动时开销大。
     * 仅本页如此，共享的磨砂卡片组件不动（设置页等仍用半透明）。 */
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, USER_PAD_SM, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 清除卡片默认的 GESTURE_BUBBLE：LVGL 基础对象默认带此标志，会把手势事件
     * 一路上抛越过卡片；清掉后手势遍历停在卡片，card_gesture_cb 才能收到左滑。 */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(card, card_gesture_cb, LV_EVENT_GESTURE, ctx);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, ctx);
    ctx->card = card;

    make_thumbnail(card, ctx);

    lv_obj_t *info = lv_obj_create(card);
    lv_obj_remove_style_all(info);
    lv_obj_set_height(info, LV_PCT(100));
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(info, USER_PAD_SM, 0);
    lv_obj_set_style_pad_right(info, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 上半：时间段 + （文件名 + 状态胶囊） */
    lv_obj_t *head = lv_obj_create(info);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(head, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *range = lv_label_create(head);
    lv_label_set_text(range, range_buf);
    lv_obj_set_style_text_color(range, user_color_text_dim, 0);
    lv_obj_set_style_text_font(range, user_font_caption(), 0);

    lv_obj_t *title_row = lv_obj_create(head);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(title_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *name = lv_label_create(title_row);
    lv_label_set_text(name, e.job_name.empty() ? "print" : e.job_name.c_str());
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    /* 不用 flex_grow：状态胶囊要紧跟文件名，不是右对齐到行尾。名字过长时靠 max_width
     * 夹住（否则胶囊会被挤出卡片），并把高度锁成一行——v9 的 DOTS 是「先按宽度换行、
     * 再截末行」，不锁高度只会换行、出不来省略号。 */
    lv_obj_set_style_max_width(name, LV_PCT(65), 0);
    lv_obj_set_height(name, lv_font_get_line_height(user_font_text()));

    make_status_pill(title_row, e.status == upc::PrintHistoryStatus::Complete);

    /* 下半：Duration | Filament | Layers（稿 4443:6165） */
    lv_obj_t *metrics = lv_obj_create(info);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_width(metrics, LV_PCT(100));
    lv_obj_set_height(metrics, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(metrics, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(metrics, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(metrics, LV_OBJ_FLAG_EVENT_BUBBLE);

    make_metric(metrics, &ic_clock, "Duration", dur_buf);
    ui_user_make_divider_vline(metrics, USER_PAD_SM);
    make_metric(metrics, &ic_cube, "Filament", mat_buf);
    ui_user_make_divider_vline(metrics, USER_PAD_SM);
    /* Layers：manifest 还没有层数字段，先占位（见文件头「等后端补数据」） */
    make_metric(metrics, &ic_layers, "Layers", "--");

    apply_card_selection(ctx);
}

void clear_cards(void)
{
    for(HistoryCard *c : g_cards) {
        delete c;
    }
    g_cards.clear();
}

bool is_photo_file(const std::filesystem::path& p)
{
    const std::string ext = p.extension().string();
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".webp" || ext == ".bmp";
}

void build_album_entry(lv_obj_t* parent, const std::filesystem::path& path)
{
    HistoryCard* ctx = new HistoryCard();
    ctx->kind = HistoryCard::Kind::AlbumPhoto;
    ctx->photo_abs_path = path.string();
    /* 优先同名 .rthm（Media 拍照时从 NV12 生成），无则回退原图 */
    {
        const std::string rthm = album_companion_rthm(ctx->photo_abs_path);
        std::error_code ec;
        if(!rthm.empty() && std::filesystem::exists(rthm, ec) && !ec) {
            ctx->thumb_src = std::string(kThumbDrive) + rthm;
        } else {
            ctx->thumb_src = std::string(kThumbDrive) + ctx->photo_abs_path;
        }
    }
    ctx->video_ready = true;
    ctx->video_pending = false;
    ctx->title = path.filename().string();
    ctx->subtitle = "Photo";
    g_cards.push_back(ctx);

    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, USER_HIST_CARD_H);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrap, USER_PAD, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    ctx->wrap = wrap;

    lv_obj_t *check = lv_obj_create(wrap);
    lv_obj_remove_style_all(check);
    lv_obj_set_size(check, USER_HIST_CHECK_SZ, USER_HIST_CHECK_SZ);
    lv_obj_set_style_radius(check, USER_BTN_RADIUS / 2, 0);
    lv_obj_set_style_border_width(check, 1, 0);
    lv_obj_set_style_border_color(check, user_color_text, 0);
    lv_obj_set_style_border_opa(check, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(check, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(check, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(check, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(check, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(check, card_click_cb, LV_EVENT_CLICKED, ctx);
    ctx->check = check;

    lv_obj_t *mark = lv_label_create(check);
    lv_label_set_text(mark, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(mark, user_color_text, 0);
    lv_obj_set_style_text_font(mark, user_font_caption(), 0);
    lv_obj_center(mark);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    ctx->check_mark = mark;

    lv_obj_t *holder = lv_obj_create(wrap);
    lv_obj_remove_style_all(holder);
    lv_obj_set_height(holder, LV_PCT(100));
    lv_obj_set_flex_grow(holder, 1);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *del = lv_button_create(holder);
    lv_obj_remove_style_all(del);
    const int32_t del_sz = USER_BTN_PRIMARY_H;
    lv_obj_set_size(del, del_sz, del_sz);
    lv_obj_add_flag(del, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(del, ui_color_danger, 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);
    lv_obj_add_flag(del, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(del, del_click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *del_icon = lv_label_create(del);
    lv_label_set_text(del_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(del_icon, user_color_text, 0);
    lv_obj_set_style_text_font(del_icon, user_font_text(), 0);
    lv_obj_center(del_icon);
    ctx->del_btn = del;

    lv_obj_t *card = lv_obj_create(holder);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(card, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, USER_PAD_SM, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(card, card_gesture_cb, LV_EVENT_GESTURE, ctx);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, ctx);
    ctx->card = card;

    make_thumbnail(card, ctx);

    lv_obj_t *info = lv_obj_create(card);
    lv_obj_remove_style_all(info);
    lv_obj_set_height(info, LV_PCT(100));
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(info, USER_PAD_SM, 0);
    lv_obj_set_style_pad_right(info, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(info, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *name = lv_label_create(info);
    lv_label_set_text(name, ctx->title.c_str());
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(name, LV_PCT(90), 0);
    lv_obj_set_height(name, lv_font_get_line_height(user_font_text()));

    lv_obj_t *sub = lv_label_create(info);
    lv_label_set_text(sub, ctx->subtitle.c_str());
    lv_obj_set_style_text_color(sub, user_color_text_dim, 0);
    lv_obj_set_style_text_font(sub, user_font_caption(), 0);

    apply_card_selection(ctx);
}

/* ============================================================================
 *  空态
 * ========================================================================= */

struct EmptyCopy {
    const char *title;
    const char *sub;
    bool        with_btn;   /**< 当前三类空态均不显示 Start Print，避免误触流程 */
};

EmptyCopy empty_copy_of(HistTab tab)
{
    switch(tab) {
    case HistTab::Timelapse:
        return { "No Timelapses",
                 "Timelapse videos from your prints will appear here.", false };
    case HistTab::Album:
        return { "No Photos", "Photos you capture will appear here.", false };
    case HistTab::History:
    default:
        return { "No Print Videos",
                 "Print process videos from your prints will appear here.", false };
    }
}

void on_empty_start_print(lv_event_t * /*e*/)
{
    if(control_is_printing()) {
        ui_toast_show("Print in progress", user_color_status_err, 1500);
        return;
    }
    /* 放版预确认弹窗挂在整屏 overlay（control_init(screen)）上，与当前页面无关，
     * 所以不用先切首页：弹窗直接盖在本页上，确认后由 control 自己切到选文件页。 */
    control_start();
}

/** 空态：80×80 圆形图标底 + 打印机图标 + 标题 + 说明 + Start Print（稿 4531:7215） */
lv_obj_t *build_empty(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(box, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, USER_PAD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

    const int32_t ring = USER_BTN_PRIMARY_H * 10 / 9;   /* 稿 80（图标 36 的 2.2 倍） */
    lv_obj_t *icon_box = lv_obj_create(box);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, ring, ring);
    lv_obj_set_style_radius(icon_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon_box, user_color_text, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_10, 0);    /* 稿 3% 白底 */
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_border_color(icon_box, user_color_text, 0);
    lv_obj_set_style_border_opa(icon_box, LV_OPA_20, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ic = ui_user_make_icon_img(icon_box, &ic_printer,
                                         USER_BTN_PRIMARY_H * 5 / 8,
                                         user_color_text_dim);
    lv_obj_center(ic);

    lv_obj_t *texts = lv_obj_create(box);
    lv_obj_remove_style_all(texts);
    lv_obj_set_size(texts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(texts, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(texts, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);

    g_empty_title = lv_label_create(texts);
    lv_obj_set_style_text_color(g_empty_title, user_color_text, 0);
    lv_obj_set_style_text_font(g_empty_title, user_font_text(), 0);

    g_empty_sub = lv_label_create(texts);
    lv_obj_set_style_text_color(g_empty_sub, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_empty_sub, user_font_caption(), 0);

    g_empty_btn = ui_user_make_primary_pill_btn(box, &ic_folder, "Start Print",
                                                on_empty_start_print);
    apply_empty_copy();   /* 先按当前 Tab 落一次文案：标签不设文本时 LVGL 会显示 "Text" */
    return box;
}

/** 按当前 Tab 换空态文案与按钮显隐 */
void apply_empty_copy(void)
{
    const EmptyCopy copy = empty_copy_of(g_tab);
    if(g_empty_title) lv_label_set_text(g_empty_title, copy.title);
    if(g_empty_sub)   lv_label_set_text(g_empty_sub, copy.sub);
    if(g_empty_btn) {
        if(copy.with_btn) {
            lv_obj_remove_flag(g_empty_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_empty_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ============================================================================
 *  列表重建
 * ========================================================================= */

/** 日期分组标题（稿 4366:5427） */
void build_date_title(lv_obj_t *parent, const char *date_text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, date_text);
    lv_obj_set_style_text_color(lbl, user_color_text_dim, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_set_style_margin_top(lbl, USER_PAD_SM, 0);
}

void show_empty(bool empty)
{
    if(g_empty) {
        if(empty) {
            apply_empty_copy();
            lv_obj_remove_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_list) {
        if(empty) {
            lv_obj_add_flag(g_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_list, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_scroll_btns) {
        if(empty) {
            lv_obj_add_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void rebuild_list(void)
{
    if(!g_list) {
        return;
    }

    lv_obj_clean(g_list);
    clear_cards();
    refresh_storage();

    std::vector<upc::PrintHistoryEntry> entries;
    (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);

    /* 未 commit 的条目（结果页还没退出）先不进历史页 */
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [](const upc::PrintHistoryEntry& e) { return !e.committed; }),
        entries.end());

    /* 时间倒序（最新在上） */
    std::sort(entries.begin(), entries.end(),
        [](const upc::PrintHistoryEntry &a, const upc::PrintHistoryEntry &b) {
            return a.start_unix > b.start_unix;
        });

    std::vector<upc::PrintHistoryEntry> visible_entries;
    visible_entries.reserve(entries.size());
    for(const auto& e : entries) {
        if(g_tab == HistTab::Timelapse) {
            if(entry_has_timelapse(e)) {
                visible_entries.push_back(e);
            }
        } else if(g_tab == HistTab::History) {
            if(entry_has_print_video(e)) {
                visible_entries.push_back(e);
            }
        }
    }

    if(g_tab == HistTab::Album) {
        std::vector<std::filesystem::path> photos;
        std::error_code ec;
        if(std::filesystem::is_directory(kPhotoDir, ec)) {
            for(const auto& de : std::filesystem::directory_iterator(kPhotoDir, ec)) {
                if(ec) {
                    break;
                }
                if(!de.is_regular_file(ec) || ec) {
                    continue;
                }
                if(is_photo_file(de.path())) {
                    photos.push_back(de.path());
                }
            }
        }
        std::sort(photos.begin(), photos.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
                std::error_code ea;
                std::error_code eb;
                const auto ta = std::filesystem::last_write_time(a, ea);
                const auto tb = std::filesystem::last_write_time(b, eb);
                if(ea || eb) {
                    return a.filename().string() > b.filename().string();
                }
                return ta > tb;
            });
        if(photos.empty()) {
            show_empty(true);
            apply_manage_ui();
            return;
        }
        show_empty(false);
        for(const auto& p : photos) {
            build_album_entry(g_list, p);
        }
        apply_manage_ui();
        return;
    }

    if(visible_entries.empty()) {
        show_empty(true);
        apply_manage_ui();
        return;
    }
    show_empty(false);

    /* 按自然日分组：日期变了就先插一条分组标题 */
    std::string cur_date;
    for(const upc::PrintHistoryEntry &e : visible_entries) {
        char date_buf[32];
        format_date(e.start_unix, date_buf, sizeof(date_buf));
        if(cur_date != date_buf) {
            cur_date = date_buf;
            build_date_title(g_list, date_buf);
        }
        build_entry(g_list, e);
    }
    apply_manage_ui();
}

} /* namespace */

void refresh_history_async(void * /*user*/)
{
    if(g_history_active) {
        page_user_history_refresh(false);
    }
}

void request_storage_usage_now()
{
    middleware::Message msg(
        middleware::MsgType::UI_Storage_Usage_Req,
        middleware::ModuleId::UI,
        middleware::ModuleId::APP_MEDIA);
    middleware::MsgPort::instance().publish_async(msg);
}

bool read_inline_manifest(const middleware::Message& msg,
                          middleware::PrintVideoManifestPayload& out)
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

bool read_inline_clean_done(const middleware::Message& msg,
                            middleware::CleanTestDonePayload& out)
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

bool read_inline_storage_usage(const middleware::Message& msg,
                               middleware::StorageUsagePayload& out)
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

void ensure_history_subscribed_once()
{
    if(g_history_subscribed) {
        return;
    }
    g_history_subscribed = true;
    auto& bus = middleware::MsgPort::instance();
    g_history_subs.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Video_Manifest_Ntf,
        middleware::ModuleId::UI,
        [](const middleware::Message& msg) {
            middleware::PrintVideoManifestPayload p{};
            if(!read_inline_manifest(msg, p)) {
                return;
            }
            if(g_history_active) {
                lv_async_call(refresh_history_async, nullptr);
            }
        }));
    g_history_subs.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Test_Done_Ntf,
        middleware::ModuleId::UI,
        [](const middleware::Message& msg) {
            middleware::CleanTestDonePayload p{};
            if(!read_inline_clean_done(msg, p)) {
                return;
            }
            if(g_history_active) {
                lv_async_call(refresh_history_async, nullptr);
            }
        }));
    g_history_subs.push_back(bus.subscribe(
        middleware::MsgType::APP_Storage_Usage_Rsp,
        middleware::ModuleId::UI,
        [](const middleware::Message& msg) {
            middleware::StorageUsagePayload p{};
            if(!read_inline_storage_usage(msg, p)) {
                return;
            }
            const double mb = 1024.0 * 1024.0;
            g_storage_total_mb = p.total_bytes > 0
                ? static_cast<float>(static_cast<double>(p.total_bytes) / mb)
                : kStorageTotalMBFallback;
            g_storage_used_mb = p.used_bytes > 0
                ? static_cast<float>(static_cast<double>(p.used_bytes) / mb)
                : 0.0f;
            if(g_history_active) {
                refresh_storage();
            }
        }));
}

void page_user_history_refresh(bool /*force*/)
{
    const bool entering = !g_history_active;
    g_history_active = true;
    if(entering) {
        request_storage_usage_now();
    }
    if(!g_list) {
        return;
    }
    /* 后台刷新（视频生成完等通知）不该把用户正在做的多选清掉 */
    if(!g_manage) {
        rebuild_list();
    } else {
        refresh_storage();
    }
}

void page_user_history_exit(void)
{
    g_history_active = false;
    close_del_dlg();
    set_manage_mode(false);
    reset_all_cards();
}

lv_obj_t *page_user_history_create(lv_obj_t *parent)
{
    g_page        = nullptr;
    g_list        = nullptr;
    g_scroll_btns = nullptr;
    g_empty       = nullptr;
    g_btn_manage  = nullptr;
    g_manage_bar  = nullptr;
    g_banner      = nullptr;
    g_storage_bar = nullptr;
    g_manage      = false;
    g_tab         = HistTab::Timelapse;
    clear_cards();

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* 见 page_ext_draw_cb */
    lv_obj_add_event_cb(page, page_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    g_page = page;
    ensure_history_subscribed_once();

    /* 顶部：Tab 行 + 通栏分隔线（稿 4366:5416） */
    build_tab_row(page);
    lv_obj_t *sep = ui_user_make_divider_line(page, LV_PCT(100));
    lv_obj_set_style_margin_bottom(sep, USER_PAD, 0);

    /* 存储告警横幅：横向边距与列表一致，视觉上与卡片同宽 */
    g_banner = build_banner(page);
    lv_obj_set_style_margin_left(g_banner, USER_PAD, 0);
    lv_obj_set_style_margin_right(g_banner, content_right_pad(), 0);
    lv_obj_set_style_margin_bottom(g_banner, USER_PAD_SM, 0);

    /* 列表容器（纵向滚动）。顶部设备信息条由外壳常驻，见 page_manager。 */
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(list, USER_PAD, 0);
    lv_obj_set_style_pad_row(list, USER_PAD_SM, 0);
    /* 给右侧悬浮翻页钮留位 */
    lv_obj_set_style_pad_right(list, content_right_pad(), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    /* 恢复边界回弹，保留惯性关闭以控制滚动抖动。 */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    g_list = list;

    g_empty = build_empty(page);

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

    /* Storage 浮层最后建：它要压在页面之上（自身透明、不可点，不挡下层控件） */
    g_storage_bar = build_storage_bar(page);

    apply_tab_style();
    page_user_history_refresh(true);
    return page;
}
