#include "page_user_history.hpp"
#include "page_user_timelapse.hpp"
#include "page_user_photo.hpp"
#include "album_index.hpp"
#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "print_history_manifest.hpp"
#include "msg_port.hpp"
#include "upc_contract.hpp"
#include "home_metric_icons.h"

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
 * @brief 打印历史页（对齐浅色设计稿 6122 一族），三列网格 + 长按多选。
 *
 * 五个态与对应稿：
 *   Timelapse 网格   6122:79998
 *   Video 网格       6122:79784
 *   Photo 网格       6122:80163
 *   空态（三个 Tab） 6122:80392 / 6122:80601 / 6122:80532
 *   多选态           6073:73736
 *
 * 布局（顶部那条设备信息由外壳的常驻条 ui_device_bar 画，本页不自建 header；
 * content_panel_ 的 pad_top 已为它让出位置）：
 *   ┌ ─ ─ 常驻条：wifi 设备名 状态 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
 *   │ **Timelapse**  Video  Photo                            Cancel（仅多选态）│
 *   │ May 2026                                                            │
 *   │ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐                 │
 *   │ │   缩略图 ▶   │ │   缩略图 ▶   │ │   缩略图 ▶   │  ← 三列等分       │
 *   │ ├──────────────┤ ├──────────────┤ ├──────────────┤                 │
 *   │ │ Gear_v3.3mf  │ │ Gear_v4.3mf  │ │ Gear_v5.3mf  │                 │
 *   │ │ May 26·22:03 │ │ May 25·22:03 │ │ May 24·22:03 │                 │
 *   │ │      234 Fr..│ │      245 Fr..│ │      250 Fr..│                 │
 *   │ └──────────────┘ └──────────────┘ └──────────────┘                 │
 *   │ April 2026 …（纵向滚动，按**自然月**分组）                             │
 *   │ ┌ 2 selected                                   [ Delete ] ┐（多选态）│
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * 卡片有两种形态，见 build_card_shell：
 *   Timelapse / Video —— 缩略图 + 中央播放钮 + 白色信息条（文件名 + meta 行）
 *   Photo             —— 纯缩略图，左上角压一枚时间戳，没有信息条，故整行更矮
 *
 * 多选由**长按**进入（LV_EVENT_LONG_PRESSED），三个 Tab 都支持，切 Tab 自动退出。
 * 进入后卡片右上角出复选框、选中的卡描一圈主色边，底部浮出「N selected + Delete」。
 *
 * 数据源：/userdata/media/video/config.json（common/fs_manifest/print_history_manifest）；
 * Photo 走 ui::album::scan()。
 * 缩略图：/userdata/media/video/{thumb}（.rthm 原始 RGB565，自定义 decoder），
 *          cover 居中裁切（竖构图相机图铺满横槽）。
 *
 * meta 行右侧那个数：Timelapse 是帧数，Video 是时长。
 *   帧数清单里没有，靠 session_frame_count() 在 frames_dir 上二分探 N.jpg 求出；
 *   探不到（非序列播放模式、或帧已被删）就退回显示时长，见 build_entry。
 */

namespace {

/* LVGL POSIX FS 盘符（lv_conf: LV_FS_POSIX_LETTER 'A'） */
constexpr const char *kThumbDrive = "A:";

/* ---- 设计稿里项目 token 没覆盖的色 ---- */
/** 缩略图槽底色（#202020），也是「视频正在生成」占位底。浅色稿里缩略图仍是深色相机图，
 *  槽底跟着压深，图没铺满时不会露出刺眼的白。 */
constexpr uint32_t kColorThumbBg = 0x202020;
/** 播放钮底（稿 6122:80025 的 black @ 50%）与 Photo 卡左上时间戳的压暗底 */
constexpr uint32_t kColorScrim   = 0x000000;

/* ---- 多选选中环。两个量都跟分辨率无关（抗锯齿的量化门槛是绝对像素数），
 *      故不进主题表，三档同值。 ----
 *
 * 稿上的环是 2px、压在卡内；这里是 4px、**其中 1px 压在卡内、3px 露在卡外**。
 * 两处偏离都是实机量出来的：
 *
 * 1. **2 → 4**：环在圆角那一段是细弧，斜穿像素网格、没有一个像素被完整覆盖，整条弧
 *    都是抗锯齿混合色。2px 时直边是满值 #485DFF(72,93,255)、四个角只有 (88,105,226)，
 *    淡一档且发毛。加粗后弧的横截面能盖满整像素，角上才吃得住色。
 *
 * 2. **往卡内压 1px**（见 kSelRingPad）：角上原先有一圈比环和照片都亮的白边——把它
 *    拆开是 0.753×蓝 + 0.147×照片 + **0.100×纯白**，三通道都对得上。那 10% 的白来自
 *    **卡片自己的底色**：圆角处缩略图是被 clip_corner 以抗锯齿方式裁的，覆盖率不足 1
 *    的那一圈就把底下的 user_light_card 露了出来（直边像素对齐、clip_corner 不介入，
 *    所以只有四个角有）。
 *
 *    lv_draw_rect 的绘制顺序是 shadow → bg → border → outline，环画在卡片白底**之后**；
 *    而子对象又画在环之后。于是把环往里压 1px，缩略图圆角的 AA 就改成与蓝色混合，白
 *    没了。实测那圈白正好 1px 宽（半径 16.5..17.5），压 1px 足够。
 *
 * 加粗与内压合起来：环覆盖半径 15..19（卡缘在 16），外侧仍是 3px，与加粗那一版观感一致。
 */
constexpr int32_t kSelRingW   = 4;
constexpr int32_t kSelRingPad = -1;

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
/* 稿上三个页签的字面（枚举名是旧的，没跟着改——改它要连带动删除逻辑里的 Kind 判定） */
constexpr const char *kTabTitles[kTabCount] = { "Timelapse", "Video", "Photo" };

/** 一个 Tab 项：文字 + 下方主色下划线。
 *  浅色稿**没有下划线**，选中只靠字重与颜色区分，故 mark 建出来后一直藏着——
 *  共享构造 ui_user_make_tab_item 与消息页同用，三个出参不能少给。 */
struct TabItem {
    lv_obj_t *root  = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *mark  = nullptr;
};

lv_obj_t *g_page        = nullptr;  /**< 页根：空态与底部多选条挂此 */
lv_obj_t *g_grid        = nullptr;  /**< 卡片网格（ROW_WRAP，纵向滚动） */
lv_obj_t *g_empty       = nullptr;  /**< 空列表占位（圆图标 + 两行文案） */
lv_obj_t *g_empty_icon  = nullptr;  /**< 圆底里那枚图标（按 Tab 换） */
lv_obj_t *g_empty_title = nullptr;
lv_obj_t *g_empty_sub   = nullptr;

TabItem   g_tabs[kTabCount];
lv_obj_t *g_btn_select_all = nullptr; /**< Tab 行右侧「Select all」（仅多选态可见） */
lv_obj_t *g_btn_cancel  = nullptr;  /**< Tab 行右侧「Cancel」（仅多选态可见） */
lv_obj_t *g_selbar      = nullptr;  /**< 底部多选浮条 */
lv_obj_t *g_lbl_sel     = nullptr;  /**< 「N selected」 */
lv_obj_t *g_btn_del     = nullptr;  /**< 浮条右侧「Delete」 */

HistTab g_tab           = HistTab::Timelapse;
bool    g_manage        = false;
bool    g_history_active   = false;
bool    g_history_subscribed = false;
std::vector<middleware::Subscription> g_history_subs;
ui_modal_dialog_t *g_del_dlg = nullptr;

/** 每张卡片的运行时上下文（删除动作 + 多选状态） */
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
    std::string thumb_src;          /**< LVGL FS 路径，如 A:/userdata/media/video/x/thumb.rthm */
    std::string title;              /**< 播放器标题（作业名） */
    std::string subtitle;           /**< 播放器副标题（时间 日期） */
    /** 这条的时间（Unix 秒）：打印历史取 start_unix、照片取文件 mtime。
     *  meta 行/角标的时间戳与按月分组都用它，0 = 未知。 */
    int64_t     stamp_unix    = 0;
    bool        video_ready   = false;
    bool        video_pending = false;
    /** 网格里的那张卡，也是删除时要摘掉的整块（缩略图 / 信息条 / 复选框都在其下）。
     *  新稿的卡只有这一层，不像旧的纵向列表还要套一层 wrap 安放左滑删除钮。 */
    lv_obj_t   *wrap       = nullptr;
    lv_obj_t   *check      = nullptr; /**< 右上角复选框（常态隐藏） */
    lv_obj_t   *check_mark = nullptr; /**< 复选框内的勾（未选中时隐藏） */
    bool        selected   = false;   /**< 多选勾选态 */
    /** 刚被长按进多选：吃掉 LVGL 随后照发的那次 CLICKED，否则松手会连带进播放器。 */
    bool        just_long_pressed = false;
};

std::vector<HistoryCard *> g_cards;

constexpr uint32_t kListBuildChunkSize = 8u;
constexpr uint32_t kListBuildTimerMs = 12u;

enum class ListBuildKind : uint8_t {
    None = 0,
    HistoryLike = 1,
    Album = 2,
};

ListBuildKind g_list_build_kind = ListBuildKind::None;
std::vector<upc::PrintHistoryEntry> g_pending_entries;
std::vector<ui::album::PhotoEntry> g_pending_photos;
size_t g_pending_index = 0;
std::string g_pending_month;
int32_t g_pending_keep_scroll = 0;
bool g_list_building = false;
lv_timer_t* g_list_build_timer = nullptr;
/** 列表是否过期。离页不拆网格；进页时 dirty=false 直接露出上次内容，避免再扫盘/建卡卡一下。 */
bool g_list_dirty = true;
bool g_rebuild_queued = false;

/* 前向声明（定义顺序晚于调用点） */
void set_manage_mode(bool on);
void apply_manage_ui(void);
void rebuild_list(void);
void mark_list_dirty(void);
void queue_rebuild_list(void);
void show_empty(bool empty);
void apply_empty_copy(void);
void stop_list_build_timer(void);
void finish_list_build(void);
bool append_list_build_chunk(uint32_t budget);
void list_build_timer_cb(lv_timer_t* timer);
void on_thumb_click(lv_event_t *e);

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

/**
 * 内容区左内距（稿 48 = Tab 文字 x176 − 侧栏 128）。
 *
 * 浅色稿里 Tab 行、月标题、卡片网格**三者左缘齐平**，都走这一个值；旧的深色稿
 * 曾让列表比 Tab 行多缩 16，消息页改新稿后也回到了同缘，那个专用字段已删。
 */
int32_t chrome_pad_l(void)
{
    return USER_HIST_PAD_L > 0 ? USER_HIST_PAD_L : USER_PAD;
}

/**
 * 内容区右内距（稿 48 = 屏宽 1280 − 末列右缘 1232）。
 *
 * 与左内距同值，网格因此左右对称。右侧翻页钮随新稿一并去掉，不再为它让位。
 */
int32_t chrome_pad_r(void)
{
    return chrome_pad_l();
}

/* ============================================================================
 *  格式化
 * ========================================================================= */

/** start_unix → "Mon DD, YYYY"（播放器副标题用它） */
void format_date(int64_t unix_sec, char *buf, size_t n)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%b %d, %Y", &tm);
}

/** start_unix → "May 2026"（分组标题；新稿按**自然月**分组，不再按自然日） */
void format_month(int64_t unix_sec, char *buf, size_t n)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%B %Y", &tm);
}

/** start_unix → "May 26 • 22:03"（卡片 meta 行左侧，稿 6122:80031） */
void format_card_stamp(int64_t unix_sec, char *buf, size_t n)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    localtime_r(&t, &tm);
    /* 分隔符用 U+2022（•，UTF-8 三字节 e2 80 a2），**不是稿上那个 U+00B7（·）**。
     * LVGL 预生成的 Montserrat 只覆盖 ASCII(0x20..0x7E) 加一张 62 个码点的稀疏表，
     * 那表里只有 U+00B0(°)、U+2022(•) 和一批 FontAwesome——U+00B7 不在其中，上屏是
     * 豆腐块（踩过一次）。要加字符得重生成字库，而 • 观感已经足够近。 */
    std::strftime(buf, n, "%b %d \xe2\x80\xa2 %H:%M", &tm);
}

/** mtime → "22:03:59"（Photo 卡左上角时间戳，稿 6122:80170） */
void format_clock_hms(int64_t unix_sec, char *buf, size_t n)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%H:%M:%S", &tm);
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

/** duration_sec → "12:48" / "1:12:48"（Video 卡 meta 行右侧，稿 6122:79797 是时钟式） */
void format_clock_dur(uint32_t sec, char *buf, size_t n)
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

/** frames → "234 Frames" / "1 Frame" */
void format_frames(uint32_t frames, char *buf, size_t n)
{
    std::snprintf(buf, n, "%u Frame%s", static_cast<unsigned>(frames),
                  frames == 1u ? "" : "s");
}


/* ============================================================================
 *  删除（多选批量；新稿去掉左滑后这里只剩一个入口）
 *
 *  Timelapse：只删延时资产（*.jpg，以及旧版残留 *.png / thumb.rthm / timelapse.mp4），保留打印录像。
 *  Print History：只删打印录像（print_h264.mp4 / print_thumb / print.h264），保留延时。
 *  两侧都空后：移除清单条目；目录空则删目录。
 *  Album：删 JPEG 原图及其配对 `.rthm` 缩略图。
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

/** 删延时摄影：层间 JPEG（含旧版残留 .png）+ thumb + timelapse.mp4 */
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
            const bool is_still =
                (ext == ".jpg" || ext == ".JPG" ||
                 ext == ".png" || ext == ".PNG");
            const bool is_thumb = (name == "thumb.rthm");
            const bool is_tl_mp4 = (name == "timelapse.mp4");
            if(is_still || is_thumb || is_tl_mp4) {
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

/** 会话目录是否至少有首帧 JPEG（1.jpg）。 */
bool session_has_jpeg_frames(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return false;
    }
    const std::string abs_first = session_abs_dir(frames_dir) + "/1.jpg";
    std::error_code ec;
    return std::filesystem::exists(abs_first, ec);
}

/**
 * 会话的延时帧数（新稿 Timelapse 卡 meta 行右侧那个「234 Frames」）。
 *
 * 清单里没有这个数，只能从盘上数。帧文件是从 1.jpg 起连号的（清粉段也在同一串里，
 * 见 clean_first_png / clean_last_png），所以**先倍增探上界、再二分**求最大的 N，
 * 约 2·log2(N) 次 stat 就够。不用 directory_iterator 逐个数：一个会话动辄上千帧，
 * 而这函数是在分块建列表时每张卡调一次的，列目录会把建列表的耗时拖上去。
 *
 * @return 帧数；目录不存在 / 非静图播放模式 / 帧已被删时返回 0（调用方退回显示时长）
 */
uint32_t session_frame_count(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return 0u;
    }
    const std::string dir = session_abs_dir(frames_dir);
    const auto exists_nth = [&dir](uint32_t i) {
        std::error_code ec;
        return std::filesystem::exists(dir + "/" + std::to_string(i) + ".jpg", ec);
    };
    if(!exists_nth(1u)) {
        return 0u;
    }
    /* 倍增：lo 一定存在，hi 一定不存在 */
    uint32_t lo = 1u;
    uint32_t hi = 2u;
    constexpr uint32_t kFrameProbeCap = 1u << 20;   /* 一百万帧的保险丝 */
    while(hi < kFrameProbeCap && exists_nth(hi)) {
        lo = hi;
        hi <<= 1;
    }
    while(hi - lo > 1u) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if(exists_nth(mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool entry_has_timelapse(const upc::PrintHistoryEntry& e)
{
    if(e.video_pending || !e.video.empty() || !e.thumb.empty()) {
        return true;
    }
#if UPC_TIMELAPSE_PLAYBACK_PNG
    return session_has_jpeg_frames(e.frames_dir);
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
            (void)remove_file_if_exists(
                ui::album::companion_rthm(c->photo_abs_path));
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

void rebuild_list_async(void * /*user*/)
{
    g_rebuild_queued = false;
    /* 离页后排队的重建作废；dirty 保留，下次进页再刷。 */
    if(!g_history_active || !g_grid) {
        return;
    }
    if(g_manage) {
        /* 多选中不打断；回来/退出多选时仍会看到 dirty。 */
        return;
    }
    rebuild_list();
}

void mark_list_dirty(void)
{
    g_list_dirty = true;
}

void queue_rebuild_list(void)
{
    if(g_rebuild_queued) {
        return;
    }
    g_rebuild_queued = true;
    lv_async_call(rebuild_list_async, nullptr);
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
    mark_list_dirty();
    queue_rebuild_list();
}

/**
 * 批量删除二次确认（稿 6251:94239）。
 *
 * 走共享的 ui_modal_dialog，但**开浅色档**——全工程十五个弹窗里目前只有这一个有浅色稿。
 * 外壳（白卡 / 深色标题 / 圆角 / 内距 / 遮罩）由 cfg.light_theme 管，body 与 footer 里的
 * 内容得自己上色：body 标签走的是共享 helper（默认深色档的浅灰字），footer 那两枚靠
 * ui_user_make_confirm_footer_btn 的 light 形参。
 */
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
    cfg.light_theme = true;
    g_del_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_del_dlg) {
        return;
    }
    lv_obj_t *body = ui_user_make_body_label(g_del_dlg->body, "This cannot be undone.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    /* helper 默认给的是深色档的浅灰（白底上糊成一片）与 text(18)，两项都得改。
     * 不给 helper 加档位参数：它有 13 个调用点，而需要浅色的只有这一处。
     *
     * 颜色：稿是 rgba(51,49,69,0.5)，不是一个实色灰。用 user_light_text + 50% 文字
     * 不透明度压出来，白底上正好是稿的 (153,152,162)——先前填的实色 #9e9e9e 量到
     * (158,158,158)，偏亮且丢了那点偏紫。
     * 字号：稿 20（Inter Regular 20），user_font_title() 的 1280 档正是 20。 */
    lv_obj_set_style_text_color(body, user_light_text, 0);
    lv_obj_set_style_text_opa(body, LV_OPA_50, 0);
    lv_obj_set_style_text_font(body, user_font_title(), 0);

    ui_user_make_confirm_footer_btn(g_del_dlg->footer, "Cancel", false,
                                    user_color_panel_alt, true,
                                    on_batch_del_cancel, nullptr);
    /* 稿 #FF4242 = user_color_status_err，与底部多选条那枚 Delete 同色 */
    ui_user_make_confirm_footer_btn(g_del_dlg->footer, "Delete", true,
                                    user_color_status_err, true,
                                    on_batch_del_confirm, nullptr);
}

/* ============================================================================
 *  多选模式（长按进入）
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

/** 该卡能否参与多选：生成中的删不掉（Camera 还在往那个目录里写），没有删除目标的也不行 */
bool card_selectable(const HistoryCard *c)
{
    return c && !c->video_pending && card_has_delete_target(c);
}

/**
 * 同步单张卡片的选中外观（稿 6073:73736）：
 *   选中   —— 卡片描一圈 2px 的主色环 + 右上角换成主色底白勾
 *   未选中 —— 无环 + 右上角是白描边的空心圆角方块
 */
void apply_card_selection(HistoryCard *c)
{
    if(!c || !c->check) {
        return;
    }
    if(c->selected) {
        lv_obj_set_style_bg_color(c->check, user_light_accent, 0);
        lv_obj_set_style_bg_opa(c->check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(c->check, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(c->check_mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_opa(c->check, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(c->check, LV_OPA_80, 0);
        lv_obj_add_flag(c->check_mark, LV_OBJ_FLAG_HIDDEN);
    }
    if(c->wrap) {
        /* outline 而不是 border、且比稿粗一档——理由都在 build_card_shell() 里那段 */
        lv_obj_set_style_outline_width(c->wrap, c->selected ? kSelRingW : 0, 0);
    }
}

/** 可勾选的条目总数（生成中的与无删除目标的不算） */
int selectable_count(void)
{
    int n = 0;
    for(const HistoryCard *c : g_cards) {
        if(card_selectable(c)) {
            ++n;
        }
    }
    return n;
}

/** 刷新底条计数、Select all 文案与 Delete 可用态（未选中时置灰不可点） */
void refresh_sel_count(void)
{
    const int n = selected_count();
    /* 「是否已全勾上」底条文案与 Select all 文案共用一份判据，免得两处说法打架。
     * 一条都没有时不算全选，否则空列表会显示 "All 0 selected"。 */
    const int  total  = selectable_count();
    const bool all_on = (total > 0 && n == total);

    if(g_lbl_sel) {
        char txt[32];
        /* 全勾上时稿另有一套文案（稿 6240:91813 的 "All 5 selected"） */
        if(all_on) {
            std::snprintf(txt, sizeof(txt), "All %d selected", n);
        } else {
            std::snprintf(txt, sizeof(txt), "%d selected", n);
        }
        lv_label_set_text(g_lbl_sel, txt);
    }
    if(g_btn_select_all) {
        /* 文案跟着「再点一次会发生什么」走——这枚本来就是个切换钮，
         * 见 on_manage_select_all()。稿上没画全选后那一态，措辞按产品定。 */
        lv_label_set_text(g_btn_select_all, all_on ? "Deselect all" : "Select all");
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
    if(!card_selectable(c)) {
        return;
    }
    c->selected = !c->selected;
    apply_card_selection(c);
    refresh_sel_count();
}

/**
 * 长按卡片进多选（稿上多选没有别的入口，Manage 按钮整个去掉了）。
 *
 * LVGL 在 LONG_PRESSED 之后照样会发 CLICKED，不挡的话手一松就顺势进了播放器；
 * 这里在卡上打个标记，由 card_click_cb 吃掉紧随的那一次。
 */
void card_press_cb(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(c) {
        /* 每次新按压都从干净状态起。只在 CLICKED 里清不够：长按之后手指若滑出卡片，
         * 那次 CLICKED 根本不会来，标记就一直挂着，把**下一次**正经点击吃掉。 */
        c->just_long_pressed = false;
    }
}

void card_long_press_cb(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(!card_selectable(c)) {
        return;
    }
    c->just_long_pressed = true;
    if(!g_manage) {
        set_manage_mode(true);
    }
    if(!c->selected) {
        toggle_card_selection(c);   /* 长按的那张直接勾上，省一次点 */
    }
}

void card_click_cb(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(c && c->just_long_pressed) {
        c->just_long_pressed = false;
        return;   /* 刚被长按进多选，这一次 CLICKED 不算数 */
    }
    if(!g_manage) {
        on_thumb_click(e);   /* 常态：整张卡都是播放/查看入口 */
        return;
    }
    toggle_card_selection(c);
}

/** 按多选态刷新页面外观：Tab 行右侧的 Cancel、底部浮条、每张卡的复选框 */
void apply_manage_ui(void)
{
    for(lv_obj_t *btn : { g_btn_select_all, g_btn_cancel }) {
        if(!btn) {
            continue;
        }
        if(g_manage) {
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(g_selbar) {
        if(g_manage) {
            lv_obj_remove_flag(g_selbar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_selbar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for(HistoryCard *c : g_cards) {
        if(!c->check) {
            continue;
        }
        if(g_manage && card_selectable(c)) {
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
        apply_manage_ui();
        /* 多选期间总线只打了 dirty：退出多选再刷，避免勾选中途整表拆掉。 */
        if(g_list_dirty && g_history_active) {
            queue_rebuild_list();
        }
        return;
    }
    apply_manage_ui();
}

void on_manage_cancel(lv_event_t * /*e*/)
{
    set_manage_mode(false);
}

/** 全选 / 全不选（稿 6251:94118 的 `Select all`）。
 *  这是个切换钮：已经全勾上时再点一次取消全选，文案同步由 refresh_sel_count() 翻成
 *  `Deselect all`。 */
void on_manage_select_all(lv_event_t * /*e*/)
{
    const bool select_all = (selected_count() < selectable_count());
    for(HistoryCard *c : g_cards) {
        if(!card_selectable(c)) {
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
 *  卡片点击 → 播放器 / 全屏查看器
 * ========================================================================= */

/**
 * 打开这张卡对应的内容。
 *
 * 新稿里整张卡都是热区（旧稿只有左侧缩略图可点），入口统一收在 card_click_cb 里，
 * 由它判过多选态与「长按刚触发」之后再转进来，所以这里不再重复判。
 */
void on_thumb_click(lv_event_t *e)
{
    HistoryCard *c = static_cast<HistoryCard *>(lv_event_get_user_data(e));
    if(!c) {
        return;
    }
    if(c->kind == HistoryCard::Kind::AlbumPhoto) {
        if(c->photo_abs_path.empty()) {
            return;
        }
        page_user_photo_set_source(c->photo_abs_path.c_str());
        ui::PageManager::instance().switch_to("user_photo");
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
    /* Print History 始终走 print_h264.mp4；延时在序列宏下不传 mp4。 */
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
        const std::string abs_first = base + e.frames_dir + "/1.jpg";
        std::error_code ec;
        if(std::filesystem::exists(abs_first, ec)) {
            return std::string(kThumbDrive) + abs_first;
        }
    }
    return {};
}

/* ============================================================================
 *  小构件
 * ========================================================================= */

/**
 * 按多选态刷新 Tab 文字外观（稿 6122:80002）。
 *
 * 浅色稿里选中与否只差字重和不透明度，**没有下划线**——共享构造 ui_user_make_tab_item
 * 还是会建出 mark，这里一律藏起来。字重靠 SemiBold / Medium 两套字；项目字库只有
 * 一档正文字重，故改用「满值 / 50% 不透明」来拉开层次，与稿的观感一致。
 */
void apply_tab_style(void)
{
    for(int i = 0; i < kTabCount; ++i) {
        const bool on = (g_tab == kTabOrder[i]);
        if(g_tabs[i].label) {
            lv_obj_set_style_text_color(g_tabs[i].label, user_light_text, 0);
            lv_obj_set_style_text_opa(g_tabs[i].label, on ? LV_OPA_COVER : LV_OPA_50, 0);
        }
        if(g_tabs[i].mark) {
            lv_obj_add_flag(g_tabs[i].mark, LV_OBJ_FLAG_HIDDEN);
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
    set_manage_mode(false);   /* 切 Tab 退出多选态 */
    mark_list_dirty();
    queue_rebuild_list();
}

/** 一个 Tab 项（稿 6122:80002）：构造与命中区在 ui_user_make_tab_item，与消息页共用 */
void make_tab_item(lv_obj_t *parent, int idx)
{
    /* 稿 6122:80003 是 24（SemiBold / Medium 两档字重，字库只有一档，见 apply_tab_style）。
     * 字号**必须在这里传进去**、不能建完再改：项宽是照它量完写死的。 */
    ui_user_make_tab_item(parent, kTabTitles[idx], user_font_body(), on_tab_click,
                          reinterpret_cast<void *>(static_cast<intptr_t>(idx)),
                          &g_tabs[idx].root, &g_tabs[idx].label,
                          &g_tabs[idx].mark);
}

/** Tab 行右侧那种纯文字钮（主色、无底无框、常态隐藏），多选态下才现身。 */
lv_obj_t *make_tab_text_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, user_light_accent, 0);
    lv_obj_set_style_text_font(lbl, user_font_body(), 0);
    /* 命中区外扩而不是加内距：内距会叠在行的 pad_column 上，两枚之间的视觉间距会从
     * 稿的 44 涨到 80。同 ui_user_make_tab_item 的处理（那边也是零内距 + 外扩）。 */
    lv_obj_set_ext_click_area(lbl, USER_PAD_SM);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(lbl, cb, LV_EVENT_CLICKED, nullptr);
    return lbl;
}

/** 顶部 Tab 行：左侧三个 Tab + 右侧 Select all / Cancel（仅多选态可见） */
lv_obj_t *build_tab_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, USER_HIST_TAB_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 左右内距同值（稿 48），与下面的网格左右缘齐平 */
    lv_obj_set_style_pad_left(row, chrome_pad_l(), 0);
    lv_obj_set_style_pad_right(row, chrome_pad_r(), 0);
    /* Tab 项间距：稿上三段文字左缘 176/354/471，净距 48 */
    lv_obj_set_style_pad_column(row, USER_HIST_PAD_L, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < kTabCount; ++i) {
        make_tab_item(row, i);
    }

    /* 弹簧：把 Cancel 推到行尾 */
    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    /* 多选态右上角那两枚（稿 6251:94118）：`Select all` + `Cancel`，都是主色文字、
     * 没有底与描边，右缘与内容区右缘齐。命中区靠内距撑到够点——光文字本身只有约
     * 60×30，比 UI_USER_MIN_TOUCH 小一圈。 */
    g_btn_select_all = make_tab_text_btn(row, "Select all", on_manage_select_all);
    g_btn_cancel     = make_tab_text_btn(row, "Cancel", on_manage_cancel);

    return row;
}

/**
 * 底部多选浮条（稿 6073:73736）：左「N selected」，右一颗红色 Delete。
 *
 * 稿上量到：条 x=160..1248（侧栏右缘 128 再让 32，右侧同样留 32）、y=688、高 80、
 * 圆角 24、内距 8；Delete 钮 140×64、圆角 16、#FF4242。条是浮层，压在网格之上，
 * 底下的卡片照常能滚。
 */
lv_obj_t *build_sel_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_height(bar, USER_HIST_SELBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(bar, USER_LIGHT_RADIUS, 0);
    lv_obj_set_style_bg_color(bar, user_light_card, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(bar, USER_PAD, 0);
    lv_obj_set_style_pad_right(bar, USER_PAD_SM / 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    g_lbl_sel = lv_label_create(bar);
    lv_label_set_text(g_lbl_sel, "0 selected");
    lv_obj_set_style_text_color(g_lbl_sel, user_light_text, 0);
    lv_obj_set_style_text_font(g_lbl_sel, user_font_body(), 0);

    g_btn_del = lv_button_create(bar);
    lv_obj_remove_style_all(g_btn_del);
    lv_obj_set_size(g_btn_del, USER_HIST_DEL_BTN_W, USER_HIST_DEL_BTN_H);
    lv_obj_set_style_radius(g_btn_del, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(g_btn_del, user_color_status_err, 0);   /* 稿 #FF4242 */
    lv_obj_set_style_bg_opa(g_btn_del, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(g_btn_del, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_event_cb(g_btn_del, on_manage_delete, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(g_btn_del);
    lv_label_set_text(lbl, "Delete");
    lv_obj_set_style_text_color(lbl, user_light_card, 0);   /* 红底白字 */
    lv_obj_set_style_text_font(lbl, user_font_body(), 0);
    lv_obj_center(lbl);

    /* 横向左右各让 USER_PAD_LG（稿：条 x=160..1248，侧栏右缘 128，两侧各 32），
     * 竖向距页底同量。宽度现算而不是写 LV_PCT(100)：FLOATING 不吃 flex 布局，
     * 百分比只会撑满父宽，让不出这两侧的边距。页面尺寸运行期不变，算一次即可。 */
    lv_obj_update_layout(parent);
    lv_obj_set_width(bar, lv_obj_get_content_width(parent) - USER_PAD_LG * 2);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -USER_PAD_LG);
    return bar;
}

/* ============================================================================
 *  网格卡片
 *
 *  两种形态（build_card_shell 按 Tab 分叉）：
 *    Timelapse / Video —— 缩略图 + 中央播放钮 + 白色信息条（文件名 + meta 行）
 *    Photo             —— 纯缩略图，左上角压一枚时间戳，没有信息条
 *
 *  稿上（1280 档）量到的数：内容区 176..1232 共 1056 宽，三列等分、列距 32，
 *  故卡宽 (1056 − 2×32)/3 = 330.67。**卡宽现算不写死**：写死 331 时
 *  3×331 + 2×32 = 1057 比 1056 多 1，最后一列会被挤到下一行。
 * ========================================================================= */

/**
 * 一张卡的宽：内容区宽按三列等分（内容宽已扣掉网格的左右内距）。
 *
 * 调用点在建卡的循环里，此时网格的尺寸必须已经算出来——rebuild_list() 清空之后
 * 会先 update_layout 一次，见那里的说明。真取到 0 只能是被挂在了没布局的父对象上，
 * 与其摆出一排 1px 的卡，不如退回稿上 1280 档的 330，至少还能看出是三列。
 */
int32_t card_w_of(lv_obj_t *grid)
{
    const int32_t avail = lv_obj_get_content_width(grid);
    const int32_t w = (avail - USER_HIST_GRID_GAP * 2) / 3;
    return w > 0 ? w : 330;
}

/** 缩略图槽高：稿上 331×186.67，约 16:9。 */
int32_t thumb_h_of(int32_t card_w)
{
    return card_w * 186 / 331;
}

/**
 * 信息条高。不另开 token，取下面两者的较大值：
 *
 * 1. **稿的比例** 80 : 186.67（信息条 : 缩略图）。跟着缩略图走，卡的整体比例在三档
 *    屏上就都和稿一致。
 * 2. **两行字实际要的高度**（上下内距 12 + 文件名行高 + 行距 8 + meta 行高）。
 *    字库的行高比稿的行框矮一截（1280 档 Montserrat 18/16 是 21/18，稿按 24 排），
 *    所以大档上是比例说了算；但到 1024 档缩略图变矮，比例给的高度会装不下两行字，
 *    那一档换成这个兜底。
 */
int32_t card_info_h(int32_t thumb_h)
{
    const int32_t by_ratio = thumb_h * 80 / 187;
    const int32_t by_text = USER_PAD_SM * 3 / 4                       /* 上内距 12 */
                          + lv_font_get_line_height(user_font_text()) /* 文件名 */
                          + USER_PAD_SM / 2                           /* 行距 8 */
                          + lv_font_get_line_height(user_font_text_sm())
                          + USER_PAD_SM * 3 / 4;                      /* 下内距 12 */
    return by_ratio > by_text ? by_ratio : by_text;
}

/**
 * 缩略图槽：图片（cover 裁切）+ 播放钮 / 时间戳角标。
 *
 * @param with_play  true = 中央压一枚播放钮（Timelapse / Video）
 * @param stamp      非空 = 左上角压一枚时间戳（Photo）
 */
void make_thumbnail(lv_obj_t *parent, const HistoryCard *c, int32_t w, int32_t h,
                    bool with_play, const char *stamp)
{
    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, w, h);
    /* 直角 + 不裁圆角：clip_corner 每帧建掩膜层，是网格滚动卡顿的主因。
     * 圆角交给外层卡片的 clip_corner 一次性处理，见 build_card_shell。 */
    lv_obj_set_style_radius(slot, 0, 0);
    lv_obj_set_style_bg_color(slot, lv_color_hex(kColorThumbBg), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    /* 点击与长按都由外层卡片统一处理，这里只把事件冒上去 */
    lv_obj_add_flag(slot, LV_OBJ_FLAG_EVENT_BUBBLE);

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
        lv_obj_set_style_text_color(tips, user_light_icon_off, 0);
        lv_obj_set_style_text_font(tips, user_font_caption(), 0);
        lv_obj_center(tips);
    } else if(!c->thumb_src.empty()) {
        lv_obj_t *img = lv_image_create(slot);
        lv_image_set_src(img, c->thumb_src.c_str());
        lv_obj_set_size(img, w, h);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        /* cover：等比铺满槽，多出部分居中裁切（与相机预览同方案） */
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* 中央播放钮（稿 6122:80025）：64 的黑 50% 圆 + 白三角。圆与三角都在一张
     * A8 图标里画不出来（图标只有 alpha），故底用对象画、字形贴图标。 */
    if(with_play && c->video_ready) {
        /* 挑最接近的那一档、再按它的**原生边长**摆圆，图标就是 1:1 贴、不用缩放。
         * 稿上圆径 64 对缩略图高 187，按同比例折算到当前槽高。 */
        const lv_image_dsc_t *glyph = ui_user_pick_icon(ic_hist_play_set, h * 64 / 187);
        const int32_t sz = glyph ? static_cast<int32_t>(glyph->header.w) : 0;
        if(sz > 0) {
            lv_obj_t *play = lv_obj_create(slot);
            lv_obj_remove_style_all(play);
            lv_obj_set_size(play, sz, sz);
            lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(play, lv_color_hex(kColorScrim), 0);
            lv_obj_set_style_bg_opa(play, LV_OPA_50, 0);
            lv_obj_clear_flag(play, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(play, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_center(play);
            /* 图标的画布边长就是圆的直径，三角在里面已按稿摆好，直接居中贴 */
            lv_obj_t *ic = ui_user_make_icon_img(play, glyph, sz, user_light_card);
            lv_obj_center(ic);
        }
    }

    /* Photo 卡左上角的时间戳（稿 6122:80170）：白字压一层暗底，免得撞上亮缩略图 */
    if(stamp && stamp[0]) {
        lv_obj_t *lbl = lv_label_create(slot);
        lv_label_set_text(lbl, stamp);
        lv_obj_set_style_text_color(lbl, user_light_card, 0);
        lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
        lv_obj_set_style_bg_color(lbl, lv_color_hex(kColorScrim), 0);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_30, 0);
        lv_obj_set_style_pad_hor(lbl, USER_PAD_SM / 2, 0);
        lv_obj_set_style_pad_ver(lbl, USER_PAD_SM / 4, 0);
        lv_obj_set_style_radius(lbl, USER_HIST_CARD_RADIUS / 2, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, USER_PAD_SM, USER_PAD_SM);
    }
}

/**
 * 卡片外壳：白底卡 + 缩略图 + 右上角复选框。写入 ctx 的 wrap / check / check_mark。
 *
 * Photo 那一路没有信息条，卡高就等于缩略图高，所以整行比另两个 Tab 矮一截——
 * 这是稿上就有的差别，不是没对齐。
 *
 * @param with_info  true = 建信息条（Timelapse / Video）；false = 只有缩略图（Photo）
 * @return           信息条对象（调用方往里塞文件名与 meta 行）；with_info 为假时返回 nullptr
 */
lv_obj_t *build_card_shell(lv_obj_t *parent, HistoryCard *ctx, bool with_info)
{
    const int32_t w  = card_w_of(parent);
    const int32_t th = thumb_h_of(w);
    const int32_t info_h = with_info ? card_info_h(th) : 0;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, th + info_h);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(card, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    /* 稿 drop-shadow(0, 2, 3) #0C1C61 @ 4%；LVGL 的不透明度最近一档是 10/255 */
    lv_obj_set_style_shadow_width(card, 3, 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_shadow_color(card, user_light_shadow, 0);
    lv_obj_set_style_shadow_opa(card, 10, 0);
    /* 选中时描一圈主色环（宽度由 apply_card_selection 在 0/kSelRingW 之间切）。
     *
     * **用 outline 而不是 border**：`LV_STYLE_BORDER_WIDTH` 带的是 `LAYOUT_UPDATE`，
     * 而且 border 算进 `lv_obj_get_style_space_left()`——一勾选内容区左右各缩 2，
     * 定宽的缩略图与 `LV_PCT(100)` 的信息条跟着变，卡内排版当场位移。
     * `LV_STYLE_OUTLINE_WIDTH` 带的是 `EXT_DRAW_UPDATE`，只刷额外绘制区、不动布局
     * （额外绘制区由 LVGL 自动刷，不必手调 lv_obj_refresh_ext_draw_size）。
     *
     * **环主要画在卡外**（只往里压 kSelRingPad 那 1px），与稿的「整圈压在卡内」不同：
     * 稿的描边 rect 覆盖 538.7..540.7 而卡从 538.7 起。这里不跟，因为 outline 是在
     * `LV_EVENT_DRAW_MAIN` 里与背景一起画的、**早于子对象**，整圈压进卡内会被缩略图
     * 盖掉——压 1px 是刚好够遮住那圈白边、又不会被完全吞掉的量，见 kSelRingPad。
     * 想严丝合缝跟稿只能另加一层浮层子对象来画环，代价是每张卡多一个对象。 */
    lv_obj_set_style_outline_width(card, 0, 0);
    lv_obj_set_style_outline_color(card, user_light_accent, 0);
    lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_pad(card, kSelRingPad, 0);
    /* 缩略图要被卡片的圆角裁住，否则四角会露出方形的深色槽底。整张卡只此一处
     * clip_corner，比早先每个缩略图各裁一次省。 */
    lv_obj_set_style_clip_corner(card, true, 0);
    /* 行距由卡片自己的下边距给，网格的 pad_row 是 0——两者叠加的坑见
     * page_user_history_create() 里 pad_row 那段。 */
    lv_obj_set_style_margin_bottom(card, USER_HIST_GRID_GAP, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_press_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, card_long_press_cb, LV_EVENT_LONG_PRESSED, ctx);
    ctx->wrap = card;

    const bool is_photo = (ctx->kind == HistoryCard::Kind::AlbumPhoto);
    char stamp[16] = { 0 };
    if(is_photo && ctx->stamp_unix > 0) {
        format_clock_hms(ctx->stamp_unix, stamp, sizeof(stamp));
    }
    make_thumbnail(card, ctx, w, th, !is_photo, stamp);

    lv_obj_t *info = nullptr;
    if(info_h > 0) {
        /* 信息条（稿 6122:80028）：内距 16/12、两行间距 8 */
        info = lv_obj_create(card);
        lv_obj_remove_style_all(info);
        lv_obj_set_size(info, LV_PCT(100), info_h);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_hor(info, USER_PAD_SM, 0);
        lv_obj_set_style_pad_row(info, USER_PAD_SM / 2, 0);
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(info, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* 右上角复选框（稿 6073:73736）：24 见方、圆角 8，压在缩略图上而不是信息条上，
     * 距上距右各 16。常态隐藏，由 apply_manage_ui 控制显隐。 */
    lv_obj_t *check = lv_obj_create(card);
    lv_obj_remove_style_all(check);
    lv_obj_add_flag(check, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(check, USER_HIST_CHECK_SZ, USER_HIST_CHECK_SZ);
    lv_obj_set_style_radius(check, USER_HIST_CARD_RADIUS / 2, 0);
    lv_obj_set_style_border_width(check, 2, 0);
    lv_obj_set_style_border_color(check, user_light_card, 0);
    lv_obj_set_style_border_opa(check, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(check, user_light_accent, 0);
    lv_obj_set_style_bg_opa(check, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(check, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
    /* 复选框自己不收点击：整张卡都是勾选热区，事件冒到卡上由 card_click_cb 处理，
     * 免得「点框」和「点卡」两条路各切一次、互相抵消。 */
    lv_obj_add_flag(check, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -USER_PAD_SM, USER_PAD_SM);
    ctx->check = check;

    lv_obj_t *mark = ui_user_make_icon_img(
        check, ui_user_pick_icon(ic_hist_check_set, USER_HIST_CHECK_SZ),
        USER_HIST_CHECK_SZ, user_light_card);
    lv_obj_center(mark);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_EVENT_BUBBLE);
    ctx->check_mark = mark;

    return info;
}

/** 卡片信息条里的文件名 + meta 行（稿 6122:80030/80031） */
void build_card_info(lv_obj_t *info, const char *name_text,
                     const char *left_text, const char *right_text)
{
    lv_obj_t *name = lv_label_create(info);
    lv_label_set_text(name, name_text);
    lv_obj_set_style_text_color(name, user_light_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);   /* 稿 6122:80030 是 20 */
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    /* v9 的 DOTS 是「先按宽度换行、再截末行」，不锁高度就只会换行、出不来省略号 */
    lv_obj_set_height(name, lv_font_get_line_height(user_font_text()));
    lv_obj_add_flag(name, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *meta = lv_obj_create(info);
    lv_obj_remove_style_all(meta);
    lv_obj_set_size(meta, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(meta, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 稿 6122:80031/80032 都是 16、#9e9ea8，两端对齐 */
    for(const char *t : { left_text, right_text }) {
        lv_obj_t *lbl = lv_label_create(meta);
        lv_label_set_text(lbl, t ? t : "");
        lv_obj_set_style_text_color(lbl, user_light_icon_off, 0);
        lv_obj_set_style_text_font(lbl, user_font_text_sm(), 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

/** 一条历史记录：缩略图 + 播放钮 + 信息条 */
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
    ctx->stamp_unix = e.start_unix;
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
            /* 列表里已提前算好 thumb_src（thumb.rthm 或 1.jpg 回退），
             * 这里直接复用，避免为每个会话遍历整目录扫描 JPEG。 */
            media_ready = !ctx->thumb_src.empty();
#else
            if(!ctx->video.empty()) {
                std::error_code ec;
                media_ready = std::filesystem::exists(base + ctx->video, ec);
            }
#endif
        }
        /* Print History 只跟 print_video_pending；Timelapse 跟 video_pending。
         * 体验策略：Timelapse 只要有可播内容就允许点开，video_pending 仅作状态提示；
         * Print History 仍保持 pending 禁播，避免点击到尚未完成的 print_h264。 */
        const bool pending = (ctx->kind == HistoryCard::Kind::PrintVideo)
            ? e.print_video_pending
            : e.video_pending;
        ctx->video_pending = pending;
        if(ctx->kind == HistoryCard::Kind::Timelapse) {
            ctx->video_ready = media_ready;
        } else {
            ctx->video_ready = media_ready && !pending;
        }
    }
    g_cards.push_back(ctx);

    char range_buf[40], date_buf[32], stamp_buf[32], right_buf[24];
    format_time_range(e.start_unix, e.duration_sec, range_buf, sizeof(range_buf));
    format_date(e.start_unix, date_buf, sizeof(date_buf));
    format_card_stamp(e.start_unix, stamp_buf, sizeof(stamp_buf));

    /* meta 行右侧：Timelapse 是帧数、Video 是时长。帧数清单里没有，要扫盘求，
     * 扫不出（非序列播放模式 / 帧已删）就退回时长——总比空着强。 */
    uint32_t frames = 0u;
    if(ctx->kind == HistoryCard::Kind::Timelapse) {
        frames = session_frame_count(e.frames_dir);
    }
    if(frames > 0u) {
        format_frames(frames, right_buf, sizeof(right_buf));
    } else {
        format_clock_dur(e.duration_sec, right_buf, sizeof(right_buf));
    }

    /* 播放器标题/副标题 */
    ctx->title    = e.job_name.empty()
                  ? (ctx->kind == HistoryCard::Kind::PrintVideo ? "Print Video" : "Timelapse")
                  : e.job_name;
    ctx->subtitle = std::string(range_buf) + "  " + date_buf;

    lv_obj_t *info = build_card_shell(parent, ctx, true);
    build_card_info(info, e.job_name.empty() ? "print" : e.job_name.c_str(),
                    stamp_buf, right_buf);

    apply_card_selection(ctx);
}

void clear_cards(void)
{
    for(HistoryCard *c : g_cards) {
        delete c;
    }
    g_cards.clear();
}

/** 一张照片：纯缩略图，左上角压一枚时间戳，**没有信息条**（稿 6122:80163） */
void build_album_entry(lv_obj_t* parent, const ui::album::PhotoEntry& e)
{
    HistoryCard* ctx = new HistoryCard();
    ctx->kind = HistoryCard::Kind::AlbumPhoto;
    ctx->photo_abs_path = e.path;
    /* 缩略图优先配对 .rthm（Media 拍照时从 NV12 生成，扫目录时已确认存在），
     * 没有就回退原图交给 LVGL 内置解码器 */
    ctx->thumb_src = std::string(kThumbDrive) +
                     (e.thumb.empty() ? e.path : e.thumb);
    ctx->video_ready = true;
    ctx->video_pending = false;
    ctx->stamp_unix = e.mtime_unix;
    ctx->title = std::filesystem::path(e.path).filename().string();
    ctx->subtitle = "Photo";
    g_cards.push_back(ctx);

    (void)build_card_shell(parent, ctx, false);   /* false = 不建信息条 */
    apply_card_selection(ctx);
}

/* ============================================================================
 *  空态
 * ========================================================================= */

/** 三个 Tab 各一套：图标 + 标题 + 说明（稿 6122:80392 / 80601 / 80532） */
struct EmptyCopy {
    const lv_image_dsc_t *const *icon;   /**< 多档图标集，按圆底大小挑档 */
    const char *title;
    const char *sub;
};

EmptyCopy empty_copy_of(HistTab tab)
{
    switch(tab) {
    case HistTab::Timelapse:
        return { ic_hist_clock_set, "No Timelapse Videos",
                 "Your completed and interrupted prints will appear here." };
    case HistTab::Album:
        return { ic_hist_image_set, "No Photos Yet",
                 "Photos captured during printing will be saved here." };
    case HistTab::History:
    default:
        return { ic_hist_film_set, "No Videos",
                 "Your completed and interrupted prints will appear here." };
    }
}

/**
 * 空态：80 的浅色圆底 + 32 的图标 + 标题 + 说明，整组在内容区居中。
 *
 * 稿上三个 Tab 只差图标与文案，版式一样，所以只建一次，切 Tab 时由
 * apply_empty_copy() 换内容。新稿**没有按钮**（原来那颗 Start Print 去掉了，
 * 首页的 PRINT 已经是同一个入口）。
 */
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

    /* 稿 6122:80396 是 80 见方、圆角全满，底 #333145 @3%、描边同色 @7% */
    const int32_t ring = USER_EMPTY_RING_SZ > 0 ? USER_EMPTY_RING_SZ
                                                : USER_BTN_PRIMARY_H * 10 / 9;
    lv_obj_t *icon_box = lv_obj_create(box);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, ring, ring);
    lv_obj_set_style_radius(icon_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon_box, user_light_text, 0);
    lv_obj_set_style_bg_opa(icon_box, 8, 0);            /* 稿 3%，255 的 3% ≈ 8 */
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_border_color(icon_box, user_light_text, 0);
    lv_obj_set_style_border_opa(icon_box, 18, 0);       /* 稿 7% */
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    /* 图标由圆底导出：稿是 32 / 80 = 2/5，跟着圆底一起缩放才不会脱节。
     * 三个 Tab 换的是图标源，尺寸不变，故这里先随便挑一档建出来，由
     * apply_empty_copy() 改 src；三套图标的档位表一致，不会换档。 */
    const int32_t icon_sz = ring * 2 / 5;
    lv_obj_t *box_ic = ui_user_make_icon_img(
        icon_box, ui_user_pick_icon(ic_hist_clock_set, icon_sz),
        icon_sz, user_light_icon_off);
    lv_obj_center(box_ic);
    g_empty_icon = lv_obj_get_child(box_ic, 0);

    lv_obj_t *texts = lv_obj_create(box);
    lv_obj_remove_style_all(texts);
    lv_obj_set_size(texts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(texts, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(texts, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_SCROLLABLE);

    /* 稿 6122:80398/80399：标题 20 深色，说明 16 灰 */
    g_empty_title = lv_label_create(texts);
    lv_obj_set_style_text_color(g_empty_title, user_light_text, 0);
    lv_obj_set_style_text_font(g_empty_title, user_font_text(), 0);

    g_empty_sub = lv_label_create(texts);
    lv_obj_set_style_text_color(g_empty_sub, user_light_icon_off, 0);
    lv_obj_set_style_text_font(g_empty_sub, user_font_text_sm(), 0);

    apply_empty_copy();   /* 先按当前 Tab 落一次文案：标签不设文本时 LVGL 会显示 "Text" */
    return box;
}

/** 按当前 Tab 换空态的图标与两行文案 */
void apply_empty_copy(void)
{
    const EmptyCopy copy = empty_copy_of(g_tab);
    if(g_empty_title) lv_label_set_text(g_empty_title, copy.title);
    if(g_empty_sub)   lv_label_set_text(g_empty_sub, copy.sub);
    if(g_empty_icon) {
        const int32_t sz = lv_obj_get_width(g_empty_icon);
        lv_image_set_src(g_empty_icon, ui_user_pick_icon(copy.icon, sz));
    }
}

/* ============================================================================
 *  列表重建
 * ========================================================================= */

/**
 * 月分组标题（稿 6122:80020，如 "May 2026"）。
 *
 * 宽度设成整行：网格是 ROW_WRAP，占满一行的子对象会把后面的卡挤到下一行，
 * 标题因此天然独占一行，不用另开容器。
 */
void build_month_title(lv_obj_t *parent, const char *month_text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, month_text);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);    /* 稿：#333145 @ 50% */
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);   /* 稿 6122:80020 是 20 */
    /* 稿上标题框底 164 → 首行卡顶 180，即下方 16。**只给下边距、不给上边距**：
     * 上方那一份由前一行卡片的 margin_bottom 给（稿量到 33，与卡行距 32 同值），
     * 首个标题没有前一行，由网格的 pad_top 给。整套分工见 page_user_history_create()
     * 里 pad_row 那段。 */
    lv_obj_set_style_margin_bottom(lbl, USER_PAD_SM, 0);
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
    if(g_grid) {
        if(empty) {
            lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void stop_list_build_timer(void)
{
    if(g_list_build_timer) {
        lv_timer_del(g_list_build_timer);
        g_list_build_timer = nullptr;
    }
    g_list_building = false;
    g_list_build_kind = ListBuildKind::None;
    g_pending_entries.clear();
    g_pending_photos.clear();
    g_pending_index = 0;
    g_pending_month.clear();
    g_pending_keep_scroll = 0;
}

void finish_list_build(void)
{
    if(g_list_build_timer) {
        lv_timer_del(g_list_build_timer);
        g_list_build_timer = nullptr;
    }
    g_list_building = false;
    g_list_dirty = false;
    apply_manage_ui();
    if(g_grid && g_pending_keep_scroll > 0) {
        lv_obj_update_layout(g_grid);
        const int32_t max_scroll = lv_obj_get_scroll_bottom(g_grid);
        const int32_t y = (g_pending_keep_scroll < max_scroll)
            ? g_pending_keep_scroll
            : max_scroll;
        if(y > 0) {
            lv_obj_scroll_to_y(g_grid, y, LV_ANIM_OFF);
        }
    }
    g_list_build_kind = ListBuildKind::None;
    g_pending_entries.clear();
    g_pending_photos.clear();
    g_pending_index = 0;
    g_pending_month.clear();
    g_pending_keep_scroll = 0;
}

bool append_list_build_chunk(uint32_t budget)
{
    if(!g_grid || budget == 0u) {
        return true;
    }
    uint32_t built = 0u;
    /* 「上一条的月份不同就插一条标题」——两个分支同一套写法。Album 早先是平铺无分组，
     * 新稿要求和另两个 Tab 一样按月分，用的是 PhotoEntry 新补的 mtime_unix。 */
    const auto month_break = [](int64_t unix_sec) {
        char buf[24];
        format_month(unix_sec, buf, sizeof(buf));
        if(g_pending_month != buf) {
            g_pending_month = buf;
            build_month_title(g_grid, buf);
        }
    };
    if(g_list_build_kind == ListBuildKind::Album) {
        while(g_pending_index < g_pending_photos.size() && built < budget) {
            const auto& p = g_pending_photos[g_pending_index];
            /* mtime 取不到（0）的归到一组，标题会是 1970 那个月——总比不分组好，
             * 而且 scan() 已经把这些排到了最后，不会插在正常月份中间。 */
            month_break(p.mtime_unix);
            build_album_entry(g_grid, p);
            ++g_pending_index;
            ++built;
        }
        return g_pending_index >= g_pending_photos.size();
    }
    if(g_list_build_kind == ListBuildKind::HistoryLike) {
        while(g_pending_index < g_pending_entries.size() && built < budget) {
            const auto& e = g_pending_entries[g_pending_index];
            month_break(e.start_unix);
            build_entry(g_grid, e);
            ++g_pending_index;
            ++built;
        }
        return g_pending_index >= g_pending_entries.size();
    }
    return true;
}

void list_build_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    if(!g_history_active || !g_grid || !g_list_building) {
        stop_list_build_timer();
        return;
    }
    if(append_list_build_chunk(kListBuildChunkSize)) {
        finish_list_build();
    }
}

/**
 * 重建整张列表。
 *
 * `lv_obj_clean` 会把滚动位置一起清零，删一条之后视线会被弹回顶部——这里重建前
 * 记下滚动量、重建后放回去。必须先 `lv_obj_update_layout()` 让新内容高度算出来，
 * 再按 `lv_obj_get_scroll_bottom()` 夹一下：删掉末尾几条后列表变矮，旧滚动量
 * 会越界，直接 scroll_to 会把列表停在空白处。
 */
void rebuild_list(void)
{
    if(!g_grid) {
        return;
    }
    stop_list_build_timer();
    const int32_t keep_scroll = lv_obj_get_scroll_y(g_grid);

    lv_obj_clean(g_grid);
    clear_cards();
    /* 卡宽是按网格的内容宽三等分现算的（见 card_w_of），所以建卡之前必须先让布局落地。
     * 首次进页时网格还是 LV_PCT(100) 的未决宽，不逼这一下会按 0 宽算出 1px 的卡。 */
    lv_obj_update_layout(g_grid);

    std::vector<upc::PrintHistoryEntry> entries;
    (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);

    /* 未 commit：结果页未退出前通常隐藏。
     * 盘满时 commit 写不出 config.json，会永久卡在 committed=false；
     * 若条目已有可展示/可删媒体，仍进列表，否则用户无法腾空间。 */
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [](const upc::PrintHistoryEntry& e) {
                if(e.committed) {
                    return false;
                }
                return entry_media_empty(e);
            }),
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
        /* 枚举与排序走共享的 ui::album::scan()：全屏浏览器用的是同一份顺序，
         * 两处若各自扫目录，「下一张」就会和这里看到的排序错位。
         * scan() 里已按 mtime 倒序排好，下面按月分组直接顺着走即可。 */
        const std::vector<ui::album::PhotoEntry> photos = ui::album::scan();
        if(photos.empty()) {
            show_empty(true);
            apply_manage_ui();
            g_list_dirty = false;
            return;
        }
        show_empty(false);
        g_pending_keep_scroll = keep_scroll;
        g_list_build_kind = ListBuildKind::Album;
        g_pending_photos = photos;
        g_pending_index = 0;
        g_list_building = true;
        apply_manage_ui();
        if(append_list_build_chunk(kListBuildChunkSize * 2u)) {
            finish_list_build();
        } else {
            g_list_build_timer = lv_timer_create(
                list_build_timer_cb, kListBuildTimerMs, nullptr);
        }
        return;
    }

    if(visible_entries.empty()) {
        show_empty(true);
        apply_manage_ui();
        g_list_dirty = false;
        return;
    }
    show_empty(false);
    g_pending_keep_scroll = keep_scroll;
    g_list_build_kind = ListBuildKind::HistoryLike;
    g_pending_entries = std::move(visible_entries);
    g_pending_index = 0;
    g_pending_month.clear();
    g_list_building = true;
    apply_manage_ui();
    if(append_list_build_chunk(kListBuildChunkSize * 2u)) {
        finish_list_build();
    } else {
        g_list_build_timer = lv_timer_create(
            list_build_timer_cb, kListBuildTimerMs, nullptr);
    }
}

} /* namespace */

void refresh_history_async(void * /*user*/)
{
    /* 总线通知一律先打脏；人在本页则异步重建，否则下次进页再刷。 */
    g_list_dirty = true;
    if(g_history_active) {
        page_user_history_refresh(false);
    }
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
            lv_async_call(refresh_history_async, nullptr);
        }));
    g_history_subs.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Test_Result_Ntf,
        middleware::ModuleId::UI,
        [](const middleware::Message& msg) {
            middleware::CleanTestDonePayload p{};
            if(!read_inline_clean_done(msg, p)) {
                return;
            }
            lv_async_call(refresh_history_async, nullptr);
        }));
    /* 新稿去掉了 Storage 容量条与告警横幅，本页不再订阅 APP_Storage_Usage_Rsp。
     * Media 侧的 UI_Storage_Usage_Req 处理仍在（见 app/media/MEDIA.md），只是暂时无人问。 */
}

void page_user_history_refresh(bool force)
{
    (void)force; /* SimplePage 进页恒传 true，不当「强制重建」——靠 dirty 决定 */
    g_history_active = true;
    if(!g_grid) {
        return;
    }
    /* 后台刷新不该把用户正在做的多选清掉；只记 dirty，退出多选/下次进页再刷 */
    if(g_manage) {
        return;
    }
    if(!g_list_dirty) {
        return; /* 缓存命中：网格还在，直接露出 */
    }
    queue_rebuild_list();
}

void page_user_history_exit(void)
{
    /* 建到一半就离开：标记过期，下次进页整表重来，别把半截列表当缓存。 */
    if(g_list_building) {
        mark_list_dirty();
    }
    stop_list_build_timer();
    if(g_rebuild_queued) {
        lv_async_call_cancel(rebuild_list_async, nullptr);
        g_rebuild_queued = false;
    }
    g_history_active = false;
    close_del_dlg();
    set_manage_mode(false);
}

lv_obj_t *page_user_history_create(lv_obj_t *parent)
{
    stop_list_build_timer();
    g_page           = nullptr;
    g_grid           = nullptr;
    g_empty          = nullptr;
    g_btn_select_all = nullptr;
    g_btn_cancel     = nullptr;
    g_selbar         = nullptr;
    g_manage     = false;
    g_tab        = HistTab::Timelapse;
    g_list_dirty = true;
    g_rebuild_queued = false;
    clear_cards();

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    /* 顶内距与左右同值（稿 48）。**本页不显示常驻设备信息条**（浅色稿五帧顶部都没画，
     * 见 PageManager::apply_device_bar 的例外），content_panel_ 的 pad_top 因此是 0，
     * 页面拿到整屏高度，Tab 行到屏顶这段留白得自己给。 */
    lv_obj_set_style_pad_top(page, chrome_pad_l(), 0);
    g_page = page;
    ensure_history_subscribed_once();

    /* 顶部 Tab 行。新稿去掉了它下面那条通栏分隔线，Tab 与网格之间只靠留白分隔。 */
    build_tab_row(page);

    /* 卡片网格（ROW_WRAP，纵向滚动）。顶部设备信息条由外壳常驻，见 page_manager。
     *
     * 用 ROW_WRAP 而不是 LVGL 的 grid 布局：条目数不定、还要在中间插占满整行的月
     * 标题，grid 得先算出行数、每插一条标题都要重排，wrap 天然按内容流。 */
    lv_obj_t *grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* 左右内距同值（稿 48），与 Tab 行左缘齐平 */
    lv_obj_set_style_pad_left(grid, chrome_pad_l(), 0);
    lv_obj_set_style_pad_right(grid, chrome_pad_r(), 0);
    lv_obj_set_style_pad_column(grid, USER_HIST_GRID_GAP, 0);
    /*
     * 纵向间距**一律由各元素自己的 margin_bottom 给，pad_row 置 0**。
     *
     * LVGL 的 flex 里这两者是叠加的、不是取其一：一行的高度算的是
     * `margin_top + 对象高 + margin_bottom`（lv_flex.c 的 lv_obj_get_height_with_margin），
     * 行与行之间**再无条件加一道 pad_row**。两套机制一起用就会打架——早先 pad_row 给
     * 32、月标题又加 16 的下边距，稿上 16 的间距实际渲染成了 48，月标题看着不像在
     * 领着它下面那组卡片。
     *
     * 现在的分工：卡片 margin_bottom = 本值（行距 32），月标题 margin_bottom = USER_PAD_SM
     * （稿 16，标题贴着自己那组卡）。首行上方没有前一行、给不出间距，由下面的 pad_top 补。
     */
    lv_obj_set_style_pad_row(grid, 0, 0);
    lv_obj_set_style_pad_top(grid, USER_HIST_GRID_GAP, 0);
    /* 底部多留出多选浮条的高度 + 它与页底的间距，多选态下最后一行不会被压住 */
    lv_obj_set_style_pad_bottom(grid, USER_HIST_SELBAR_H + USER_PAD_LG * 2, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    /* 恢复边界回弹，保留惯性关闭以控制滚动抖动。 */
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    g_grid = grid;

    g_empty  = build_empty(page);
    /* 多选浮条最后建：要压在网格之上 */
    g_selbar = build_sel_bar(page);

    apply_tab_style();
    /* 首次展示由 SimplePage::on_enter 触发 refresh；这里不再提前刷，避免首进双重建。 */
    return page;
}
