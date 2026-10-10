#include "ui_alarm_state.hpp"

#include "lvgl_wrap.hpp"
#include "message_paths.hpp"
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

/* CONDITION 全集 95 条（2026-08-30 表），留余量。这个余量必须留够：槽满时
 * slot_alloc() 返回 -1，两个调用点只能丢掉这条报警，是**静默丢失**。MCU 每次
 * 扩报警表都要回头看一眼这个数。 */
constexpr int      kCurrentCap      = 128;
constexpr int      kMaxObservers    = 4;
constexpr uint32_t kResetTimeoutMs  = 10000; /* 复位无回报自动解锁按钮 */

/* ---- 快照攒帧 ----
 * 快照是请求-响应的，MCU 只回 (active|latched)!=0 的 slot，没回的一律按全零算。
 * 必须收齐 nonzero_slot_count 个不同 slot 才能原子提交——用半套去覆盖当前表会
 * 把没收到那些 slot 的报警误判成已解除。
 *
 * 轮次边界用「本地发出请求的时刻」界定，不做 Seq 匹配：串口组帧把 Seq 写死成
 * 0x01（serial_frame.cpp），收发两侧都没有 Seq 通路，而快照本来就是我们主动请求
 * 的、同时只维护一个，所以请求时刻是比 Seq 更可靠且零协议成本的分界。 */
constexpr int      kSnapSlots       = middleware::kAlarmSnapshotSlotCount;
constexpr uint32_t kSnapAssembleMs  = 2000; /* 攒帧超时：宁可丢一批也不用半套 */
/* 请求节流：进消息页 / 30s 轮询 / 打印前几个触发点会撞在一起，两次请求至少隔这么久 */
constexpr uint32_t kSnapRequestGapMs = 2000;
/* 兜底轮询：串口丢一帧单报是无声的，本地会一直缺那条，只能定期重新对齐 */
constexpr uint32_t kSnapPollMs       = 30000;

/* 快照在途时暂存的单条上报条数上限（开发指导 §11：收齐快照后按序重放）。
 * 攒帧窗口只有 2s，正常撑死几条；溢出就退回直接应用，宁可有回退风险也别丢报警。 */
constexpr int      kPendingReportCap = 16;

/* 消息中心自己的落盘目录，独立于 UPC_MEDIA_ROOT——报警历史不是媒体，早先挂在
 * media 下只是为了借那边的路径宏。目录由 ui_alarm_state_init() 负责创建：UI 起得
 * 可能比 Media 早，不能指望别的进程先建好。 */
constexpr const char *kMessageDir     = UPC_MESSAGE_DIR;
constexpr const char *kHistoryPath    = UPC_ALARM_HISTORY_PATH;
/* 先写 .tmp 再 rename，掉电不会留下半截文件 */
constexpr const char *kHistoryPathTmp = UPC_ALARM_HISTORY_PATH_TMP;

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

/* 显示序（severity 降序、同级新在前）的槽位下标表 */
int g_order[kCurrentCap];
int g_order_count = 0;

/* 正在攒的这一轮快照 */
struct SnapAssembly {
    bool     busy;              /**< 请求已发出、这一轮尚未收齐或作废 */
    bool     meta_known;        /**< 已收到首帧，count/severity 已锁定 */
    uint8_t  count;             /**< 本轮非零 slot 总数（首帧锁定） */
    uint8_t  severity;          /**< 本轮 highest_severity（首帧锁定） */
    uint16_t got_mask;          /**< 已收到哪些 slot_id（12 bit） */
    int      got_count;
    uint32_t active[kSnapSlots];
    uint32_t latched[kSnapSlots];
    /** 请求发出时刻 = 轮次起点，兼作超时基准。用 LVGL tick 而非 wall_ms()：
     *  wall_ms() 只有秒级精度，且 WiFi 连上后 NTP 会把系统时钟往前跳——跳成负差
     *  的话这一轮永远不超时，busy 卡死，后续所有快照请求都被节流挡在门外。 */
    uint32_t started_tick;
};
SnapAssembly g_snap{};
uint32_t g_snap_last_req_tick = 0;  /* 上次发请求的 tick，用于节流；0=从未发过 */
/** 攒帧超时 oneshot：文档写「攒超 2s 作废」，但原先只在下一帧快照或下一轮请求时
 *  才检查 elapsed。MCU 忙（起印握手/铺粉）迟迟不回时，busy 会一直占着，CONDITION
 *  单报（含 SOC 合成的 1120）卡在 g_pending，灯已红、浮层却要等最多 30s 轮询。 */
lv_timer_t *g_snap_assemble_timer = nullptr;

/* 快照在途期间暂存的 CONDITION 单报，收齐后按序重放（EVENT 不进这里，
 * 它不改 CONDITION 状态，直接落历史即可）。 */
middleware::AlarmReportPayload g_pending[kPendingReportCap];
int g_pending_count = 0;

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

/* ---- 定时器：复位超时 ----
 * user_data 直接塞 code（uintptr），不挂堆内存。
 *
 * 这里原先还有一支 EVENT 专用的 5s 自动撤浮层定时器。自动收起现在归
 * `ui_message_store` 统一按等级计时（INFO 5s / 告警级 15s），这支就撤了——留着会和
 * 那边打架，同一条 EVENT 被两个时长各撤一次。而且它一直只对 INFO 生效：撤下走的是
 * `ui_message_dismiss()`，那个函数第一道门就把非 INFO 挡掉了。 */

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
    /* EVENT 每一帧都是一次独立发生，一律 emit：哪怕同一个码上一次被关掉或收起过，
     * 也该重新弹。停留多久由 ui_message_store 按等级计时，这里不用管。 */
    ui_message_emit_ex(code, ui_alarm_severity_to_msg(severity),
                       txt->title, txt->body, now);
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

    /* 首帧（本地还没有这条）必然是一次新发生；已有的则看是不是从「已恢复等复位」
     * 回到了「条件又出现」——门关了又开、传感器反复越限都走这个上升沿。 */
    bool fresh = (i < 0);
    if(i < 0) {
        i = slot_alloc();
        if(i < 0) {
            /* 槽满（不应发生：kCurrentCap > CONDITION 全集）。真发生了这条报警
             * 就此丢失，必须留痕——否则界面少一条谁也查不出为什么。 */
            UPC_LOG_ALARM_STATE(Error, "current table full (%d), dropping alarm %u",
                                kCurrentCap, static_cast<unsigned>(p.alarm_code));
            return;
        }
        std::memset(&g_current[i], 0, sizeof(g_current[i]));
        g_current[i].used   = true;
        g_current[i].e.code = p.alarm_code;
        g_current[i].e.ts_ms = now;
    } else if(active && g_current[i].e.stage == UI_ALARM_STAGE_RTN_LATCHED) {
        fresh = true;
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

    /* 记录粒度仍是「一轮一条」：上面这个上升沿不新建条目，只是让 toast 重新弹一次，
     * 提醒用户条件又回来了。ACTIVE→RTN_LATCHED 这个方向只是同一轮内的状态迁移，
     * 走 sync 静默更新，不打扰已读与排序。
     * 1124/1125 以前在打印会话期被抑制（那时终止结果页自带失败原因条 + Reset），
     * 那个条已删，抑制也一并去掉——否则打印中断时哪儿都看不到停机原因。 */
    if(fresh) {
        ui_message_emit_ex(e.code, ui_alarm_severity_to_msg(e.severity),
                           e.title, e.body, e.ts_ms);
    } else {
        ui_message_sync_ex(e.code, ui_alarm_severity_to_msg(e.severity),
                           e.title, e.body, e.ts_ms);
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
        /* 槽满（不应发生：kCurrentCap > CONDITION 全集），同 on_alarm_condition */
        UPC_LOG_ALARM_STATE(Error, "current table full (%d), dropping snapshot alarm %u",
                            kCurrentCap, static_cast<unsigned>(code));
        return;
    }
    /* 本地没有这条，只能靠字典把卡片凑出来。字典比固件旧时查不到：
     *  - severity 取 warning 这一中间档；
     *  - clear_mode 取 RESET。快照报文里**根本没有 clear_mode 这个字段**，这里
     *    填的是个凭空造的值；填 NONE 会被 ui_alarm_reset_allowed() 第一道判断
     *    直接否掉，抽屉连 Reset 钮都不建，用户对字典没跟上的新报警毫无办法。
     *    这个兜底只活到该 code 的第一帧单条上报，on_alarm_condition() 会用报文
     *    值覆盖它——所以放宽的只是"信息缺失"，不违反"以报文为准"。 */
    const ui_alarm_catalog_entry_t *cat = ui_alarm_catalog_find(code);
    std::memset(&g_current[i], 0, sizeof(g_current[i]));
    g_current[i].used = true;

    ui_alarm_entry_t &e = g_current[i].e;
    e.code       = code;
    e.severity   = cat ? cat->severity_fallback : 2;
    e.clear_mode = cat ? cat->clear_mode_fallback
                       : static_cast<uint8_t>(middleware::AlarmClearMode::Reset);
    e.detail     = 0;     /* 快照没有 detail */
    e.ts_ms      = now;   /* 真实触发时刻已不可考，只能记补齐时刻 */
    e.stage      = stage;
    compose_text(e.code, e.detail, false, e.title, sizeof(e.title),
                 e.body, sizeof(e.body));

    /* 快照一律走 sync：它是对齐、不是事件。上面那个 slot_find 命中的分支直接 return，
     * 所以走到这里必然是「本地压根没有这条」——开机或串口重连后补齐 MCU 那边一直亮着
     * 的报警。sync 在从无到有时会弹一次，正是这一档要的：用户刚开机就该看见还有
     * 没处理完的事。反过来，30s 兜底轮询里那些本地已有的条目走不到这儿，不会刷屏。 */
    ui_message_sync_ex(e.code, ui_alarm_severity_to_msg(e.severity),
                       e.title, e.body, e.ts_ms);
}

/** 收齐一轮后做全量对齐（原子提交）。未收到的 slot 已在缓存里保持全零。 */
void snapshot_commit(void)
{
    const int64_t now = wall_ms();

    /* 一、快照里亮着、本地没有的 → 补进来。槽位→报警号用固定公式，字典比固件
     * 旧也能算出号来（算得出就不会丢，文案那边自会降级成 "Unknown alarm #"）。 */
    for(uint8_t s = 0; s < kSnapSlots; s++) {
        const uint32_t live = g_snap.active[s] | g_snap.latched[s];
        for(uint8_t b = 0; b < 32; b++) {
            if(!(live & (1u << b))) {
                continue;
            }
            snapshot_upsert(middleware::alarm_snapshot_code_of(s, b),
                            (g_snap.active[s] >> b) & 1u,
                            (g_snap.latched[s] >> b) & 1u, now);
        }
    }

    /* 二、本地有、快照说不亮了 → 当作已解除转历史。
     * 只处理受快照管辖的 code：EVENT 与保留号段（xx96..xx99）不占 bit，
     * 它们"快照里没提"是信息缺失不是状态变化，照清会把真报警抹掉。
     *
     * 已在 g_pending 里的码先跳过：在途单报已乐观上屏（见 apply_report_async），
     * 快照是请求那一刻的旧位图，照清会先撤卡再被重放接回来，浮层闪一下
     * （打印氧门 1120：灯已红、卡却眨）。round_end 重放仍会以单报为准。 */
    for(int i = 0; i < kCurrentCap; i++) {
        if(!g_current[i].used) {
            continue;
        }
        ui_alarm_entry_t &e = g_current[i].e;
        if(!middleware::alarm_snapshot_covers(e.code)) {
            continue;
        }
        bool in_pending = false;
        for(int j = 0; j < g_pending_count; j++) {
            if(g_pending[j].alarm_code == e.code) {
                in_pending = true;
                break;
            }
        }
        if(in_pending) {
            continue;
        }
        const int rel  = e.code - 1000;
        const int slot = rel / 100 * 3 + rel % 100 / 32;
        const int bit  = rel % 100 % 32;
        if(((g_snap.active[slot] | g_snap.latched[slot]) >> bit) & 1u) {
            continue;   /* 还亮着 */
        }
        ui_message_retract(e.code);
        history_prepend(e.code, e.severity, false, e.detail, e.ts_ms, now,
                        e.simulated);
        g_current[i].used = false;
    }

    notify_observers();
}

void snapshot_reject(const char *why); /* 前向：超时 cb 在 round_end 之前 */

void snap_assemble_timer_cancel(void)
{
    if(g_snap_assemble_timer) {
        lv_timer_delete(g_snap_assemble_timer);
        g_snap_assemble_timer = nullptr;
    }
}

void snap_assemble_timeout_cb(lv_timer_t * /*t*/)
{
    /* repeat_count=1：回调后由 LVGL 自删，先摘句柄避免 round_end 再 delete。 */
    g_snap_assemble_timer = nullptr;
    if(g_snap.busy) {
        snapshot_reject("assemble timeout");
    }
}

void snap_assemble_timer_arm(void)
{
    snap_assemble_timer_cancel();
    g_snap_assemble_timer = lv_timer_create(snap_assemble_timeout_cb,
                                            kSnapAssembleMs, nullptr);
    lv_timer_set_repeat_count(g_snap_assemble_timer, 1);
}

/** 结束当前这一轮（提交与否由调用方决定），并按序重放暂存的单条上报。 */
void snapshot_round_end(bool commit)
{
    snap_assemble_timer_cancel();
    if(commit) {
        snapshot_commit();
    }
    g_snap = SnapAssembly{};

    /* 快照是「请求那一刻」的状态，暂存的单报都比它新，必须压在快照之上重放，
     * 否则较旧的快照会把较新的状态盖回去，界面出现回退（开发指导 §11）。 */
    const int n = g_pending_count;
    g_pending_count = 0;   /* 先清零：重放路径不会再往这儿塞 */
    for(int i = 0; i < n; i++) {
        on_alarm_condition(g_pending[i]);
    }
    if(n > 0) {
        UPC_LOG_ALARM_STATE(Info, "snapshot: replayed %d buffered report(s)", n);
    }
}

/** 整轮作废：半套位图会把没收到那些 slot 的报警误判成已解除，宁可不更新。 */
void snapshot_reject(const char *why)
{
    UPC_LOG_ALARM_STATE(Warn, "snapshot round rejected: %s", why);
    snapshot_round_end(false);
}

void on_alarm_snapshot(const middleware::AlarmSnapshotPayload &p)
{
    /* 没请求过却来了响应：可能是上一轮超时作废后的迟到帧。无从判断它属于哪一轮，
     * 更不知道同轮还有几个 slot 没到，收下就有提交半套的风险，直接丢。
     * 真需要最新状态时下一个触发点（最迟 30s 轮询）会重新请求。 */
    if(!g_snap.busy) {
        UPC_LOG_ALARM_STATE(Warn, "snapshot slot %u arrived without a request",
                            static_cast<unsigned>(p.slot_id));
        return;
    }
    if(lv_tick_elaps(g_snap.started_tick) > kSnapAssembleMs) {
        snapshot_reject("assemble timeout");
        return;
    }
    if(p.slot_id >= kSnapSlots || p.nonzero_slot_count > kSnapSlots) {
        snapshot_reject("slot_id / count out of range");
        return;
    }

    /* 同一轮所有分片的 count 与 highest_severity 必须一致，对不上说明混进了
     * 别轮的帧（无 Seq 可辨，这两个字段就是仅有的一致性线索）。 */
    if(!g_snap.meta_known) {
        g_snap.meta_known = true;
        g_snap.count      = p.nonzero_slot_count;
        g_snap.severity   = p.highest_severity;
    } else if(g_snap.count != p.nonzero_slot_count ||
              g_snap.severity != p.highest_severity) {
        snapshot_reject("round metadata mismatch");
        return;
    }

    /* count=0 是「当前无报警」的清空响应，只有一帧且必须全零 */
    if(g_snap.count == 0) {
        if(p.slot_id != 0 || p.highest_severity != 0 ||
           p.active_mask != 0 || p.latched_mask != 0) {
            snapshot_reject("count=0 frame not all-zero");
            return;
        }
        UPC_LOG_ALARM_STATE(Info, "snapshot: all clear");
        snapshot_round_end(true);   /* 缓存本就全零，直接提交即全清 */
        return;
    }

    if((p.active_mask | p.latched_mask) == 0) {
        snapshot_reject("nonzero slot carries empty masks");
        return;
    }
    if(g_snap.got_mask & (1u << p.slot_id)) {
        snapshot_reject("duplicate slot_id");
        return;
    }

    g_snap.active[p.slot_id]  = p.active_mask;
    g_snap.latched[p.slot_id] = p.latched_mask;
    g_snap.got_mask |= static_cast<uint16_t>(1u << p.slot_id);
    g_snap.got_count++;

    if(g_snap.got_count >= g_snap.count) {
        UPC_LOG_ALARM_STATE(Info, "snapshot: committed %d slot(s), highest_sev=%u",
                            g_snap.got_count,
                            static_cast<unsigned>(g_snap.severity));
        snapshot_round_end(true);
    }
}

/**
 * 发一次快照请求，并以「此刻」作为新一轮的起点。
 *
 * MCU 不会主动推快照，不请求就永远补不齐重启/重连前就亮着的报警。触发点有五处
 * （启动、串口重连、进消息中心、30s 轮询、开始打印前），会撞在一起，所以这里
 * 兜住两种重复：上一轮还没收齐就不再发，两次请求也至少隔 kSnapRequestGapMs。
 */
void snapshot_request(const char *reason)
{
    const uint32_t now = lv_tick_get();
    if(g_snap.busy && lv_tick_elaps(g_snap.started_tick) <= kSnapAssembleMs) {
        return;   /* 上一轮在途，等它收齐或超时 */
    }
    if(g_snap_last_req_tick != 0 &&
       lv_tick_elaps(g_snap_last_req_tick) < kSnapRequestGapMs) {
        return;
    }
    if(g_snap.busy) {
        snapshot_reject("superseded by a new request");
    }

    g_snap = SnapAssembly{};
    g_snap.busy         = true;
    g_snap.started_tick = now;
    g_snap_last_req_tick = now ? now : 1u;   /* 0 是"从未发过"的哨兵，避开它 */
    snap_assemble_timer_arm();
    UPC_LOG_ALARM_STATE(Info, "snapshot request (%s)", reason ? reason : "-");
    ui::UiMsgProxy::instance().send_alarm_snapshot_req();
}

/** 总线线程送来的一帧，堆上暂存等切回 UI 线程（同 ui_message_store）。 */
void apply_report_async(void *raw)
{
    auto *p = static_cast<middleware::AlarmReportPayload *>(raw);
    if(p->state_mask & middleware::kAlarmStateEvent) {
        /* EVENT 不改 CONDITION 状态，快照在途也能直接落历史（开发指导 §11.4）。
         * 顺带记一笔异常 clear_mode：按表 EVENT 恒为 NONE(3)，而
         * 信息报警命令.csv 的示例帧里写的是 0，两者矛盾且示例已确认作废。
         * 真收到非 3 的话说明固件行为与表不符，得回头找 MCU 组。 */
        if(p->clear_mode != static_cast<uint8_t>(middleware::AlarmClearMode::None)) {
            UPC_LOG_ALARM_STATE(Warn, "EVENT %u clear_mode=%u (expected NONE=3)",
                                static_cast<unsigned>(p->alarm_code),
                                static_cast<unsigned>(p->clear_mode));
        }
        on_alarm_event(p->alarm_code, p->severity, p->detail);
    } else if(g_snap.busy && g_pending_count < kPendingReportCap) {
        /* 快照在途：仍入 pending，等原子提交后按序重放（开发指导 §11）。
         * 同时立刻应用一遍——否则要等 MCU 回齐或 2s 超时才上浮层，而 Glavo
         * 侧灯效已按单报变红（打印氧门 1120）。重放幂等，同码 ACTIVE 只刷新。 */
        g_pending[g_pending_count++] = *p;
        on_alarm_condition(*p);
    } else {
        /* 暂存溢出就直接应用：回退风险远小于把报警丢掉 */
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

/** 复位失败回执：MCU 明确拒了，立刻解锁按钮，不用干等 10s 超时。 */
void apply_reset_result_async(void *raw)
{
    auto *p = static_cast<middleware::AlarmResetResultPayload *>(raw);
    const int i = slot_find(p->alarm_code);
    if(i >= 0 && g_current[i].e.reset_pending) {
        g_current[i].e.reset_pending = false;
        UPC_LOG_ALARM_STATE(Warn, "reset failed for %u (result=%u)",
                            static_cast<unsigned>(p->alarm_code),
                            static_cast<unsigned>(p->result));
        notify_observers();
    }
    delete p;
}

/** 30s 兜底轮询：串口丢帧这种无声异常检测不到，只能靠定期重新对齐。 */
void snapshot_poll_cb(lv_timer_t * /*t*/)
{
    snapshot_request("poll");
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
    ui::UiMsgProxy::instance().register_alarm_reset_result(
        [](const middleware::AlarmResetResultPayload &p) {
            lv_async_call(apply_reset_result_async,
                          new middleware::AlarmResetResultPayload(p));
        });

    /* 启动补齐 + 30s 兜底轮询。UI 可能比 Glavo 先起，这一发未必有人接；轮询
     * 会把它捞回来，Glavo 侧串口一通也会自己请求一次。 */
    snapshot_request("init");
    lv_timer_create(snapshot_poll_cb, kSnapPollMs, nullptr);
}

void ui_alarm_request_snapshot(const char *reason)
{
    snapshot_request(reason);
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

bool ui_alarm_reset_allowed(const ui_alarm_entry_t *e)
{
    if(!e || e->reset_pending ||
       e->clear_mode != static_cast<uint8_t>(middleware::AlarmClearMode::Reset)) {
        return false;
    }
    if(e->stage == UI_ALARM_STAGE_RTN_LATCHED) {
        return true;
    }
    /* ACTIVE：OWNER_TASK 类值得下发——MCU 会把请求转给业务任务做硬件恢复。
     * LOCAL 类（1124/1125）条件仍在时必被拒，让按钮亮着只会白等一轮 pending。
     * reset_policy 不在报文里，只能查字典；**查不到按放行处理**：那是字典没跟上
     * 固件的信息缺失，不是"已知不可复位"，而现表 30 条 RESET 里 28 条都是
     * OWNER_TASK。拦下来的代价是新报警永远复不了位（只能断电），误发一帧的代价
     * 只是 MCU 不认就不回、10s 超时自解锁，真拒了还有失败回执立刻解锁。 */
    const ui_alarm_catalog_entry_t *cat = ui_alarm_catalog_find(e->code);
    return !cat || cat->reset_owner_task;
}

bool ui_alarm_request_reset(uint16_t code)
{
    const int i = slot_find(code);
    if(i < 0) {
        return false;
    }
    ui_alarm_entry_t &e = g_current[i].e;
    if(!ui_alarm_reset_allowed(&e)) {
        return false;
    }
    e.reset_pending = true;
    ui::UiMsgProxy::instance().send_alarm_reset(code);
    arm_oneshot(reset_timeout_cb, kResetTimeoutMs, code);
    notify_observers();
    return true;
}

void ui_alarm_debug_inject(uint16_t code, uint8_t severity, uint8_t state_mask,
                           uint8_t clear_mode, uint32_t detail)
{
    /* 与 apply_report_async 同一分发；调试页本就在 UI 线程，无需 async 中转。
     * 注意注入 clear_mode=RESET 的条目按 Reset 会真发 UI_Alarm_Reset_Ntf 到
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
    /* 未复位期间 MCU 会拒绝新打印请求的报警。它拒的时候只回 EVENT 2003
     * PRINT_START_DENIED，那条给不出是哪儿卡住，所以打印入口先查这张表把原因
     * 亮出来。报文和字典里都没有"是否阻断打印"这样的字段，只能按报警语义维护——
     * 七条都是 clear_mode=RESET 的停机等待复位类：
     *   1004 内门运动故障   1124 打印步骤超时   1125 运动失败打印中止
     *   1126 筛粉电机过流   1127 筛粉电流检测故障
     *   1128 PTC 加热过流   1129 PTC 电流检测故障
     * MCU 以后再加停机类报警，这里要跟着补。 */
    static constexpr uint16_t kBlockers[] = {
        1004, 1124, 1125, 1126, 1127, 1128, 1129,
    };
    /* 按显示序遍历，命中的第一条就是最该先说的（severity 降序、同级新在前） */
    for(int i = 0; i < g_order_count; i++) {
        const ui_alarm_entry_t &e = g_current[g_order[i]].e;
        for(uint16_t blocker : kBlockers) {
            if(e.code == blocker) {
                return &e;
            }
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

int ui_alarm_history_delete_keys(const ui_alarm_history_key_t *keys, int n)
{
    if(!keys || n <= 0) {
        return 0;
    }
    /* 原地压缩：不命中的往前挪，一遍扫完，次序（新→旧）不变 */
    int kept = 0;
    for(int i = 0; i < g_history_count; i++) {
        bool hit = false;
        for(int k = 0; k < n; k++) {
            if(g_history[i].code == keys[k].code && g_history[i].ts_ms == keys[k].ts_ms) {
                hit = true;
                break;
            }
        }
        if(!hit) {
            if(kept != i) {
                g_history[kept] = g_history[i];
            }
            kept++;
        }
    }
    const int removed = g_history_count - kept;
    if(removed > 0) {
        g_history_count = kept;
        history_save();
        notify_observers();
    }
    return removed;
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
