#include "page_user_message.hpp"

#include "page_manager.hpp"
#include "ui_alarm_catalog.hpp"
#include "ui_alarm_state.hpp"
#include "ui_message_store.hpp"
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
 * 布局（稿 6230:89616 / 6091:13405 / 6091:13612 / 6091:13847，浅色档）。
 * **本页不显示常驻设备条**：稿上顶部从屏顶起就是 Tab 行，设备条那条被不透明顶栏
 * 整条盖住，等价于不画，判据在 page_manager 的 show 那行（同历史页）。
 *
 *   ┌ All   Device   System                              [ All read ] ┐ ← 顶栏浮层
 *   │·····························································    │   y 0..124
 *   │ May 26, 2026                                                    │   y 140
 *   │ ┌──────────────────────────────────────────────────────────┐    │
 *   │ │ ▣  System update complete                          22:13 │    │
 *   │ │    The new version has been installed and applied.       │    │
 *   │ └──────────────────────────────────────────────────────────┘    │
 *   │ ┌───...（历史，可左滑露出右侧删除钮）        ┐   ┌──┐            │
 *   │                                                 │🗑│            │
 *   │                        Load More                └──┘            │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * 顶栏是**浮层**、列表是满高的：稿上列表从顶栏底下滚过去（6091:13847 顶部那张卡
 * 被切了一半），所以列表铺满整页、靠 pad_top 把首屏顶到顶栏之下，顶栏另铺一层
 * 不透明底 + 一道投影盖住。旧版那条通栏分隔线随之去掉。
 *
 * 卡片是白底实色 + 1px 白 10% 描边 + 一道浅投影，左侧一枚 48 见方的实色圆角徽标
 * （等级配色 + 白图标，与左下通知卡同款），右侧只有时间——**新稿去掉了那枚 ›**。
 * Tab 行与卡片列表横向同缘（都是 48/48），见「几何」一节的 content_pad_*。
 *
 * 数据三路混排（时间倒序，按自然日分组）：
 *   1. `ui_alarm_state` 当前 CONDITION（ACTIVE/RTN_LATCHED）——不可删，点开进抽屉；
 *   2. `ui_message_store` 非报警消息（设备事件 + 前端本地）——跟随生命周期自动撤，
 *      不可删，**点了没反应**（无抽屉，判据是 card_click_cb 里的 ItemSrc::StoreMsg）；
 *   3. `ui_alarm_state` 历史（Resolved CONDITION + EVENT）——可左滑删除，点开进抽屉。
 *   报警 EVENT 在 store 里也挂着一份（喂浮层用），列表里跳过以免与历史重复。
 *
 * 详情抽屉（右侧滑出，稿 6091:13612）：建在 `lv_layer_top()` 上，遮罩 60% 盖满整屏
 * （含侧栏），面板 632 宽贴右满高。挂页面里的话上边会被 content_panel_ 顶下来。
 * 返回行（chevron + Notification Details）+ 正文（标题 → 状态胶囊 → 相对时刻 →
 * 完整描述 → 448×280 处理步骤图位 → 底部小字 code/detail/时刻 → 主按钮）。
 * **稿上只画到「描述 → 图位 → 提示语 → 按钮」**，胶囊与小字那几行是稿外保留的，
 * 理由见各自处；图位真图还没出，先照稿画成占位灰块。
 * Reset 按钮三态（见 build_drawer_footer）：RESET+latched 可点 → Resetting…，
 * AUTO/POWER_CYCLE/NONE/EVENT 无按钮只给处理指引。复位成功（MCU 回 NORMAL 帧）
 * 后条目自动转历史、抽屉自动关。
 *
 * **多选批量删除已按新稿整体去掉**（稿四张列表帧都没画），删除只剩左滑单条，
 * 且仍只对历史条目开放：当前报警要等 MCU 解除、设备事件跟随后端生命周期，都删不掉。
 * 右侧那两枚悬浮翻页钮同样按稿去掉，翻页靠底部 Load More。
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
};

/** 卡片运行时上下文（左滑状态 + 点击路由），new/delete 随卡片走。 */
struct MsgCard {
    ItemSrc   src;
    uint16_t  code;
    int       hist_idx;
    bool      deletable;    /**< 仅历史条目可左滑删 */
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
lv_obj_t *g_empty     = nullptr;
lv_obj_t *g_drawer    = nullptr;   /**< 抽屉整层（backdrop + panel），NULL=未开 */
bool      g_active    = false;     /**< 本页在前台（refresh 置位 / exit 清零） */
bool      g_rebuild_queued = false;

TabItem   g_tabs[kTabCount];
MsgTab    g_tab        = MsgTab::All;

/* ---- 分页：初始只建最近 kRowsPerPage 条，其余靠底部「查看更多」逐批放出 ----
 * collect_items() 已按 sort_ts 降序，所以"最近的"就是前 g_visible_limit 条。
 * 重置只发生在切 Tab 与进页（见 on_tab_click / page_user_message_refresh），
 * **不能放进 rebuild_list()**：observer 的 rebuild_async() 也走那条路，
 * 放进去的话用户展开到第 50 条时来一条新消息，列表会当场折回 10 条。 */
constexpr size_t kRowsPerPage = 10;
size_t    g_visible_limit = kRowsPerPage;

std::vector<MsgCard *> g_cards;

/* 抽屉当前展示的目标（重建后据此刷新/关闭） */
ItemSrc  g_drawer_src;
uint16_t g_drawer_code;
int      g_drawer_hist_idx;

void rebuild_list(void);
void drawer_close(void);
void drawer_sync(void);
void apply_tab_style(void);

/* ============================================================================
 *  几何：稿是 1280×800，这里一律换算成 theme token，三档屏共用
 * ========================================================================= */

/**
 * 内容区左右内距，Tab 行与卡片列表**同缘**（稿 6230:89616：两者都是 x=176，
 * 侧栏占 128 故内距 48；右缘 1232，同样退 48）。
 *
 * 旧稿里这两者是错开的（Tab 行 48 / 列表 64、右侧还要给悬浮翻页钮让 112 与 160），
 * 新稿把翻页钮去掉之后四个量塌缩成同一个，所以只剩这一对函数。历史页的浅色稿也是
 * 同缘，两页共用 USER_HIST_PAD_L。
 */
int32_t content_pad_l(void)
{
    return USER_HIST_PAD_L > 0 ? USER_HIST_PAD_L : USER_PAD;
}

int32_t content_pad_r(void)
{
    return content_pad_l();
}

/** 顶栏浮层的高（稿 6230:89688 的内距 48/48/16 裹住一条 Tab 行，1280 档合计 124）。 */
int32_t chrome_h(void)
{
    return content_pad_l() + USER_HIST_TAB_H + USER_PAD_SM;
}

/**
 * 多行文本要补的行距。
 *
 * 稿上正文的行距是**写死**的（卡片 Poppins 20 号行距 28、抽屉 Noto Sans SC 18 号 1.5em），
 * 而 LVGL 预生成 Montserrat 的 line_height 只有字号的 1.1 倍左右（1280 档 20 号是 22、
 * 18 号是 21）。不补的话多行正文会挤成一坨，与稿差得最明显的就是卡片那条两行描述和
 * 抽屉里那段四行说明。
 *
 * 行距是稿上独立标注的排版属性，跟「不追字族字重」那条不冲突：字形追不了，行距能追。
 * 比值按 1280 档量的 28/22 给，其余档位同比——那两档本就没有稿。
 */
int32_t extra_line_space(const lv_font_t *f)
{
    const int32_t lh = lv_font_get_line_height(f);
    const int32_t want = lh * 28 / 22;
    return want > lh ? want - lh : 0;
}

/** 卡片左侧实色徽标方块边长（稿 6230:89622 是 48 见方，内含 24 的图标）。 */
int32_t badge_size(void)
{
    return USER_PRINT_BADGE_SZ * 3;
}

/** 详情抽屉宽（稿 6091:13766 是 632@1280，右贴屏边、满高）。 */
int32_t drawer_w(void)
{
    lv_display_t *d = lv_display_get_default();
    const int32_t disp_w = d ? lv_display_get_horizontal_resolution(d) : 1280;
    return disp_w * 632 / 1280;
}

/**
 * 抽屉内部的设计坐标换算（基准 632 宽 @1280）。
 *
 * 抽屉里的元素在稿上全是绝对定位，且那几个量（92 内距、21 列距、448 内容宽、
 * 280 图位高）没一个落得回现有 token 梯度，逐个开主题字段不值当，统一按抽屉宽折。
 * 同 page_user_home 的 card_px()。
 */
int32_t drawer_px(int32_t design_v)
{
    return design_v * drawer_w() / 632;
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

/**
 * 卡片左移量：露出删除钮 + 钮与卡片之间那道间隙。
 *
 * 稿 6091:13405 量到 104：卡片左缘从 176 滑到 72、右缘从 1232 滑到 1128，而删除钮
 * 固定在 1160..1232（距屏右 48，与内容区右缘同线）。104 = 钮径 72 + 净距 32。
 *
 * 基准是删除钮本身，**不是翻页钮**：两者本无关系，早先共用 scroll_btn_size() 只是
 * 碰巧同一个数，导致调翻页钮尺寸会连带改变左滑手感。翻页钮现已按新稿去掉。
 */
int32_t del_btn_size(void)
{
    return USER_LIGHT_BTN_SZ;
}

int32_t swipe_reveal_px(void)
{
    return del_btn_size() + USER_PAD_LG;
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
    if(!c->del_btn || !indev) {
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
    lv_obj_set_style_text_color(c, lv_color_black(), 0);
    lv_obj_set_style_text_opa(c, LV_OPA_30, 0);
    lv_obj_set_style_text_font(c, user_font_caption_sm(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_color(v, lv_color_black(), 0);
    lv_obj_set_style_text_opa(v, LV_OPA_30, 0);
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
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);
        lv_obj_set_style_text_font(lbl, user_font_text(), 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        return;
    }

    /* RESET：能不能点由 ui_alarm_reset_allowed() 统一裁决（三个复位入口共用），
     * 它按 reset_policy 区分 ACTIVE 下该放行还是该拦——别在这里另加判据。 */
    const bool pending = cur->reset_pending;
    const bool enabled = ui_alarm_reset_allowed(cur);

    if(cur->stage == UI_ALARM_STAGE_ACTIVE && !pending) {
        /* 同是 ACTIVE，放行与拦截要说不同的话：OWNER_TASK 类点了才会好转，
         * 让用户以为"得先修好"反而把人劝退了。 */
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, enabled
            ? "The condition is still present. Reset asks the machine to recover."
            : "Resolve the condition first, then reset the alarm.");
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);
        lv_obj_set_style_text_font(lbl, user_font_text(), 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    }

    /* 稿 6091:13694：满宽主按钮，448×72、#333145 实心、圆角 16。
     * 448 就是内容区的净宽，所以这里仍写 PCT(100)。 */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_PCT(100), drawer_px(72));
    lv_obj_set_style_radius(btn, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_light_text, 0);
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
    /* 稿 I6091:13694 的按钮文字是 20 */
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);
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
    lv_obj_set_style_bg_opa(g_drawer, LV_OPA_60, 0);   /* 稿 6091:13680 是 60% */
    lv_obj_clear_flag(g_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_drawer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_drawer, backdrop_click_cb, LV_EVENT_CLICKED, nullptr);

    /* 稿 6091:13766：右贴屏边的满高面板，632 宽、浅底、**无描边无投影**（旧稿那道
     * 左描边是深色档才需要的，浅底压在 60% 遮罩上自己就分得开）。
     * 顶内距 52、左右 92 都是稿值，见 drawer_px()。 */
    lv_obj_t *panel = lv_obj_create(g_drawer);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, drawer_w(), LV_PCT(100));
    lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, user_light_bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(panel, drawer_px(52), 0);
    lv_obj_set_style_pad_hor(panel, drawer_px(92), 0);
    /* 返回行底 80 到内容列顶 128，正好 48 */
    lv_obj_set_style_pad_row(panel, drawer_px(48), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* 挡住 backdrop 的关闭点击 */
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    const char    *title  = cur ? cur->title : hist->title;
    const char    *body   = cur ? cur->body : hist->body;
    const uint16_t code   = g_drawer_code;
    const uint32_t detail = cur ? cur->detail : hist->detail;

    /* 返回行（稿 6091:13762）：一枚 8×16 的 chevron + 20 的间距 + "Notification
     * Details"。旧稿那枚圆形返回钮在新稿里没了。chevron 是条 2px 描边的折线，工程
     * 没有这个矢量资源，取 Montserrat 自带的 LV_SYMBOL_LEFT——形状最接近的可用字形。 */
    lv_obj_t *head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(head, drawer_px(12), 0);   /* 稿的行内左内距 */
    lv_obj_set_style_pad_column(head, drawer_px(20), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    /* 整行都是返回热区（稿上那枚 8×16 的 chevron 单独当按钮太小） */
    lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(head, USER_PAD_SM);
    lv_obj_add_event_cb(head, backdrop_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *back_ico = lv_label_create(head);
    lv_label_set_text(back_ico, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_ico, lv_color_black(), 0);
    lv_obj_set_style_text_font(back_ico, user_font_text_sm(), 0);

    lv_obj_t *head_title = lv_label_create(head);
    lv_label_set_text(head_title, "Notification Details");
    lv_obj_set_style_text_color(head_title, lv_color_black(), 0);
    /* 稿 6091:13764 是「一级标题」20 */
    lv_obj_set_style_text_font(head_title, user_font_title(), 0);

    /* 正文区（稿 6091:13688）：宽 448、列距 21，标题 → 胶囊 → 时刻 → 描述 →
     * 图位 → 报警号 → 页脚按钮。稿上只画到「描述 → 图位 → 提示语 → 按钮」，
     * 胶囊与报警号那几行是稿外补的，理由见各自处。 */
    lv_obj_t *body_col = lv_obj_create(panel);
    lv_obj_remove_style_all(body_col);
    lv_obj_set_width(body_col, LV_PCT(100));
    lv_obj_set_flex_grow(body_col, 1);
    lv_obj_set_flex_flow(body_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(body_col, drawer_px(52), 0);
    lv_obj_set_style_pad_row(body_col, drawer_px(21), 0);
    lv_obj_set_style_bg_opa(body_col, LV_OPA_TRANSP, 0);
    /* 补回胶囊与报警号那几行之后内容会超过 800，这里可竖向滚 */
    lv_obj_set_scroll_dir(body_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_col, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *t = lv_label_create(body_col);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_black(), 0);
    /* 稿 6091:13689 是 28（Noto Sans SC Bold）。字重追不平，只对字号。 */
    lv_obj_set_style_text_font(t, user_font_dlg_title(), 0);
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
    lv_obj_set_style_text_color(when, lv_color_black(), 0);
    lv_obj_set_style_text_opa(when, LV_OPA_30, 0);
    lv_obj_set_style_text_font(when, user_font_text(), 0);   /* 稿 6091:13690 是 18 */

    /* 完整描述（稿 6091:13691 是 18 / 黑 50%） */
    if(body && body[0]) {
        lv_obj_t *b = lv_label_create(body_col);
        lv_label_set_text(b, body);
        lv_obj_set_style_text_color(b, lv_color_black(), 0);
        lv_obj_set_style_text_opa(b, LV_OPA_50, 0);
        lv_obj_set_style_text_font(b, user_font_text(), 0);
        /* 稿 6091:13691 的行距是 1.5em，四行说明全靠它撑开，见 extra_line_space() */
        lv_obj_set_style_text_line_space(b, extra_line_space(user_font_text()), 0);
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    }

    /**
     * 处理步骤图位（稿 6091:13692）：448×280、黑 10%、圆角 16 的空灰块。
     *
     * **稿上它就是个占位**——真图还没出。先照稿把这块留出来，图到了往这儿填即可；
     * 不画的话抽屉会比稿矮 300 左右，下面那枚主按钮的位置也就跟着不对了。
     */
    lv_obj_t *shot = lv_obj_create(body_col);
    lv_obj_remove_style_all(shot);
    lv_obj_set_size(shot, LV_PCT(100), drawer_px(280));
    lv_obj_set_style_radius(shot, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(shot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shot, LV_OPA_10, 0);
    lv_obj_clear_flag(shot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(shot, LV_OBJ_FLAG_CLICKABLE);

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

/**
 * 顶栏右侧那枚 `All read`（稿 6255:95773）：白底卡片钮，不是胶囊。
 *
 * **目前只画不接逻辑**——「进过一次消息页就全部标已读」的现有语义还在
 * page_user_message_refresh() 里，这枚钮的点击语义待产品定，定了再挂回调。
 */
lv_obj_t *build_all_read_btn(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_light_card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    /* 稿 0 2 6 rgba(0,0,0,0.04)：LVGL 的 shadow 只有一个「宽度」，取稿的模糊半径 6，
     * 再按 ofs_y 2 下沉。与卡片同一档投影。 */
    lv_obj_set_style_shadow_width(btn, 6, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 2, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(btn, 10, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);        /* 稿 24 */
    lv_obj_set_style_pad_ver(btn, USER_PAD_SM, 0);     /* 稿 16 */
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "All read");
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    /* 稿 22；与浅色档弹窗页脚钮同档，走 user_font_dlg_btn() */
    lv_obj_set_style_text_font(lbl, user_font_dlg_btn(), 0);
    lv_obj_center(lbl);
    return btn;
}

/**
 * 选中与否只差不透明度，**没有下划线**（稿 6230:89690：选中 #333145 实色 SemiBold、
 * 未选中同色 50% Medium）。共享构造 ui_user_make_tab_item 仍会建出那条下划线——它
 * 还服务着别处，这里建完就永久藏起来，与历史页同一处理。
 *
 * 字重追不平：稿分 SemiBold / Medium 两档，Montserrat 只有一档，只落地不透明度这半。
 */
void apply_tab_style(void)
{
    for(int i = 0; i < kTabCount; ++i) {
        const bool on = (g_tab == kTabOrder[i]);
        if(g_tabs[i].label) {
            lv_obj_set_style_text_color(g_tabs[i].label, user_light_text, 0);
            lv_obj_set_style_text_opa(g_tabs[i].label,
                on ? LV_OPA_COVER : LV_OPA_50, 0);
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
    if(idx < 0 || idx >= kTabCount || kTabOrder[idx] == g_tab) {
        return;
    }
    g_tab = kTabOrder[idx];
    apply_tab_style();
    drawer_close();
    reset_all_cards();                /* 切 Tab 收起没关的左滑 */
    g_visible_limit = kRowsPerPage;   /* 换了一批内容，展开进度不该带过去 */
    /* 换了一批内容，滚动位置没有沿用的意义，回到顶部 */
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    rebuild_list();
}

/** 一个 Tab 项（稿 6230:89692）：构造与命中区在 ui_user_make_tab_item，与历史页共用 */
void make_tab_item(lv_obj_t *parent, int idx)
{
    /* 稿 6230:89692 是 24，即 user_font_body 的 1280 档（与历史页浅色稿同档）。
     * 旧稿那版是 16，改稿后两页 Tab 统一到 24。 */
    ui_user_make_tab_item(parent, kTabTitles[idx], user_font_body(), on_tab_click,
                          reinterpret_cast<void *>(static_cast<intptr_t>(idx)),
                          &g_tabs[idx].root, &g_tabs[idx].label,
                          &g_tabs[idx].mark);
}

/**
 * 顶部 Tab 行：左侧 All / Device / System + 右侧一枚 All read。
 *
 * 结构与历史页 `build_tab_row()` 同构。新稿把左右内距统一成了 48（见 content_pad_l），
 * 不再像旧稿那样 Tab 行与卡片列表各缩各的——右侧那套 112/160 是给悬浮翻页钮让位的，
 * 钮已按新稿去掉。
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
    lv_obj_set_style_pad_left(row, content_pad_l(), 0);
    lv_obj_set_style_pad_right(row, content_pad_r(), 0);
    /* Tab 项间距：稿 6230:89690 的三项在 490 宽的组里 gap 48 */
    lv_obj_set_style_pad_column(row, USER_HIST_PAD_L, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < kTabCount; ++i) {
        make_tab_item(row, i);
    }

    /* 弹簧：把 All read 推到行尾 */
    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    build_all_read_btn(row);
    return row;
}

/** 日期分组标题（"Today" / "Aug 09, 2026"，稿 6230:89619）。 */
void build_group_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    /* 稿是 #000000 @50%，不是卡片正文那档 #111111 @50%——两者只差 17/255，但稿上
     * 确实分开写了，照给。 */
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);
    /* 稿 6230:89619 是 20。旧稿那版是 16，改稿后与卡片标题同号。 */
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);
    /* 组间距比卡间距大一档，两组才分得开（列表自身 pad_row 就是卡间距 16）。
     * 稿上组与组之间是 32（首组 y=140、内容高 296、次组 y=468），这里 16+16 凑出来；
     * 标题到组内首卡不额外留白，靠 pad_row 那一档即可——稿上同为 16。 */
    lv_obj_set_style_margin_top(lbl, USER_PAD_SM, 0);
}

/** 点「查看更多」：再放出一批，列表原地重建。
 *  rebuild_list() 末尾会沿用当前滚动位置，所以新内容出现在下方、视口不跳。 */
void on_load_more(lv_event_t *)
{
    g_visible_limit += kRowsPerPage;
    rebuild_list();
}

/**
 * 底部「Load More」（稿 6091:13921 的「查看更多」）。
 *
 * 稿上只是一段 16px 黑 50% 的纯文字，没有底色/描边。但 64×24 的文字在触摸屏上太小，
 * 这里外面包一层整行宽、USER_BTN_PRIMARY_H 高的透明按钮当热区，按压时把文字提到不透明
 * 当反馈——视觉与稿一致，可点范围放大。
 *
 * 稿 6101:14155 画的那个**加载圈有意未实现**：三路消息全在内存（store 合并 + 排序，
 * 无 IO、无异步），点一次就是同步多建几张卡；而 LVGL 单线程，同步重建期间画面根本
 * 不刷新，转圈一帧都不会转。即便照历史页 append_list_build_chunk() 改成异步分块，
 * 每块 8 张、追 10 张也只有两块约 24ms，仍是 1~2 帧。等消息真接了云端、展开变成
 * 回调式再启用它。
 */
void build_load_more(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_load_more, LV_EVENT_CLICKED, nullptr);
    /* 文字样式挂在**按钮**上而不是 label 上：LVGL 的状态不向子节点传播，写在 label 上
     * 的 LV_STATE_PRESSED 永远不会命中（label 自己不会被按）。text_* 是可继承属性，
     * 挂在按钮上时 label 会沿父链取值，于是跟着按钮的按压态一起变。 */
    lv_obj_set_style_text_color(btn, lv_color_black(), 0);
    lv_obj_set_style_text_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_text_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_font(btn, user_font_text_sm(), 0);   /* 稿 16，见上面说明 */

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Load More");
    lv_obj_center(lbl);
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

    /* 整条：卡片 + 垫在它右侧空档里的删除钮。多选态没了，第一格不再有复选框，
     * 但这层壳还留着——删除钮要浮在卡片之外（稿上钮在卡片右缘再外 32），得有个
     * 不随卡片平移的父节点兜着。 */
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wrap, [](lv_event_t *e) {
        delete static_cast<MsgCard *>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, ctx);

    lv_obj_t *holder = lv_obj_create(wrap);
    lv_obj_remove_style_all(holder);
    lv_obj_set_flex_grow(holder, 1);
    lv_obj_set_height(holder, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    /**
     * 左滑露出的删除钮（仅历史条目，稿 6091:13408）：72 见方、**圆角 24 的方钮，
     * 不是圆钮**。红渐变 + 一圈浅白描边。
     *
     * 位置仍是贴住 holder 右缘（`RIGHT_MID, 0`）：稿上钮在 1160..1232，而内容区右缘
     * 正好也是 1232，两者同线。卡片左滑 104 后右缘退到 1128，钮与它之间自然空出
     * 104 − 72 = 32 的净距，与稿一致。别改成 `LV_ALIGN_OUT_RIGHT_MID`——`lv_obj_align`
     * 的 OUT_* 是给 `lv_obj_align_to()` 那套参照物用的，对父节点用会把钮甩到左边去。
     */
    if(it.deletable) {
        const int32_t del_sz = del_btn_size();
        lv_obj_t *del = lv_button_create(holder);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, del_sz, del_sz);
        lv_obj_add_flag(del, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_style_radius(del, USER_LIGHT_RADIUS, 0);
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
        lv_obj_set_style_text_opa(icon, LV_OPA_90, 0);   /* 稿 white @90% */
        lv_obj_set_style_text_font(icon, user_font_text(), 0);
        lv_obj_center(icon);
        ctx->del_btn = del;
    }

    /* 稿 6230:89621：白底实色 + 1px 白 10% 描边 + 0 2 6 rgba(0,0,0,.04) 投影。
     * 白底压白描边等于看不见，与浅色档弹窗那道外描边同一个情况，稿上就这么画的，
     * 照留；真正撑出边界的是那道投影。
     * 底必须不透明：垫在卡片下层的删除钮靠卡片挡着，半透明会直接透出来。 */
    lv_obj_t *card = lv_obj_create(holder);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, kCardBorderOpa, 0);
    lv_obj_set_style_shadow_width(card, 6, 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, 10, 0);   /* 稿 4% ≈ 10/255 */
    /* 按下压一档灰（不能改 bg_opa：那会让卡片按下时透出下层删除钮） */
    lv_obj_set_style_bg_color(card, user_light_more_bg, LV_STATE_PRESSED);
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
    /* 稿 6230:89622 的圆角是 8，即 USER_PAD_SM/2（1280 档正好）。 */
    lv_obj_set_style_radius(badge, USER_PAD_SM / 2, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    /* 子容器一律不可点：命中目标恒定是卡片本身，拖动才不用依赖事件冒泡 */
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    /* 稿 48 的盒里是 24 的图标，正好一半 */
    /* 取图与显示用同一个 ic_sz：severity_icon 按这个边长挑原生尺寸的那一档，
     * 对上了就不会走缩放。稿 6230:89622 是 48 徽标内含 24 图标。 */
    const int32_t ic_sz = box_sz / 2;
    lv_obj_center(ui_user_make_icon_img(badge,
        ui_message_severity_icon(it.sev, ic_sz), ic_sz, lv_color_white()));

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

    /* 标题与正文稿上**同为 20**（6230:89630/89631 的命名样式「一级标题」/「一级正文」），
     * 同色 #111111，只靠字重（SemiBold / Regular）和 50% 透明度分层；Montserrat 单字重，
     * 字重那半做不出来，只落地「同号 + 透明度」。与左下通知栈同一处理，见该节。
     * 注意本页这套 20 与历史页卡片那套 16 不是一个尺度，稿上就不同，别互相看齐。 */
    lv_obj_t *title = lv_label_create(mid);
    lv_label_set_text(title, it.title);
    lv_obj_set_style_text_color(title, user_light_value, 0);
    lv_obj_set_style_text_font(title, user_font_title(), 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    if(it.body && it.body[0]) {
        lv_obj_t *body = lv_label_create(mid);
        lv_label_set_text(body, it.body);
        lv_obj_set_style_text_color(body, user_light_value, 0);
        lv_obj_set_style_text_opa(body, LV_OPA_50, 0);
        lv_obj_set_style_text_font(body, user_font_title(), 0);
        lv_obj_set_width(body, LV_PCT(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        /* 稿 6230:89631 的行距是 28（字号 20），补上差额，见 extra_line_space() */
        const int32_t ls = extra_line_space(user_font_title());
        lv_obj_set_style_text_line_space(body, ls, 0);
        /* 长文最多两行：按字体行高箍住，再长的进抽屉看全文。
         * 行高基准必须与上面的字体**和行距**一致，否则会截在半行上。
         * 稿上长正文是整段铺开（3~4 行）、卡片随之长高，这里仍箍两行：200 条历史
         * 全铺开会让列表长到没法翻，且点开抽屉就能看全文。 */
        const int32_t lh = lv_font_get_line_height(user_font_title());
        lv_obj_set_style_max_height(body, lh * 2 + ls + USER_PAD_SM / 2, 0);
        lv_obj_add_flag(body, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 右列：Resolved 胶囊 + 时间（稿上没有 ›，见下） */
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

    /* 稿 6230:89632 的时间是 Futura 20、同正文那档 #111111 @50%。Futura 追不到，
     * 只对字号（见 UI.md 本页的字体一节）。24 小时制：稿上写的 "22:13PM" 是错的，
     * 24 小时制不带 PM，这里照 format_clock 的 %H:%M 出。 */
    char clk[16];
    format_clock(it.sort_ts, clk, sizeof(clk));
    lv_obj_t *time_lbl = lv_label_create(card);
    lv_label_set_text(time_lbl, clk);
    lv_obj_set_style_text_color(time_lbl, user_light_value, 0);
    lv_obj_set_style_text_opa(time_lbl, LV_OPA_50, 0);
    lv_obj_set_style_text_font(time_lbl, user_font_title(), 0);
    lv_obj_add_flag(time_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 新稿去掉了右端那枚 › 圆钮（旧稿 3058 有）：卡片可点这件事不再有视觉提示，
     * 稿上四张列表帧都没画它。随之去掉的还有 MsgItem::has_detail——它只喂过那枚 ›，
     * 「点了有没有抽屉」的判据一直是 card_click_cb 里的 ItemSrc::StoreMsg。 */
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
        return;
    }
    if(g_empty) {
        lv_obj_add_flag(g_empty, LV_OBJ_FLAG_HIDDEN);
    }

    /* 只建前 g_visible_limit 条（items 已按时间降序，即最近的那些）。截断可能落在
     * 某个日期组中间，但分组标题是"组名变了才插"，所以最后一组不完整也不会漏标题。 */
    const size_t shown = (items.size() < g_visible_limit) ? items.size()
                                                          : g_visible_limit;
    char cur_group[32] = "";
    for(size_t i = 0; i < shown; i++) {
        const MsgItem &it = items[i];
        char grp[32];
        format_group(it.sort_ts, grp, sizeof(grp));
        if(std::strcmp(cur_group, grp) != 0) {
            std::snprintf(cur_group, sizeof(cur_group), "%s", grp);
            build_group_title(g_list, grp);
        }
        build_entry(g_list, it);
    }
    /* 还有没放出来的就在末尾挂一行 Load More；放完了自然不建，等于自动隐藏。
     * 它不进 g_cards，所以左滑那套手势天然碰不到它。 */
    if(items.size() > shown) {
        build_load_more(g_list);
    }

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
    /* 用户特地来看消息，先跟 MCU 对一次账：单条上报错过就不再补发，本地这份
     * 可能已经过期。请求侧自带节流，和 30s 轮询撞上也不会重复发；快照回来时
     * 观察者会把列表重建一遍。 */
    ui_alarm_request_snapshot("message page");
    /* 进过一次这页就算全看过了：清未读、熄侧栏红点 */
    ui_message_mark_all_read();
    g_visible_limit = kRowsPerPage;   /* 每次进页都从最近 10 条重新看 */
    rebuild_list();

    /* 撤掉 mark_all_read() 顺手排下的那次异步重建。
     * 它有未读时会 notify_observers()（红点要靠这个通知才灭，不能省），一路转到
     * on_store_change() → queue_rebuild()。而上面刚亲手建过最新列表，且本页从不读
     * unread（未读只驱动侧栏红点，见 ui_app.cpp 的 sync_message_badge），那一遍产出
     * 逐像素相同、纯属浪费——进页正是列表最长的时刻，白建一遍代价不小。
     * 只撤这一次排队，异步机制对其它真实变更照旧生效。 */
    lv_async_call_cancel(rebuild_async, nullptr);
    g_rebuild_queued = false;
}

void page_user_message_exit(void)
{
    g_active = false;
    drawer_close();
    reset_all_cards();   /* 切页收起左滑，回来时是干净的常态 */
}

lv_obj_t *page_user_message_create(lv_obj_t *parent)
{
    /* 稿上本页没有页面级大标题，也没有常驻设备条（见 page_manager 的 show 判据）：
     * 页面从屏顶起就是 Tab 行。
     *
     * **顶栏是浮层、列表是满高的**，不是上下并排的两格：稿 6091:13847 顶部那张卡被
     * 顶栏切掉了一半，说明列表是从顶栏底下滚过去的。所以列表铺满整页、用 pad_top 把
     * 首屏内容顶到顶栏之下（padding 会跟着一起滚，滚上去就压到顶栏底下），顶栏另铺
     * 一层不透明底盖住。旧版那条通栏分隔线随之去掉，改由顶栏的投影分隔。 */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    g_list = lv_obj_create(page);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_size(g_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(g_list, content_pad_l(), 0);
    lv_obj_set_style_pad_right(g_list, content_pad_r(), 0);
    /* 顶内距只给顶栏那 124：顶栏到首个日期标题之间那 16 由标题自己的 margin_top
     * 出（build_group_title 给的，组与组之间也靠它），两处加起来正好是稿的 y=140。
     * 这里再加一次的话首组会掉到 156。 */
    lv_obj_set_style_pad_top(g_list, chrome_h(), 0);
    lv_obj_set_style_pad_bottom(g_list, USER_PAD, 0);
    lv_obj_set_style_pad_row(g_list, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);

    /* 顶栏浮层：不透明底把滚上来的卡片盖住，底缘一道投影（稿 6265:24099
     * 的 0 2 6 rgba(0,0,0,0.05)）。建在列表之后，故画在它之上。 */
    lv_obj_t *chrome = lv_obj_create(page);
    lv_obj_remove_style_all(chrome);
    lv_obj_add_flag(chrome, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(chrome, LV_PCT(100), chrome_h());
    lv_obj_align(chrome, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(chrome, user_light_bg, 0);
    lv_obj_set_style_bg_opa(chrome, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(chrome, 6, 0);
    lv_obj_set_style_shadow_ofs_y(chrome, 2, 0);
    lv_obj_set_style_shadow_color(chrome, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(chrome, 13, 0);   /* 稿 5% ≈ 13/255 */
    lv_obj_set_style_pad_top(chrome, USER_HIST_PAD_L, 0);   /* 稿 48 */
    lv_obj_clear_flag(chrome, LV_OBJ_FLAG_SCROLLABLE);

    build_tab_row(chrome);
    apply_tab_style();   /* make_tab_item 不置初始态，选中样式在这里落一次 */

    /* 空态：居中一句话 */
    g_empty = lv_label_create(page);
    lv_label_set_text(g_empty, "No messages");
    lv_obj_set_style_text_color(g_empty, user_light_value, 0);
    lv_obj_set_style_text_opa(g_empty, LV_OPA_50, 0);
    lv_obj_set_style_text_font(g_empty, user_font_text(), 0);
    lv_obj_add_flag(g_empty, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_empty, LV_OBJ_FLAG_HIDDEN);

    ui_message_add_observer(on_store_change);
    ui_alarm_add_observer(on_alarm_change);
    return page;
}
