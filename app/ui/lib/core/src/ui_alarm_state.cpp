#include "ui_alarm_state.hpp"

#include "lvgl_wrap.hpp"
#include "rs_log.hpp"
#include "ui_alarm_catalog.hpp"
#include "ui_message_store.hpp"
#include "ui_msg_proxy.hpp"
#include "upc_contract.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace {

constexpr int      kCurrentCap      = 80;   /* CONDITION 全集 66 条，留余量 */
constexpr int      kMaxObservers    = 4;
constexpr uint32_t kResetTimeoutMs  = 10000; /* 复位无回报自动解锁按钮 */
constexpr uint32_t kEventDismissMs  = 5000;  /* EVENT 蓝卡自动从浮层撤下 */

/* ---- 快照攒帧 ----
 * 快照按 32 条一组分片，收齐 word_count 帧才能对齐；用半套去覆盖当前表会把没
 * 收到那几组的报警误判成已解除。8 组 = 256 条，远超可预见规模。 */
constexpr int      kSnapMaxWords    = 8;
constexpr uint32_t kSnapAssembleMs  = 2000; /* 攒帧超时：宁可丢一批也不用半套 */

/* 消息中心自己的落盘目录，独立于 /home/media——报警历史不是媒体，早先挂在
 * media 下只是为了借那边的路径宏。目录由 ui_alarm_state_init() 负责创建：UI 起得
 * 可能比 Media 早，不能指望别的进程先建好。 */
constexpr const char *kMessageDir     = "/home/messages";
constexpr const char *kHistoryPath    = "/home/messages/alarm_history.jsonl";
/* 先写 .tmp 再 rename，掉电不会留下半截文件 */
constexpr const char *kHistoryPathTmp = "/home/messages/alarm_history.jsonl.tmp";

/* ---- 当前 CONDITION：固定槽位，不搬移 ----
 * ui_message_store 只存 title/body 指针，条目存续期内地址必须稳定。 */
struct CurrentSlot {
    bool             used;
    ui_alarm_entry_t e;
};
CurrentSlot g_current[kCurrentCap];

/* EVENT 文案槽：按 code 复用（store 同 code 只挂一条，重复触发原位覆写）。 */
struct EventText {
    uint16_t code;
    bool     used;
    char     title[UI_ALARM_TITLE_LEN];
    char     body[UI_ALARM_BODY_LEN];
};
constexpr int kEventTextCap = 16;
EventText g_event_text[kEventTextCap];

ui_alarm_history_entry_t g_history[UI_ALARM_HISTORY_CAP];
int g_history_count = 0;   /* g_history[0] 最新 */

ui_alarm_observer_t g_observers[kMaxObservers] = { nullptr };
bool g_inited        = false;
bool g_print_session = false;

/** 会话期抑制浮层卡的两个打印阻断码（结果页已展示失败原因 + Reset）。 */
bool session_suppressed(uint16_t code)
{
    return g_print_session && (code == 1124 || code == 1125);
}

/* 显示序（severity 降序、同级新在前）的槽位下标表 */
int g_order[kCurrentCap];
int g_order_count = 0;

/* 正在攒的这一批快照 */
struct SnapAssembly {
    bool     busy;                    /**< 已收到至少一帧、尚未收齐 */
    uint8_t  word_count;              /**< 本批总帧数（首帧确定） */
    uint32_t got_mask;                /**< 已收到哪些 word_index（按位） */
    uint32_t active[kSnapMaxWords];
    uint32_t latched[kSnapMaxWords];
    int64_t  started_ms;              /**< 首帧时刻，用于超时作废 */
};
SnapAssembly g_snap{};

int64_t wall_ms(void)
{
    return static_cast<int64_t>(time(nullptr)) * 1000;
}

/* ---- 文案组合：字典优先，查不到降级显示原始编号（固件可能比字典新） ----
 *
 * 假报警的 [SIM] 前缀在这里加：浮层卡、消息页列表、详情抽屉、历史读的都是这份
 * 文案，一处加完四处都有，渲染代码不用动。 */

void compose_text(uint16_t code, uint32_t detail, bool simulated,
                  char *title, size_t title_len, char *body, size_t body_len)
{
    const char *sim = simulated ? "[SIM] " : "";
    const ui_alarm_catalog_entry_t *cat = ui_alarm_catalog_find(code);
    if(cat) {
        snprintf(title, title_len, "%s%s", sim, cat->title);
    } else {
        snprintf(title, title_len, "%sUnknown alarm #%u", sim,
                 static_cast<unsigned>(code));
    }

    if(detail != 0) {
        const ui_alarm_detail_entry_t *det = ui_alarm_detail_find(detail);
        if(det) {
            snprintf(body, body_len, "%s", det->text);
        } else {
            snprintf(body, body_len, "Detail code %u",
                     static_cast<unsigned>(detail));
        }
    } else if(cat && cat->desc[0]) {
        snprintf(body, body_len, "%s", cat->desc);
    } else if(!cat) {
        snprintf(body, body_len, "Not in the alarm dictionary (%s)",
                 ui_alarm_catalog_version);
    } else {
        body[0] = '\0';
    }
}

/* ---- 观察者 / 排序 ---- */

void rebuild_order(void)
{
    g_order_count = 0;
    for(int i = 0; i < kCurrentCap; i++) {
        if(!g_current[i].used) {
            continue;
        }
        int pos = g_order_count;
        while(pos > 0) {
            const ui_alarm_entry_t &a = g_current[i].e;
            const ui_alarm_entry_t &b = g_current[g_order[pos - 1]].e;
            const bool before = (a.severity != b.severity)
                                    ? (a.severity > b.severity)
                                    : (a.ts_ms > b.ts_ms);
            if(!before) {
                break;
            }
            g_order[pos] = g_order[pos - 1];
            pos--;
        }
        g_order[pos] = i;
        g_order_count++;
    }
}

void notify_observers(void)
{
    rebuild_order();
    for(int i = 0; i < kMaxObservers; i++) {
        if(g_observers[i]) {
            g_observers[i]();
        }
    }
}

/* ---- 历史落盘：一行一条 JSON，只存原始字段（文案由字典重生成） ---- */

void history_save(void)
{
    FILE *f = fopen(kHistoryPathTmp, "w");
    if(!f) {
        /* 不能静默：写不进去等于历史整段丢失，而调用方（转历史、删除）拿不到
         * 返回值也无从补救，只能靠日志暴露。 */
        UPC_LOG_ALARM_STATE(Error, "history save open %s failed: %s",
                            kHistoryPathTmp, strerror(errno));
        return;
    }
    for(int i = 0; i < g_history_count; i++) {
        const ui_alarm_history_entry_t &h = g_history[i];
        if(h.simulated) {
            continue;   /* 调试注入的假报警不落盘，重启即净 */
        }
        fprintf(f,
                "{\"code\":%u,\"sev\":%u,\"evt\":%u,\"detail\":%u,"
                "\"ts\":%" PRId64 ",\"rts\":%" PRId64 "}\n",
                static_cast<unsigned>(h.code), static_cast<unsigned>(h.severity),
                static_cast<unsigned>(h.is_event),
                static_cast<unsigned>(h.detail), h.ts_ms, h.resolved_ts_ms);
    }
    fclose(f);
    rename(kHistoryPathTmp, kHistoryPath);
}

void history_load(void)
{
    g_history_count = 0;
    FILE *f = fopen(kHistoryPath, "r");
    if(!f) {
        return;
    }
    char line[256];
    while(g_history_count < UI_ALARM_HISTORY_CAP && fgets(line, sizeof(line), f)) {
        unsigned code = 0, sev = 0, evt = 0, detail = 0;
        long long ts = 0, rts = 0;
        if(sscanf(line,
                  "{\"code\":%u,\"sev\":%u,\"evt\":%u,\"detail\":%u,"
                  "\"ts\":%lld,\"rts\":%lld}",
                  &code, &sev, &evt, &detail, &ts, &rts) != 6) {
            continue;
        }
        ui_alarm_history_entry_t &h = g_history[g_history_count++];
        std::memset(&h, 0, sizeof(h));
        h.code           = static_cast<uint16_t>(code);
        h.severity       = static_cast<uint8_t>(sev);
        h.is_event       = static_cast<uint8_t>(evt);
        h.detail         = detail;
        h.ts_ms          = ts;
        h.resolved_ts_ms = rts;
        /* 落盘的都是真报警（假的在 history_save 里被跳过） */
        compose_text(h.code, h.detail, false, h.title, sizeof(h.title),
                     h.body, sizeof(h.body));
    }
    fclose(f);
}

void history_prepend(uint16_t code, uint8_t severity, bool is_event,
                     uint32_t detail, int64_t ts_ms, int64_t resolved_ts_ms,
                     bool simulated)
{
    if(g_history_count >= UI_ALARM_HISTORY_CAP) {
        g_history_count = UI_ALARM_HISTORY_CAP - 1;   /* 丢最旧 */
    }
    for(int i = g_history_count; i > 0; i--) {
        g_history[i] = g_history[i - 1];
    }
    g_history_count++;

    ui_alarm_history_entry_t &h = g_history[0];
    std::memset(&h, 0, sizeof(h));
    h.code           = code;
    h.severity       = severity;
    h.is_event       = is_event ? 1 : 0;
    h.detail         = detail;
    h.ts_ms          = ts_ms;
    h.resolved_ts_ms = resolved_ts_ms;
    h.simulated      = simulated;
    compose_text(code, detail, simulated, h.title, sizeof(h.title),
                 h.body, sizeof(h.body));
    history_save();
}

/* ---- 当前表槽位 ---- */

int slot_find(uint16_t code)
{
    for(int i = 0; i < kCurrentCap; i++) {
        if(g_current[i].used && g_current[i].e.code == code) {
            return i;
        }
    }
    return -1;
}

int slot_alloc(void)
{
    for(int i = 0; i < kCurrentCap; i++) {
        if(!g_current[i].used) {
            return i;
        }
    }
    return -1;
}

/* ---- 定时器：复位超时 / EVENT 自动撤浮层 ----
 * user_data 直接塞 code（uintptr），不挂堆内存。 */

void reset_timeout_cb(lv_timer_t *t)
{
    /* repeat_count=1 的一次性定时器，回调后由 LVGL 自行删除 */
    const uint16_t code =
        static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(t)));
    const int i = slot_find(code);
    if(i >= 0 && g_current[i].e.reset_pending) {
        /* MCU 没回报：大概率异常条件仍在或帧丢失，恢复按钮让用户可重试 */
        g_current[i].e.reset_pending = false;
        notify_observers();
    }
}

void event_dismiss_cb(lv_timer_t *t)
{
    const uint16_t code =
        static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(t)));
    ui_message_dismiss(code);
}

void arm_oneshot(lv_timer_cb_t cb, uint32_t period_ms, uint16_t code)
{
    lv_timer_t *t = lv_timer_create(cb, period_ms,
        reinterpret_cast<void *>(static_cast<uintptr_t>(code)));
    lv_timer_set_repeat_count(t, 1);
}

/* ---- EVENT 文案槽 ---- */

EventText *event_text_slot(uint16_t code)
{
    EventText *free_slot = nullptr;
    for(auto &s : g_event_text) {
        if(s.used && s.code == code) {
            return &s;
        }
        if(!s.used && !free_slot) {
            free_slot = &s;
        }
    }
    if(!free_slot) {
        free_slot = &g_event_text[0];   /* 槽满复用最老的一格（16 格 > EVENT 全集） */
    }
    free_slot->used = true;
    free_slot->code = code;
    return free_slot;
}

/* ---- 报文处理（UI 线程） ----
 *
 * g_inject_simulated 只在 ui_alarm_debug_inject() 调用期间为真：两个 on_alarm_*
 * 是真假报文的共同入口，用它给条目打标而不是另开一条注入专用通路，注入才真的
 * "走真实路径"。 */

bool g_inject_simulated = false;

void on_alarm_event(uint16_t code, uint8_t severity, uint32_t detail)
{
    const int64_t now = wall_ms();
    history_prepend(code, severity, true, detail, now, 0, g_inject_simulated);

    EventText *txt = event_text_slot(code);
    compose_text(code, detail, g_inject_simulated, txt->title, sizeof(txt->title),
                 txt->body, sizeof(txt->body));
    ui_message_push_ex(code, ui_alarm_severity_to_msg(severity),
                       txt->title, txt->body, now, true);
    /* EVENT 是看过即走的消息：浮层挂几秒自动撤，列表里还留着 */
    arm_oneshot(event_dismiss_cb, kEventDismissMs, code);
    notify_observers();
}

void on_alarm_condition(const middleware::AlarmReportPayload &p)
{
    const bool active  = (p.state_mask & middleware::kAlarmStateActive) != 0;
    const bool latched = (p.state_mask & middleware::kAlarmStateLatched) != 0;
    const int64_t now  = wall_ms();

    int i = slot_find(p.alarm_code);

    if(!active && !latched) {
        /* NORMAL：撤卡 + 转历史。不在表中的 NORMAL 帧（如复位一条从未见过的
         * 报警）没有触发时刻可记，直接忽略。 */
        if(i < 0) {
            return;
        }
        const ui_alarm_entry_t &e = g_current[i].e;
        ui_message_retract(e.code);
        history_prepend(e.code, e.severity, false, e.detail, e.ts_ms, now,
                        e.simulated);
        g_current[i].used = false;
        notify_observers();
        return;
    }

    if(i < 0) {
        i = slot_alloc();
        if(i < 0) {
            return;   /* 槽满（不应发生：容量 > CONDITION 全集） */
        }
        std::memset(&g_current[i], 0, sizeof(g_current[i]));
        g_current[i].used   = true;
        g_current[i].e.code = p.alarm_code;
        g_current[i].e.ts_ms = now;
    }

    ui_alarm_entry_t &e = g_current[i].e;
    e.severity      = p.severity;
    e.clear_mode    = p.clear_mode;
    e.detail        = p.detail;
    e.stage         = active ? UI_ALARM_STAGE_ACTIVE : UI_ALARM_STAGE_RTN_LATCHED;
    e.reset_pending = false;   /* 收到该 code 的任何新帧都视为复位请求已被处理 */
    /* 跟着最新一帧的来源走：同一个码后来收到真报文就该摘掉 [SIM] */
    e.simulated     = g_inject_simulated;
    compose_text(e.code, e.detail, e.simulated, e.title, sizeof(e.title),
                 e.body, sizeof(e.body));

    /* CONDITION 状态迁移原位更新（renew=false），不打扰已读/排序 */
    if(!session_suppressed(e.code)) {
        ui_message_push_ex(e.code, ui_alarm_severity_to_msg(e.severity),
                           e.title, e.body, e.ts_ms, false);
    }
    notify_observers();
}

/* ---- 快照对齐 ----
 *
 * 单条上报只在状态变化时发，所以 UI 重启 / 串口重连 / SOC 比 MCU 后启动这几种
 * 情况下，之前就亮着的报警再也收不到——本地表和 MCU 真实状态会脱节。快照给的是
 * MCU 此刻的全量位图，用它把本地这份可能过期的副本刷新一遍。
 */

/** 快照说这条亮着：本地有就校正生命周期，没有就用字典兜底补一条。 */
void snapshot_upsert(uint16_t code, bool active, bool latched, int64_t now)
{
    if(!active && !latched) {
        return;   /* 两位都灭就不是"亮着"，反向清理那步会处理 */
    }
    const ui_alarm_stage_t stage =
        active ? UI_ALARM_STAGE_ACTIVE : UI_ALARM_STAGE_RTN_LATCHED;

    int i = slot_find(code);
    if(i >= 0) {
        /* 已在表里：只校正生命周期。severity/clear_mode/detail 保持单条上报给的
         * 值——那才是权威数据，快照压根给不出。 */
        g_current[i].e.stage = stage;
        return;
    }

    i = slot_alloc();
    if(i < 0) {
        return;   /* 槽满（不应发生：容量 > CONDITION 全集） */
    }
    /* 本地没有这条，只能靠字典把卡片凑出来。字典比固件旧时查不到，按最保守的
     * 一档显示：warning 且不给复位钮，避免误发 RESET。 */
    const ui_alarm_catalog_entry_t *cat = ui_alarm_catalog_find(code);
    std::memset(&g_current[i], 0, sizeof(g_current[i]));
    g_current[i].used = true;

    ui_alarm_entry_t &e = g_current[i].e;
    e.code       = code;
    e.severity   = cat ? cat->severity_fallback : 2;
    e.clear_mode = cat ? cat->clear_mode_fallback : 3;
    e.detail     = 0;     /* 快照没有 detail */
    e.ts_ms      = now;   /* 真实触发时刻已不可考，只能记补齐时刻 */
    e.stage      = stage;
    compose_text(e.code, e.detail, false, e.title, sizeof(e.title),
                 e.body, sizeof(e.body));

    if(!session_suppressed(e.code)) {
        ui_message_push_ex(e.code, ui_alarm_severity_to_msg(e.severity),
                           e.title, e.body, e.ts_ms, false);
    }
}

/** 收齐一批后做全量对齐。 */
void snapshot_apply(void)
{
    const int64_t now = wall_ms();

    /* 一、快照里亮着、本地没有的 → 补进来 */
    for(int w = 0; w < g_snap.word_count; w++) {
        const uint32_t live = g_snap.active[w] | g_snap.latched[w];
        for(int b = 0; b < 32; b++) {
            if(!(live & (1u << b))) {
                continue;
            }
            const uint16_t code =
                ui_alarm_snapshot_code(static_cast<uint8_t>(w),
                                       static_cast<uint8_t>(b));
            if(code == 0) {
                continue;   /* 该槽位字典里没有（字典比固件旧），跳过 */
            }
            snapshot_upsert(code, (g_snap.active[w] >> b) & 1u,
                            (g_snap.latched[w] >> b) & 1u, now);
        }
    }

    /* 二、本地有、快照说不亮了 → 当作已解除转历史。
     * 只处理受快照管辖的 code：EVENT 和字典里没有槽位的不能因为"快照里没有"
     * 就清掉，那是信息缺失不是状态变化。 */
    for(int i = 0; i < kCurrentCap; i++) {
        if(!g_current[i].used) {
            continue;
        }
        ui_alarm_entry_t &e = g_current[i].e;
        uint8_t w = 0, b = 0;
        if(!ui_alarm_snapshot_slot_of(e.code, &w, &b) || w >= g_snap.word_count) {
            continue;
        }
        if(((g_snap.active[w] | g_snap.latched[w]) >> b) & 1u) {
            continue;   /* 还亮着 */
        }
        ui_message_retract(e.code);
        history_prepend(e.code, e.severity, false, e.detail, e.ts_ms, now,
                        e.simulated);
        g_current[i].used = false;
    }

    notify_observers();
}

void on_alarm_snapshot(const middleware::AlarmSnapshotPayload &p)
{
    if(p.word_count == 0 || p.word_count > kSnapMaxWords ||
       p.word_index >= p.word_count) {
        UPC_LOG_ALARM_STATE(Warn, "snapshot bad frame: index=%u count=%u",
                            static_cast<unsigned>(p.word_index),
                            static_cast<unsigned>(p.word_count));
        return;
    }

    const int64_t now = wall_ms();
    /* 重开一批的三种情形：还没开始攒 / 上一批攒到超时 / 这一帧不属于上一批
     * （总帧数变了，或这个 index 已经收过——多半是 MCU 重发）。 */
    const bool timeout = g_snap.busy && (now - g_snap.started_ms > kSnapAssembleMs);
    if(!g_snap.busy || timeout || g_snap.word_count != p.word_count ||
       (g_snap.got_mask & (1u << p.word_index))) {
        g_snap = SnapAssembly{};
        g_snap.busy       = true;
        g_snap.word_count = p.word_count;
        g_snap.started_ms = now;
    }

    g_snap.active[p.word_index]  = p.active_mask;
    g_snap.latched[p.word_index] = p.latched_mask;
    g_snap.got_mask |= (1u << p.word_index);

    /* word_count ≤ kSnapMaxWords(8)，移位安全 */
    const uint32_t full = (1u << p.word_count) - 1u;
    if((g_snap.got_mask & full) == full) {
        snapshot_apply();
        g_snap = SnapAssembly{};
    }
}

/** 总线线程送来的一帧，堆上暂存等切回 UI 线程（同 ui_message_store）。 */
void apply_report_async(void *raw)
{
    auto *p = static_cast<middleware::AlarmReportPayload *>(raw);
    if(p->state_mask & middleware::kAlarmStateEvent) {
        on_alarm_event(p->alarm_code, p->severity, p->detail);
    } else {
        on_alarm_condition(*p);
    }
    delete p;
}

void apply_snapshot_async(void *raw)
{
    auto *p = static_cast<middleware::AlarmSnapshotPayload *>(raw);
    on_alarm_snapshot(*p);
    delete p;
}

} /* namespace */

ui_msg_severity_t ui_alarm_severity_to_msg(uint8_t severity)
{
    switch(severity) {
        case 1:  return UI_MSG_SEV_INFO;
        case 2:  return UI_MSG_SEV_ERROR;
        case 3:  return UI_MSG_SEV_CRITICAL;
        case 4:  return UI_MSG_SEV_EMERGENCY;
        default: return UI_MSG_SEV_ERROR;   /* 未知等级按不低于 warning */
    }
}

void ui_alarm_state_init(void)
{
    if(!g_inited) {
        std::memset(g_current, 0, sizeof(g_current));
        std::memset(g_event_text, 0, sizeof(g_event_text));
        /* 目录不存在时 history_save() 的 fopen 会失败，历史静默丢光，所以先建 */
        std::error_code ec;
        std::filesystem::create_directories(kMessageDir, ec);
        if(ec) {
            UPC_LOG_ALARM_STATE(Error, "mkdir %s failed: %s",
                                kMessageDir, ec.message().c_str());
        }
        history_load();
        g_inited = true;
    }
    ui::UiMsgProxy::instance().register_alarm_report(
        [](const middleware::AlarmReportPayload &p) {
            lv_async_call(apply_report_async,
                          new middleware::AlarmReportPayload(p));
        });
    ui::UiMsgProxy::instance().register_alarm_snapshot(
        [](const middleware::AlarmSnapshotPayload &p) {
            lv_async_call(apply_snapshot_async,
                          new middleware::AlarmSnapshotPayload(p));
        });
}

int ui_alarm_current_count(void)
{
    return g_order_count;
}

const ui_alarm_entry_t *ui_alarm_current_at(int idx)
{
    if(idx < 0 || idx >= g_order_count) {
        return nullptr;
    }
    return &g_current[g_order[idx]].e;
}

const ui_alarm_entry_t *ui_alarm_current_find(uint16_t code)
{
    const int i = slot_find(code);
    return (i >= 0) ? &g_current[i].e : nullptr;
}

bool ui_alarm_request_reset(uint16_t code)
{
    const int i = slot_find(code);
    if(i < 0) {
        return false;
    }
    ui_alarm_entry_t &e = g_current[i].e;
    if(e.clear_mode != static_cast<uint8_t>(middleware::AlarmClearMode::Reset) ||
       e.reset_pending) {
        return false;
    }
    e.reset_pending = true;
    ui::UiMsgProxy::instance().send_alarm_reset(code);
    arm_oneshot(reset_timeout_cb, kResetTimeoutMs, code);
    notify_observers();
    return true;
}

void ui_alarm_set_print_session(bool active)
{
    if(g_print_session == active) {
        return;
    }
    g_print_session = active;
    for(int i = 0; i < kCurrentCap; i++) {
        if(!g_current[i].used) {
            continue;
        }
        ui_alarm_entry_t &e = g_current[i].e;
        if(e.code != 1124 && e.code != 1125) {
            continue;
        }
        if(active) {
            ui_message_retract(e.code);
        } else {
            ui_message_push_ex(e.code, ui_alarm_severity_to_msg(e.severity),
                               e.title, e.body, e.ts_ms, true);
        }
    }
}

void ui_alarm_debug_inject(uint16_t code, uint8_t severity, uint8_t state_mask,
                           uint8_t clear_mode, uint32_t detail)
{
    /* 与 apply_report_async 同一分发；调试页本就在 UI 线程，无需 async 中转。
     * 注意注入 clear_mode=RESET 的条目按 Reset 会真发 UI_Alarm_Reset_Req 到
     * 串口，MCU 对未知报警不会回帧，10s 后 pending 自动解除——正好验超时路径。
     * 分发期间置 g_inject_simulated：条目因此带 [SIM] 前缀且不落盘。 */
    middleware::AlarmReportPayload p{};
    p.alarm_code = code;
    p.severity   = severity;
    p.state_mask = state_mask;
    p.clear_mode = clear_mode;
    p.detail     = detail;
    g_inject_simulated = true;
    if(p.state_mask & middleware::kAlarmStateEvent) {
        on_alarm_event(p.alarm_code, p.severity, p.detail);
    } else {
        on_alarm_condition(p);
    }
    g_inject_simulated = false;
}

const ui_alarm_entry_t *ui_alarm_print_blocker(void)
{
    /* 1124/1125 未复位期间 MCU 会拒绝新打印请求（联调确认），入口先拦提示 */
    for(int i = 0; i < g_order_count; i++) {
        const ui_alarm_entry_t &e = g_current[g_order[i]].e;
        if(e.code == 1124 || e.code == 1125) {
            return &e;
        }
    }
    return nullptr;
}

int ui_alarm_history_count(void)
{
    return g_history_count;
}

const ui_alarm_history_entry_t *ui_alarm_history_at(int idx)
{
    if(idx < 0 || idx >= g_history_count) {
        return nullptr;
    }
    return &g_history[idx];
}

void ui_alarm_history_delete_at(int idx)
{
    if(idx < 0 || idx >= g_history_count) {
        return;
    }
    for(int i = idx; i < g_history_count - 1; i++) {
        g_history[i] = g_history[i + 1];
    }
    g_history_count--;
    history_save();
    notify_observers();
}

void ui_alarm_add_observer(ui_alarm_observer_t cb)
{
    if(!cb) {
        return;
    }
    for(int i = 0; i < kMaxObservers; i++) {
        if(g_observers[i] == cb) {
            return;
        }
        if(!g_observers[i]) {
            g_observers[i] = cb;
            return;
        }
    }
}
