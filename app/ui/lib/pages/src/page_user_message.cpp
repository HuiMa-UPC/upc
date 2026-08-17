#include "page_user_message.hpp"

#include "page_manager.hpp"
#include "ui_alarm_catalog.hpp"
#include "ui_alarm_state.hpp"
#include "ui_message_store.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "home_metric_icons.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

/**
 * @file page_user_message.cpp
 * @brief 消息中心页：设备事件 + MCU 报警的日期分组混排列表 + 详情抽屉。
 *
 * 布局（稿 3033:5247 / 3063:7338 / 3201:9570；顶部设备信息由外壳常驻条画，
 * 本页不自建 header。稿新版加了三 Tab 行 + 通栏分隔线，结构与打印历史页同构）：
 *   ┌ All  Device  System                                  [ Manage ] ┐
 *   ├────────────────────────────────────────────────────────────────┤
 *   │ Today                                                           │
 *   │ ┌──────────────────────────────────────────────────────────┐    │
 *   │ │ ▣  Z axis motion error                        14:03  (›) │    │
 *   │ │    Motion execution timeout                               │    │
 *   │ └──────────────────────────────────────────────────────────┘    │
 *   │ Aug 09, 2026                                          ┌────┐    │
 *   │ ┌───...（历史，可左滑删除）                     ┐       │ ▲▼ │    │
 *   └──────────────────────────────────────────────────────────────── ┘
 *
 * 卡片按稿是玻璃拟态（等效不透明底 + 1px 白 10% 描边），左侧一枚实色圆角徽标
 * （等级配色 + 白图标，与左下通知卡同款），右侧时间 + 半透明圆形 ›。
 * 右缘留出 content_right_pad() 给悬浮翻页钮，与历史页对齐。
 *
 * 数据三路混排（时间倒序，按自然日分组）：
 *   1. `ui_alarm_state` 当前 CONDITION（ACTIVE/RTN_LATCHED）——不可删，带 › 进抽屉；
 *   2. `ui_message_store` 非报警消息（设备事件 + 前端本地）——跟随生命周期自动撤，不可删；
 *   3. `ui_alarm_state` 历史（Resolved CONDITION + EVENT）——可左滑删除，带 › 进抽屉。
 *   报警 EVENT 在 store 里也挂着一份（喂浮层用），列表里跳过以免与历史重复。
 *
 * 详情抽屉（右侧滑出，稿 3201:9570）：建在 `lv_layer_top()` 上，满高覆盖侧栏与
 * 常驻设备条（挂页面里会被 content_panel_ 的 pad_top 顶下来、又被设备条压住头）。
 * 顶栏（圆形返回钮 + Notification Details）
 * + 正文（标题 → 状态胶囊 → 相对时刻 → 完整描述 → 底部小字 code/detail/时刻
 * → 主按钮）。稿上标题下那块 448×280 是处理步骤图，暂无图片资源故不画。
 * Reset 按钮三态（见 build_drawer_footer）：RESET+latched 可点 → Resetting…，
 * AUTO/POWER_CYCLE/NONE/EVENT 无按钮只给处理指引。复位成功（MCU 回 NORMAL 帧）
 * 后条目自动转历史、抽屉自动关。
 *
 * 管理（多选）模式与左滑一样只对历史条目开放：当前报警要等 MCU 解除、设备事件
 * 跟随后端生命周期，都删不掉，因此不出复选框、点了没反应。入口是列表上方那行
 * 稿外补的 Manage（稿上本页没有顶栏）。
 *
 * 左滑删除复用 page_user_history 的手势模式（清 GESTURE_BUBBLE + translate_x）。
 * store / alarm_state 两路观察者都只置脏 + lv_async 重建（回调里可能正拿着
 * 将被删除的按钮派发事件，不能同步 clean）。
 */

namespace {

/* ---- 生命周期胶囊配色。等级配色不在这儿：四档阶梯由
 *      ui_message_severity_color() 出，消息页与左下浮层共用一份。 ---- */
constexpr uint32_t kPillActive   = 0xFF4242;  /**< 红：条件当前存在 */
constexpr uint32_t kPillLatched  = 0xFF8A3D;  /**< 橙：已恢复、等待复位 */
constexpr uint32_t kPillEvent    = 0x485DFF;  /**< 蓝：一次性 EVENT */
constexpr uint32_t kColorResolved = 0x37E47B; /**< 绿（Resolved 胶囊） */

/* ---- 稿 3033:5247 / 3063:7338 / 3201:9570 的卡片观感 ----
 * 稿上卡片填充是白 4%，实现用等效不透明底（见 build_entry），故这里只留描边值。 */
constexpr lv_opa_t kCardBorderOpa = 26;   /**< 稿 rgba(255,255,255,0.1) */
constexpr uint32_t kDelGradA      = 0xFC3D3E;  /**< 删除钮渐变起（稿 139°） */
constexpr uint32_t kDelGradB      = 0xFF6667;  /**< 删除钮渐变止 */

/** 列表一条的来源。 */
enum class ItemSrc : uint8_t {
    AlarmCurrent,   /**< ui_alarm_state 当前 CONDITION */
    StoreMsg,       /**< ui_message_store 非报警消息 */
    AlarmHistory,   /**< ui_alarm_state 历史（Resolved / EVENT） */
};

/* ---- 顶部三 Tab（稿 3033:5247 新版）----
 *
 * 分档判据是码段，`DeviceEventCode::UiWearMask`(512) 以上是"UI 本地事件，后端不
 * 应发送"（见 upc_contract.hpp 那段注释），正好是设备/系统的天然分界：
 *   Device = MCU 报警（码 ≥ 1000，走 AlarmCurrent/AlarmHistory）+ 后端设备事件（码 < 512）
 *   System = 前端本地提示（512..999）
 * 稿上系统消息那栏画的是 OTA 升级、账号共享，本工程还没有这类数据源，所以这个
 * Tab 目前只有打印结束那两条本地提示，多数时候是空的。 */
enum class MsgTab : uint8_t { All = 0, Device, System };

constexpr int kTabCount = 3;
constexpr MsgTab kTabOrder[kTabCount] = { MsgTab::All, MsgTab::Device, MsgTab::System };
constexpr const char *kTabTitles[kTabCount] = { "All", "Device", "System" };

/** 本地消息码段起点：< 512 是后端设备事件，≥ 512 是前端自己 push 的。 */
constexpr uint16_t kLocalCodeBase = 512;

/** 一个 Tab 项：文字 + 下方主色下划线（未选中时下划线隐藏）。 */
struct TabItem {
    lv_obj_t *root  = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *mark  = nullptr;
};

/** 组装期的一条列表项（重建时现算，不长期持有）。 */
struct MsgItem {
    ItemSrc     src;
    uint16_t    code;
    int         hist_idx;   /**< AlarmHistory：ui_alarm_history_at 下标 */
    int64_t     sort_ts;    /**< 排序/分组时刻（毫秒） */
    const char *title;
    const char *body;
    ui_msg_severity_t sev;  /**< 徽标配色与图标都由它出（共用四档阶梯） */
    bool        resolved;   /**< 历史 CONDITION（画 Resolved 胶囊） */
    bool        deletable;  /**< 仅历史可左滑删除 */
    bool        has_detail; /**< 报警两路有抽屉，画 › */
};

/** 卡片运行时上下文（左滑状态 + 点击路由 + 多选态），new/delete 随卡片走。 */
struct MsgCard {
    ItemSrc   src;
    uint16_t  code;
    int       hist_idx;
    bool      deletable;    /**< 仅历史条目：能左滑删、管理模式下能勾选 */
    bool      selected;     /**< 管理模式勾选态 */
    lv_obj_t *check;        /**< 管理模式复选框（常态隐藏，flex 会跳过） */
    lv_obj_t *check_mark;   /**< 复选框里的对勾 */
    bool      open;         /**< 左滑展开态（松手吸附后的稳定态） */
    /* 跟手拖动的按压期状态，每次 PRESSED 重置 */
    int32_t   press_x;      /**< 按下点（屏幕坐标） */
    int32_t   press_y;
    int32_t   base_x;       /**< 按下时的位移基准：0 或 -reveal */
    bool      dragging;     /**< 已判定为横向拖动 */
    bool      drag_locked;  /**< 本次按压判给竖向滚动，不再拖 */
    bool      just_dragged; /**< 刚拖过：吃掉紧随其后的 CLICKED */
    lv_obj_t *card;
    lv_obj_t *del_btn;
};

lv_obj_t *g_list      = nullptr;
lv_obj_t *g_scroll_btns = nullptr; /**< 右侧上下翻页悬浮层（空列表时收起） */
lv_obj_t *g_empty     = nullptr;
lv_obj_t *g_drawer    = nullptr;   /**< 抽屉整层（backdrop + panel），NULL=未开 */
bool      g_active    = false;     /**< 本页在前台（refresh 置位 / exit 清零） */
bool      g_rebuild_queued = false;

TabItem   g_tabs[kTabCount];
MsgTab    g_tab        = MsgTab::All;

/* ---- 管理（多选）模式：结构照搬历史页，判据换成 MsgItem::deletable ---- */
bool      g_manage     = false;
lv_obj_t *g_btn_manage = nullptr;  /**< 常态右上「Manage」 */
lv_obj_t *g_manage_bar = nullptr;  /**< 管理态「N selected + 三钮」 */
lv_obj_t *g_lbl_sel    = nullptr;
lv_obj_t *g_btn_del    = nullptr;
ui_modal_dialog_t *g_del_dlg = nullptr;

std::vector<MsgCard *> g_cards;

/* 抽屉当前展示的目标（重建后据此刷新/关闭） */
ItemSrc  g_drawer_src;
uint16_t g_drawer_code;
int      g_drawer_hist_idx;

void rebuild_list(void);
void drawer_close(void);
void drawer_sync(void);
void apply_tab_style(void);
void apply_manage_ui(void);
void set_manage_mode(bool on);
void refresh_sel_count(void);

/* ============================================================================
 *  几何：稿是 1280×800，这里一律换算成 theme token，三档屏共用
 * ========================================================================= */

/** 右侧翻页圆钮直径（与历史页同款同尺寸，两页右缘才对得齐）。 */
int32_t scroll_btn_size(void)
{
    return USER_BACK_BTN_H * 5 / 4;
}

/** 列表右侧内缩：让开悬浮翻页钮，卡片右缘落在稿上的位置。 */
int32_t content_right_pad(void)
{
    return scroll_btn_size() + USER_PAD * 2;
}

/** 卡片左侧实色徽标方块边长（稿 32 见方，同左下通知卡）。 */
int32_t badge_size(void)
{
    return USER_PRINT_BADGE_SZ * 2;
}

/* ============================================================================
 *  格式化
 * ========================================================================= */

/** ts → 组头："Today" / "Yesterday" / "Aug 09, 2026" */
void format_group(int64_t ts_ms, char *buf, size_t n)
{
    const std::time_t t   = static_cast<std::time_t>(ts_ms / 1000);
    const std::time_t now = time(nullptr);
    std::tm tm_t{}, tm_now{};
    localtime_r(&t, &tm_t);
    localtime_r(&now, &tm_now);

    const int day_t   = tm_t.tm_year * 366 + tm_t.tm_yday;
    const int day_now = tm_now.tm_year * 366 + tm_now.tm_yday;
    if(day_t == day_now) {
        snprintf(buf, n, "Today");
    } else if(day_t == day_now - 1) {
        snprintf(buf, n, "Yesterday");
    } else {
        std::strftime(buf, n, "%b %d, %Y", &tm_t);
    }
}

/** ts → 卡片右侧 "14:03" */
void format_clock(int64_t ts_ms, char *buf, size_t n)
{
    if(ts_ms <= 0) {
        buf[0] = '\0';
        return;
    }
    const std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%H:%M", &tm);
}

/** ts → 抽屉里的完整时刻 "Aug 09, 2026 14:03:27" */
void format_full(int64_t ts_ms, char *buf, size_t n)
{
    if(ts_ms <= 0) {
        snprintf(buf, n, "--");
        return;
    }
    const std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, n, "%b %d, %Y %H:%M:%S", &tm);
}

/** ts → 抽屉标题下的相对时刻 "Today 18:30"（稿 3204:9818 那行浅字） */
void format_relative(int64_t ts_ms, char *buf, size_t n)
{
    if(ts_ms <= 0) {
        snprintf(buf, n, "--");
        return;
    }
    char grp[32], clk[16];
    format_group(ts_ms, grp, sizeof(grp));
    format_clock(ts_ms, clk, sizeof(clk));
    snprintf(buf, n, "%s %s", grp, clk);
}

/* ============================================================================
 *  数据组装：三路合并 + 时间倒序
 * ========================================================================= */

std::vector<MsgItem> collect_items(void)
{
    std::vector<MsgItem> items;
    const int64_t now_ms = static_cast<int64_t>(time(nullptr)) * 1000;

    /* 1. 当前 CONDITION 报警 */
    for(int i = 0; i < ui_alarm_current_count(); i++) {
        const ui_alarm_entry_t *e = ui_alarm_current_at(i);
        MsgItem it{};
        it.src        = ItemSrc::AlarmCurrent;
        it.code       = e->code;
        it.hist_idx   = -1;
        it.sort_ts    = (e->ts_ms > 0) ? e->ts_ms : now_ms;
        it.title      = e->title;
        it.body       = e->body;
        it.sev        = ui_alarm_severity_to_msg(e->severity);
        it.has_detail = true;
        items.push_back(it);
    }

    /* 2. store 里的非报警消息（报警号从 1000 起：当前 CONDITION 与 EVENT 都
     *    另有一路，跳过防重复） */
    for(int i = 0; i < ui_message_total_count(); i++) {
        const ui_message_t *m = ui_message_at(i);
        if(m->code >= 1000) {
            continue;
        }
        MsgItem it{};
        it.src      = ItemSrc::StoreMsg;
        it.code     = m->code;
        it.hist_idx = -1;
        it.sort_ts  = (m->ts_ms > 0) ? m->ts_ms : now_ms;
        it.title    = m->title;
        it.body     = m->body;
        it.sev      = m->severity;
        items.push_back(it);
    }

    /* 3. 报警历史（Resolved CONDITION + EVENT） */
    for(int i = 0; i < ui_alarm_history_count(); i++) {
        const ui_alarm_history_entry_t *h = ui_alarm_history_at(i);
        MsgItem it{};
        it.src        = ItemSrc::AlarmHistory;
        it.code       = h->code;
        it.hist_idx   = i;
        it.sort_ts    = (h->resolved_ts_ms > 0) ? h->resolved_ts_ms : h->ts_ms;
        it.title      = h->title;
        it.body       = h->body;
        it.sev        = ui_alarm_severity_to_msg(h->severity);
        it.resolved   = !h->is_event;
        it.deletable  = true;
        it.has_detail = true;
        items.push_back(it);
    }

    /* Tab 过滤：报警一律算设备侧，store 消息按码段分（≥512 是前端本地提示） */
    if(g_tab != MsgTab::All) {
        const bool want_system = (g_tab == MsgTab::System);
        items.erase(std::remove_if(items.begin(), items.end(),
            [want_system](const MsgItem &it) {
                const bool is_system = (it.src == ItemSrc::StoreMsg &&
                                        it.code >= kLocalCodeBase);
                return is_system != want_system;
            }), items.end());
    }

    std::stable_sort(items.begin(), items.end(),
                     [](const MsgItem &a, const MsgItem &b) {
                         return a.sort_ts > b.sort_ts;
                     });
    return items;
}

/* ============================================================================
 *  左滑删除：跟手拖动 + 松手吸附
 *
 *  不用 LVGL 的 LV_EVENT_GESTURE：它的门槛是"每帧位移 ≥ gesture_min_velocity(3)
 *  且累计 > gesture_min_distance(50)"，慢慢拖时累加器每帧被清零，手指怎么划都
 *  触发不了（见 lv_indev.c indev_gesture）。而且手势不会吞掉释放时的 CLICKED，
 *  两者都挂在卡片上必然打架。这里改成自己按 PRESSED/PRESSING/RELEASED 算位移，
 *  顺带用 just_dragged 挡住随后那次点击。
 * ========================================================================= */

/** 卡片左移量：露出删除钮 + 稿上钮与卡片之间那道间隙。 */
int32_t swipe_reveal_px(void)
{
    return scroll_btn_size() + USER_PAD;
}

/** 起拖阈值：小于它仍算点击，避免手抖把点击吃掉。 */
constexpr int32_t kDragStartPx = 8;

/** 落位移 + 按位移决定删除钮显隐（拖动中/吸附后共用）。 */
void set_card_shift(MsgCard *c, int32_t x)
{
    lv_obj_set_style_translate_x(c->card, x, 0);
    if(!c->del_btn) {
        return;
    }
    if(x < 0) {
        lv_obj_remove_flag(c->del_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->del_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void close_card(MsgCard *c)
{
    if(!c || !c->open) {
        return;
    }
    c->open = false;
    set_card_shift(c, 0);
}

void reset_all_cards(void)
{
    for(MsgCard *c : g_cards) {
        close_card(c);
    }
}

void card_press_cb(lv_event_t *e)
{
    MsgCard *c = static_cast<MsgCard *>(lv_event_get_user_data(e));
    lv_indev_t *indev = lv_indev_active();
    /* 每次新按压都清干净：上一次若以 PRESS_LOST 收场（没有 CLICKED 来消费），
     * just_dragged 会残留下来把下一次点击吃掉。 */
    c->dragging     = false;
    c->drag_locked  = false;
    c->just_dragged = false;
    /* 兜底解除上一次的轴向排他（正常由 release 恢复，这里防万一漏掉） */
    lv_obj_add_flag(c->card, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    /* 管理模式下左滑让位给勾选 */
    if(g_manage || !c->del_btn || !indev) {
        c->drag_locked = true;   /* 不可删条目不参与拖动 */
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    c->press_x = p.x;
    c->press_y = p.y;
    c->base_x  = c->open ? -swipe_reveal_px() : 0;
}

void card_pressing_cb(lv_event_t *e)
{
    MsgCard *c = static_cast<MsgCard *>(lv_event_get_user_data(e));
    if(c->drag_locked) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if(!indev) {
        return;
    }

    /* 列表一旦真的滚起来，这次按压整个让给滚动，卡片弹回原位 */
    if(lv_indev_get_scroll_obj(indev)) {
        if(c->dragging) {
            set_card_shift(c, c->base_x);
            c->dragging = false;
        }
        c->drag_locked = true;
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int32_t dx = p.x - c->press_x;
    const int32_t dy = p.y - c->press_y;

    if(!c->dragging) {
        if(LV_ABS(dx) < kDragStartPx) {
            return;                     /* 还没够起拖，先当点击 */
        }
        if(LV_ABS(dy) > LV_ABS(dx)) {   /* 方向锁定：竖向归列表滚动 */
            c->drag_locked = true;
            return;
        }
        c->dragging = true;
        /* 轴向排他：起拖后不许列表再抢走这次按压。LVGL 每帧按整次按压的累计位移
         * 重新判方向（lv_indev_scroll.c find_scroll_obj），斜着拉一下竖向累计
         * 超过横向就会切成滚动、把卡片弹回。卡片自身不可滚，清掉 SCROLL_CHAIN_VER
         * 后滚动搜索断在卡片这层，够不到列表；松手再加回来。 */
        lv_obj_remove_flag(c->card, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        if(!c->open) {
            reset_all_cards();          /* 同时只允许一张展开 */
        }
    }

    const int32_t reveal = swipe_reveal_px();
    int32_t x = c->base_x + dx;
    if(x > 0)       x = 0;
    if(x < -reveal) x = -reveal;
    set_card_shift(c, x);
}

/** 松手（或按压被抢走）：过半吸附到展开，否则弹回。 */
void card_release_cb(lv_event_t *e)
{
    MsgCard *c = static_cast<MsgCard *>(lv_event_get_user_data(e));
    /* 无条件解除轴向排他：不能只在拖过的分支恢复，否则一次异常收场就永久失效 */
    lv_obj_add_flag(c->card, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    if(!c->dragging) {
        return;
    }
    c->dragging     = false;
    c->just_dragged = true;

    const int32_t reveal = swipe_reveal_px();
    const int32_t x      = lv_obj_get_style_translate_x(c->card, LV_PART_MAIN);
    c->open = (-x) > reveal / 2;
    set_card_shift(c, c->open ? -reveal : 0);
}

/* ============================================================================
 *  管理（多选）模式
 *
 *  只有历史条目可删（当前报警要等 MCU 解除、设备事件跟随后端生命周期），所以
 *  不可删的卡片在管理模式下不出复选框、点了也没反应——判据是 MsgItem::deletable。
 *  管理模式期间左滑让位给勾选（见 card_press_cb）。
 * ========================================================================= */

int selected_count(void)
{
    int n = 0;
    for(const MsgCard *c : g_cards) {
        if(c->selected) {
            ++n;
        }
    }
    return n;
}

/** 同步单张卡的勾选外观（红底白勾 / 空框）与卡片选中底色。 */
void apply_card_selection(MsgCard *c)
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
        lv_obj_set_style_bg_color(c->card,
            c->selected ? user_color_panel_alt : user_color_panel, 0);
    }
}

void toggle_card_selection(MsgCard *c)
{
    if(!c || !c->deletable) {
        return;   /* 当前报警 / 设备事件删不掉，不参与多选 */
    }
    c->selected = !c->selected;
    apply_card_selection(c);
    refresh_sel_count();
}

/** 刷新「N selected」与 Delete 可用态（未选中时置灰）。 */
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

/** 常态只有 Manage（且要有可删条目才出），管理态换成计数 + 三钮。 */
void apply_manage_ui(void)
{
    int deletable = 0;
    for(const MsgCard *c : g_cards) {
        if(c->deletable) {
            ++deletable;
        }
    }
    if(g_btn_manage) {
        if(!g_manage && deletable > 0) {
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
    for(MsgCard *c : g_cards) {
        if(!c->check) {
            continue;
        }
        if(g_manage && c->deletable) {
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
        for(MsgCard *c : g_cards) {
            c->selected = false;
            apply_card_selection(c);
        }
    }
    reset_all_cards();   /* 进出管理模式都先收起左滑 */
    apply_manage_ui();
}

void on_manage_click(lv_event_t *)
{
    drawer_close();      /* 抽屉开着时进管理模式，先收起来 */
    set_manage_mode(true);
}

void on_manage_cancel(lv_event_t *)
{
    set_manage_mode(false);
}

void on_manage_select_all(lv_event_t *)
{
    /* 已经全选则反向取消（同历史页：这颗钮是 toggle 语义） */
    int deletable = 0;
    for(const MsgCard *c : g_cards) {
        if(c->deletable) {
            ++deletable;
        }
    }
    const bool all = (deletable > 0 && selected_count() == deletable);
    for(MsgCard *c : g_cards) {
        if(!c->deletable) {
            continue;
        }
        c->selected = !all;
        apply_card_selection(c);
    }
    refresh_sel_count();
}

void close_del_dlg(void)
{
    if(g_del_dlg) {
        ui_modal_dialog_close(g_del_dlg);
        g_del_dlg = nullptr;
    }
}

void on_batch_del_cancel(lv_event_t *)
{
    close_del_dlg();
}

void on_batch_del_confirm(lv_event_t *)
{
    close_del_dlg();

    /* 按下标**从大到小**删：ui_alarm_history_delete_at 会把后面的条目前移，
     * 从小到大删的话第二次就删错人了。 */
    std::vector<int> idx;
    idx.reserve(g_cards.size());
    for(const MsgCard *c : g_cards) {
        if(c->selected && c->deletable && c->hist_idx >= 0) {
            idx.push_back(c->hist_idx);
        }
    }
    std::sort(idx.begin(), idx.end());
    for(auto it = idx.rbegin(); it != idx.rend(); ++it) {
        ui_alarm_history_delete_at(*it);
    }
    set_manage_mode(false);   /* 删完退出管理模式（列表由观察者异步重建） */
}

void open_batch_del_confirm(int count)
{
    if(g_del_dlg || count <= 0) {
        return;
    }
    char title[48];
    if(count > 1) {
        std::snprintf(title, sizeof(title), "Delete %d messages?", count);
    } else {
        std::snprintf(title, sizeof(title), "%s", "Delete this message?");
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

void on_manage_delete(lv_event_t *)
{
    open_batch_del_confirm(selected_count());
}

void queue_rebuild(void);

void del_click_cb(lv_event_t *e)
{
    MsgCard *c = static_cast<MsgCard *>(lv_event_get_user_data(e));
    if(c->src == ItemSrc::AlarmHistory) {
        /* 直接按下标删：观察者回调会 queue_rebuild，hist_idx 不会陈旧——
         * 除非同帧插了新历史，那也只是删错相邻一条的理论窗口，重建后自愈 */
        ui_alarm_history_delete_at(c->hist_idx);
    }
}

void card_click_cb(lv_event_t *e)
{
    MsgCard *c = static_cast<MsgCard *>(lv_event_get_user_data(e));
    /* LVGL 释放时照发 CLICKED（拖动不会吞掉它），刚拖完这次必须挡掉，
     * 否则一松手就把刚拖开的卡片当成点击又关回去。 */
    if(c->just_dragged) {
        return;
    }
    if(g_manage) {
        toggle_card_selection(c);   /* 管理模式下整张卡是勾选热区 */
        return;
    }
    if(c->open) {
        close_card(c);
        return;
    }
    if(c->src == ItemSrc::StoreMsg) {
        return;   /* 非报警消息无抽屉 */
    }
    reset_all_cards();
    g_drawer_src      = c->src;
    g_drawer_code     = c->code;
    g_drawer_hist_idx = c->hist_idx;
    drawer_sync();
}

/* ============================================================================
 *  详情抽屉
 * ========================================================================= */

void drawer_close(void)
{
    if(g_drawer) {
        lv_obj_delete(g_drawer);
        g_drawer = nullptr;
    }
}

void backdrop_click_cb(lv_event_t *)
{
    drawer_close();
}

void reset_btn_click_cb(lv_event_t *)
{
    if(ui_alarm_request_reset(g_drawer_code)) {
        /* 状态观察者会把按钮刷成 Resetting…；失败/超时自动恢复可点 */
        drawer_sync();
    }
}

/** 抽屉信息行："Alarm code" + 值 之类的两栏。 */
void drawer_info_row(lv_obj_t *parent, const char *cap, const char *val)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c = lv_label_create(row);
    lv_label_set_text(c, cap);
    lv_obj_set_style_text_color(c, user_color_text_dim, 0);
    lv_obj_set_style_text_font(c, user_font_caption_sm(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_color(v, user_color_text_dim, 0);
    lv_obj_set_style_text_font(v, user_font_caption_sm(), 0);
    lv_obj_set_style_max_width(v, LV_PCT(62), 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
}

/** 状态胶囊：Active（红）/ Awaiting reset（橙）/ Resolved（绿）/ Event（蓝）。 */
void drawer_state_pill(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_10, 0);
    lv_obj_set_style_pad_hor(pill, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(pill, 2, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
}

/** 页脚：Reset 按钮三态或处理指引（clear_mode 一律以报文为准）。 */
void build_drawer_footer(lv_obj_t *parent, const ui_alarm_entry_t *cur)
{
    const char *hint = nullptr;
    if(!cur) {
        return;   /* 历史/EVENT：无操作 */
    }
    switch(cur->clear_mode) {
        case 0:  hint = "Clears automatically once the condition is resolved.";
                 break;
        case 1:  break;   /* RESET：出按钮 */
        case 2:  hint = "Power cycle the machine to clear this alarm.";
                 break;
        case 3:  hint = "This alarm cannot be cleared remotely.";
                 break;
        default: hint = "This alarm cannot be reset from the screen.";
                 break;   /* 未知 clear_mode 按不可软件复位处理 */
    }

    if(hint) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, hint);
        lv_obj_set_style_text_color(lbl, user_color_text_dim, 0);
        lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        return;
    }

    /* RESET：ACTIVE 置灰（条件还在，MCU 会拒绝）；pending 置灰显示 Resetting… */
    const bool pending = cur->reset_pending;
    const bool enabled = !pending && cur->stage == UI_ALARM_STAGE_RTN_LATCHED;

    if(cur->stage == UI_ALARM_STAGE_ACTIVE) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl,
            "Resolve the condition first, then reset the alarm.");
        lv_obj_set_style_text_color(lbl, user_color_text_dim, 0);
        lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    }

    /* 稿 3209:9880：满宽主按钮，蓝色实心 + 8 圆角（稿上那 448 宽是被图示区
     * 撑出来的，图示区暂不画，按钮跟着内容区铺满更顺眼） */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_PCT(100), USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_margin_top(btn, USER_PAD_SM, 0);
    lv_obj_set_style_radius(btn, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_color_accent, 0);
    lv_obj_set_style_bg_opa(btn, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    if(enabled) {
        lv_obj_add_event_cb(btn, reset_btn_click_cb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, pending ? "Resetting..." : "Reset");
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_center(lbl);
}

/**
 * 按 g_drawer_* 重建抽屉内容。目标已不存在（复位成功转历史、历史被删）时
 * 自动关闭。current 条目转历史后如果抽屉还开着，切到对应语义即由下次点击走。
 */
void drawer_sync(void)
{
    /* 取目标数据 */
    const ui_alarm_entry_t         *cur  = nullptr;
    const ui_alarm_history_entry_t *hist = nullptr;
    if(g_drawer_src == ItemSrc::AlarmCurrent) {
        cur = ui_alarm_current_find(g_drawer_code);
        if(!cur) {
            drawer_close();
            return;
        }
    } else {
        hist = ui_alarm_history_at(g_drawer_hist_idx);
        if(!hist || hist->code != g_drawer_code) {
            drawer_close();
            return;
        }
    }

    drawer_close();   /* 简单起见整层重建（字段少，开销可忽略） */

    /* 整层建在 lv_layer_top()：稿上抽屉是满高的（y=0..800），自带的顶栏本就
     * 顶替右半屏那段状态栏。挂在页面里的话上边会被 content_panel_ 的 pad_top
     * 顶下来，且常驻设备条画在页面之上会压住抽屉头。
     * 代价（已确认可接受）：遮罩连侧栏一起盖住，开着抽屉点侧栏是先关抽屉。
     * layer_top 不随页面隐藏销毁，靠 page_user_message_exit() 收尾。 */
    g_drawer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_drawer);
    lv_obj_set_size(g_drawer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_drawer, 0, 0);
    lv_obj_set_style_bg_color(g_drawer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_drawer, LV_OPA_50, 0);
    lv_obj_clear_flag(g_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_drawer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_drawer, backdrop_click_cb, LV_EVENT_CLICKED, nullptr);

    /* 稿 3201:9694：右半屏满高面板，宽 632/1280 ≈ 49%（整屏为基准） */
    lv_obj_t *panel = lv_obj_create(g_drawer);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(49), LV_PCT(100));
    lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, user_color_panel, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(panel, kCardBorderOpa, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* 挡住 backdrop 的关闭点击 */
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    const char    *title  = cur ? cur->title : hist->title;
    const char    *body   = cur ? cur->body : hist->body;
    const uint16_t code   = g_drawer_code;
    const uint32_t detail = cur ? cur->detail : hist->detail;

    /* 顶栏（稿 3209:9873）：圆形返回钮 + "Notification Details"。
     * 高度取 USER_TOPBAR_H，标题才与左半屏常驻设备条上的设备名同一基线。 */
    lv_obj_t *head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, USER_TOPBAR_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(head, USER_PAD, 0);
    lv_obj_set_style_pad_ver(head, USER_PAD, 0);
    lv_obj_set_style_pad_column(head, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t back_sz = badge_size();
    lv_obj_t *back_btn = lv_button_create(head);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, back_sz, back_sz);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(back_btn, kCardBorderOpa, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(back_btn, USER_PAD_SM);
    lv_obj_add_event_cb(back_btn, backdrop_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_ico = lv_label_create(back_btn);
    lv_label_set_text(back_ico, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_ico, lv_color_white(), 0);
    lv_obj_set_style_text_opa(back_ico, LV_OPA_70, 0);
    lv_obj_set_style_text_font(back_ico, user_font_caption(), 0);
    lv_obj_center(back_ico);

    lv_obj_t *head_title = lv_label_create(head);
    lv_label_set_text(head_title, "Notification Details");
    lv_obj_set_style_text_color(head_title, user_color_text, 0);
    lv_obj_set_style_text_font(head_title, user_font_text(), 0);

    /* 正文区（稿 3204:9818）：标题 → 时刻 → 描述 → …… → 页脚按钮 */
    lv_obj_t *body_col = lv_obj_create(panel);
    lv_obj_remove_style_all(body_col);
    lv_obj_set_width(body_col, LV_PCT(100));
    lv_obj_set_flex_grow(body_col, 1);
    lv_obj_set_flex_flow(body_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(body_col, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(body_col, USER_PAD, 0);
    lv_obj_set_style_pad_row(body_col, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(body_col, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(body_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_col, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *t = lv_label_create(body_col);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, user_color_text, 0);
    lv_obj_set_style_text_font(t, user_font_text(), 0);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

    /* 状态胶囊：稿上没有，但报警必须一眼看出是否已解除 */
    if(cur) {
        if(cur->stage == UI_ALARM_STAGE_ACTIVE) {
            drawer_state_pill(body_col, "Active", kPillActive);
        } else {
            drawer_state_pill(body_col, "Recovered - awaiting reset", kPillLatched);
        }
    } else if(hist->is_event) {
        drawer_state_pill(body_col, "Event", kPillEvent);
    } else {
        drawer_state_pill(body_col, "Resolved", kColorResolved);
    }

    char buf[64];
    const int64_t trig_ts = cur ? cur->ts_ms : hist->ts_ms;
    format_relative(trig_ts, buf, sizeof(buf));
    lv_obj_t *when = lv_label_create(body_col);
    lv_label_set_text(when, buf);
    lv_obj_set_style_text_color(when, lv_color_white(), 0);
    lv_obj_set_style_text_opa(when, LV_OPA_30, 0);
    lv_obj_set_style_text_font(when, user_font_caption(), 0);

    /* 完整描述 */
    if(body && body[0]) {
        lv_obj_t *b = lv_label_create(body_col);
        lv_label_set_text(b, body);
        lv_obj_set_style_text_color(b, lv_color_white(), 0);
        lv_obj_set_style_text_opa(b, LV_OPA_50, 0);
        lv_obj_set_style_text_font(b, user_font_caption(), 0);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    }

    /* 页脚之前撑开：报警号那几行贴着按钮排在底部 */
    lv_obj_t *spacer = lv_obj_create(body_col);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_flex_grow(spacer, 1);

    /* 底部小字信息（稿上没有，报修要报号，留着但降到最小字号） */
    format_full(trig_ts, buf, sizeof(buf));
    drawer_info_row(body_col, "Occurred", buf);
    if(hist && !hist->is_event && hist->resolved_ts_ms > 0) {
        format_full(hist->resolved_ts_ms, buf, sizeof(buf));
        drawer_info_row(body_col, "Resolved", buf);
    }
    {
        const ui_alarm_catalog_entry_t *cat = ui_alarm_catalog_find(code);
        if(cat) {
            snprintf(buf, sizeof(buf), "%u (%s)",
                     static_cast<unsigned>(code), cat->macro);
        } else {
            snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(code));
        }
        drawer_info_row(body_col, "Alarm code", buf);
    }
    if(detail != 0) {
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(detail));
        drawer_info_row(body_col, "Detail code", buf);
    }

    build_drawer_footer(body_col, cur);
}

/* ============================================================================
 *  列表构建
 * ========================================================================= */

/** 右侧翻页圆钮（样式/行为与历史页一致）。 */
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

/** 操作胶囊钮（与历史页管理态同款：危险色实心 / 半透明白）。 */
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
    if(idx < 0 || idx >= kTabCount || kTabOrder[idx] == g_tab) {
        return;
    }
    g_tab = kTabOrder[idx];
    apply_tab_style();
    drawer_close();
    set_manage_mode(false);   /* 切 Tab 退出管理模式（同历史页） */
    /* 换了一批内容，滚动位置没有沿用的意义，回到顶部 */
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    rebuild_list();
}

/** 一个 Tab 项：文字贴顶 + 主色下划线贴底（下划线宽度取文字的一半，与稿一致）。 */
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

    /* 量出文字宽再把项宽写死：SIZE_CONTENT 容器里放百分比宽的子对象会互相依赖
     * （下划线按文字宽算，文字宽又决定容器宽）。同历史页。 */
    lv_obj_update_layout(item);
    const int32_t txt_w = lv_obj_get_width(lbl);
    lv_obj_set_width(item, txt_w);

    lv_obj_t *mark = lv_obj_create(item);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, txt_w / 2, 2);
    lv_obj_set_style_bg_color(mark, user_color_accent, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_align(mark, LV_ALIGN_BOTTOM_MID, 0, 0);

    g_tabs[idx].root  = item;
    g_tabs[idx].label = lbl;
    g_tabs[idx].mark  = mark;
}

/**
 * 顶部 Tab 行：左侧 All / Device / System + 右侧 Manage / 多选操作组。
 *
 * 结构与历史页 `build_tab_row()` 同构（稿 3033:5247 新版本身就是照那页画的）。
 * 右缘用 content_right_pad() 与列表让位量对齐，Manage 才和卡片右缘齐平——稿上
 * 那颗按钮摆在 x=1068，既不对齐卡片右缘也不对齐翻页钮，这里按工程惯例对齐卡片。
 */
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
    lv_obj_set_style_pad_right(row, content_right_pad(), 0);
    lv_obj_set_style_pad_column(row, USER_PAD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_add_flag(g_btn_manage, LV_OBJ_FLAG_HIDDEN);   /* 有可删条目才露面 */

    g_manage_bar = lv_obj_create(row);
    lv_obj_remove_style_all(g_manage_bar);
    lv_obj_set_size(g_manage_bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_manage_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_manage_bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_manage_bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(g_manage_bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_manage_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_manage_bar, LV_OBJ_FLAG_HIDDEN);

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

/** 日期分组标题（"Today" / "Aug 09, 2026"）。稿上比卡片标题小一档、压到 50% 白。 */
void build_group_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);
    lv_obj_set_style_text_font(lbl, user_font_caption(), 0);
    /* 组间距比卡间距大一档，两组才分得开（列表自身 pad_row 就是卡间距）。
     * 标题到组内首卡不额外留白，靠 pad_row 那一档即可——稿上两者同为 16。 */
    lv_obj_set_style_margin_top(lbl, USER_PAD_SM, 0);
}

/** 一条消息卡：[sev 图标] 标题+描述 | 时间 (Resolved) › ，可删的带左滑删除钮。 */
void build_entry(lv_obj_t *parent, const MsgItem &it)
{
    MsgCard *ctx = new MsgCard();
    ctx->src       = it.src;
    ctx->code      = it.code;
    ctx->hist_idx  = it.hist_idx;
    ctx->deletable = it.deletable;
    g_cards.push_back(ctx);

    /* 整条：管理模式下第一格是复选框（隐藏时 flex 跳过，卡片自动顶到最左） */
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrap, USER_PAD, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wrap, [](lv_event_t *e) {
        delete static_cast<MsgCard *>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, ctx);

    /* 复选框：空框 / 红底白勾，常态隐藏；不可删的条目压根不建 */
    if(it.deletable) {
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
        lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(check, [](lv_event_t *e) {
            toggle_card_selection(static_cast<MsgCard *>(lv_event_get_user_data(e)));
        }, LV_EVENT_CLICKED, ctx);
        ctx->check = check;

        lv_obj_t *mark = lv_label_create(check);
        lv_label_set_text(mark, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(mark, user_color_text, 0);
        lv_obj_set_style_text_font(mark, user_font_caption(), 0);
        lv_obj_center(mark);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
        ctx->check_mark = mark;
    }

    lv_obj_t *holder = lv_obj_create(wrap);
    lv_obj_remove_style_all(holder);
    lv_obj_set_flex_grow(holder, 1);
    lv_obj_set_height(holder, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    /* 左滑露出的删除圆钮（仅历史条目）。稿上是红色渐变 + 一圈浅白描边，
     * 右缘与卡片右缘齐平——卡片左移之后正好在腾出的空档里。 */
    if(it.deletable) {
        lv_obj_t *del = lv_button_create(holder);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, scroll_btn_size(), scroll_btn_size());
        lv_obj_add_flag(del, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(kDelGradA), 0);
        lv_obj_set_style_bg_grad_color(del, lv_color_hex(kDelGradB), 0);
        lv_obj_set_style_bg_grad_dir(del, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(del, 1, 0);
        lv_obj_set_style_border_color(del, lv_color_white(), 0);
        lv_obj_set_style_border_opa(del, LV_OPA_30, 0);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(del, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(del, del_click_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_t *icon = lv_label_create(del);
        lv_label_set_text(icon, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(icon, user_color_text, 0);
        lv_obj_set_style_text_font(icon, user_font_text(), 0);
        lv_obj_center(icon);
        ctx->del_btn = del;
    }

    /* 稿是白 4% 填充 + 1px 白 10% 描边。填充这里换成等效的不透明底（同历史页）：
     * 半透明的话，垫在卡片下层的删除圆钮会直接透出来——它靠卡片挡着，滑开才该露；
     * 而且半透明每帧要与背景混合，滚动时开销大。描边保留，玻璃边的观感还在。 */
    lv_obj_t *card = lv_obj_create(holder);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_color_panel, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, kCardBorderOpa, 0);
    /* 按下抬一档亮度（不能改 bg_opa：那会让卡片按下时透出下层删除钮） */
    lv_obj_set_style_bg_color(card, user_color_panel_alt, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card, USER_PAD, 0);
    lv_obj_set_style_pad_column(card, USER_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 跟手左滑：自己按 PRESSED/PRESSING/RELEASED 算位移（不用 LV_EVENT_GESTURE，
     * 原因见「左滑删除」一节）。PRESS_LOST 与 RELEASED 同一处收尾。 */
    lv_obj_add_event_cb(card, card_press_cb,    LV_EVENT_PRESSED,    ctx);
    lv_obj_add_event_cb(card, card_pressing_cb, LV_EVENT_PRESSING,   ctx);
    lv_obj_add_event_cb(card, card_release_cb,  LV_EVENT_RELEASED,   ctx);
    lv_obj_add_event_cb(card, card_release_cb,  LV_EVENT_PRESS_LOST, ctx);
    lv_obj_add_event_cb(card, card_click_cb,    LV_EVENT_CLICKED,    ctx);
    ctx->card = card;

    /* 等级徽标：实色圆角方块 + 白图标（与左下通知卡同款，稿 32 见方/18 图标）。
     * 配色与图标都取共用的四档映射，两处观感一致。 */
    const int32_t box_sz = badge_size();
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, box_sz, box_sz);
    lv_obj_set_style_bg_color(badge, ui_message_severity_color(it.sev), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, USER_RADIUS / 2, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    /* 子容器一律不可点：命中目标恒定是卡片本身，拖动才不用依赖事件冒泡 */
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(ui_user_make_icon_img(badge,
        ui_message_severity_icon(it.sev), box_sz * 9 / 16, lv_color_white()));

    /* 中列：标题 + 描述（两行截断） */
    lv_obj_t *mid = lv_obj_create(card);
    lv_obj_remove_style_all(mid);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(mid);
    lv_label_set_text(title, it.title);
    lv_obj_set_style_text_color(title, user_color_text, 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    if(it.body && it.body[0]) {
        lv_obj_t *body = lv_label_create(mid);
        lv_label_set_text(body, it.body);
        lv_obj_set_style_text_color(body, user_color_text_dim, 0);
        lv_obj_set_style_text_font(body, user_font_caption(), 0);
        lv_obj_set_width(body, LV_PCT(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        /* 长文最多两行：按字体行高箍住，再长的进抽屉看全文 */
        const int32_t lh = lv_font_get_line_height(user_font_caption());
        lv_obj_set_style_max_height(body, lh * 2 + USER_PAD_SM / 2, 0);
        lv_obj_add_flag(body, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 右列：Resolved 胶囊 + 时间 + › */
    if(it.resolved) {
        lv_obj_t *pill = lv_obj_create(card);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(kColorResolved), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_10, 0);
        lv_obj_set_style_pad_hor(pill, USER_PAD_SM / 2, 0);
        lv_obj_set_style_pad_ver(pill, 2, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *pl = lv_label_create(pill);
        lv_label_set_text(pl, "Resolved");
        lv_obj_set_style_text_color(pl, lv_color_hex(kColorResolved), 0);
        lv_obj_set_style_text_font(pl, user_font_caption(), 0);
    }

    char clk[16];
    format_clock(it.sort_ts, clk, sizeof(clk));
    lv_obj_t *time_lbl = lv_label_create(card);
    lv_label_set_text(time_lbl, clk);
    lv_obj_set_style_text_color(time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(time_lbl, LV_OPA_50, 0);
    lv_obj_set_style_text_font(time_lbl, user_font_caption(), 0);
    lv_obj_add_flag(time_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 稿上的 › 是一枚半透明圆钮而不是裸符号；点击仍由整张卡片受理 */
    if(it.has_detail) {
        const int32_t chev_sz = badge_size();
        lv_obj_t *chev = lv_obj_create(card);
        lv_obj_remove_style_all(chev);
        lv_obj_set_size(chev, chev_sz, chev_sz);
        lv_obj_set_style_radius(chev, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(chev, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(chev, kCardBorderOpa, 0);
        lv_obj_clear_flag(chev, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(chev, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *arrow = lv_label_create(chev);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, lv_color_white(), 0);
        lv_obj_set_style_text_opa(arrow, LV_OPA_70, 0);
        lv_obj_set_style_text_font(arrow, user_font_caption(), 0);
        lv_obj_center(arrow);
    }
}

/**
 * 重建整张列表。
 *
 * `lv_obj_clean` 会把滚动位置一起清零，所以删一条消息后视线会被弹回顶部——
 * 这里重建前记下滚动量、重建后放回去。必须先 `lv_obj_update_layout()` 让新内容
 * 的高度算出来，再按 `lv_obj_get_scroll_bottom()` 夹一下：删掉末尾几条后列表
 * 变矮，旧的滚动量会越界，直接 scroll_to 会把列表停在空白处。
 */
void rebuild_list(void)
{
    if(!g_list) {
        return;
    }
    const int32_t keep_scroll = lv_obj_get_scroll_y(g_list);

    g_cards.clear();          /* 对象随 lv_obj_clean 删除，上下文在 DELETE 回调里释放 */
    lv_obj_clean(g_list);

    const std::vector<MsgItem> items = collect_items();
    if(items.empty()) {
        if(g_empty) {
            lv_obj_remove_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
        }
        if(g_scroll_btns) {
            lv_obj_add_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
        }
        set_manage_mode(false);   /* 删空了自动退出管理模式 */
        return;
    }
    if(g_empty) {
        lv_obj_add_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_scroll_btns) {
        lv_obj_remove_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
    }

    char cur_group[32] = "";
    for(const MsgItem &it : items) {
        char grp[32];
        format_group(it.sort_ts, grp, sizeof(grp));
        if(std::strcmp(cur_group, grp) != 0) {
            std::snprintf(cur_group, sizeof(cur_group), "%s", grp);
            build_group_title(g_list, grp);
        }
        build_entry(g_list, it);
    }
    apply_manage_ui();   /* 重建后复选框/按钮组按当前模式重新落位 */

    if(keep_scroll > 0) {
        lv_obj_update_layout(g_list);
        /* 此刻列表停在顶部，scroll_bottom 就是最大可滚量 */
        const int32_t max_scroll = lv_obj_get_scroll_bottom(g_list);
        const int32_t y = (keep_scroll < max_scroll) ? keep_scroll : max_scroll;
        if(y > 0) {
            lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF);
        }
    }
}

/* ============================================================================
 *  观察者：置脏 + 异步重建（回调栈上可能正拿着将被删除的对象）
 * ========================================================================= */

void rebuild_async(void *)
{
    g_rebuild_queued = false;
    if(!g_active) {
        return;
    }
    rebuild_list();
    if(g_drawer) {
        drawer_sync();   /* 复位成功/条目消失时刷新或自动关抽屉 */
    }
}

void queue_rebuild(void)
{
    if(g_active && !g_rebuild_queued) {
        g_rebuild_queued = true;
        lv_async_call(rebuild_async, nullptr);
    }
}

void on_store_change(void)
{
    queue_rebuild();
}

void on_alarm_change(void)
{
    queue_rebuild();
}

} /* namespace */

void page_user_message_refresh(bool /*force*/)
{
    g_active = true;
    /* 进过一次这页就算全看过了：清未读、熄侧栏红点 */
    ui_message_mark_all_read();
    rebuild_list();
}

void page_user_message_exit(void)
{
    g_active = false;
    drawer_close();
    close_del_dlg();
    set_manage_mode(false);   /* 切页退出管理模式，回来时是干净的常态 */
    reset_all_cards();
}

lv_obj_t *page_user_message_create(lv_obj_t *parent)
{
    /* 稿上本页没有页面级大标题：设备信息条之下直接是第一个日期分组。 */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部 Tab 行 + 通栏分隔线（稿 3033:5247：线从侧栏右缘拉到屏幕最右） */
    build_tab_row(page);
    apply_tab_style();   /* make_tab_item 不置初始态，选中样式在这里落一次 */
    ui_user_make_divider_line(page, LV_PCT(100));

    g_list = lv_obj_create(page);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_width(g_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(g_list, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(g_list, USER_PAD, 0);
    lv_obj_set_style_pad_row(g_list, USER_PAD_SM, 0);
    /* 给右侧悬浮翻页钮让位（同历史页，两页卡片右缘因此对齐） */
    lv_obj_set_style_pad_right(g_list, content_right_pad(), 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);

    /* 空态：居中一句话 */
    g_empty = lv_label_create(page);
    lv_label_set_text(g_empty, "No messages");
    lv_obj_set_style_text_color(g_empty, user_color_text_dim, 0);
    lv_obj_set_style_text_font(g_empty, user_font_text(), 0);
    lv_obj_add_flag(g_empty, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_empty, LV_OBJ_FLAG_HIDDEN);

    /* 右侧竖排翻页悬浮层（稿 3033:5253/5256，与历史页同款） */
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
    lv_obj_add_flag(col, LV_OBJ_FLAG_HIDDEN);
    g_scroll_btns = col;

    ui_message_add_observer(on_store_change);
    ui_alarm_add_observer(on_alarm_change);
    return page;
}
