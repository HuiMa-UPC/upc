#include "ui_message_store.hpp"

#include "ui_msg_proxy.hpp"
#include "upc_contract.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "notice_badge_icons.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr int kCapacity     = 24;
constexpr int kMaxObservers = 4;

/** 自动收起的停留时长，按等级分两档：信息看一眼就够，告警得给足读完的时间。
 *  扫描周期取 500ms——比最短的 5s 小一个量级，误差看不出来，又不至于空转太勤。 */
constexpr uint32_t kCollapseInfoMs  = 5000;
constexpr uint32_t kCollapseAlarmMs = 15000;
constexpr uint32_t kCollapseScanMs  = 500;

using middleware::DeviceEventCode;

/** 码 → 人话。文案先按稿意写的英文占位，正式文案到位后只改这一张表。
 *  等级列与 doc/设备通信协议-数据结构定义（v0.9）.md 第五节那张表逐行对齐。 */
struct CatalogEntry {
    uint16_t          code;
    ui_msg_severity_t severity;
    const char       *title;
    const char       *body;
};

const CatalogEntry kCatalog[] = {
    /* ---- info ---- */
    { (uint16_t)DeviceEventCode::PowderLow, UI_MSG_SEV_INFO,
      "Powder running low", "Refill the powder bin soon" },
    { (uint16_t)DeviceEventCode::FilterDue, UI_MSG_SEV_INFO,
      "Filter service due", "Replace the filter cartridge" },
    { (uint16_t)DeviceEventCode::PrintPaused, UI_MSG_SEV_INFO,
      "Print paused", "Resume from the print controls" },
    { (uint16_t)DeviceEventCode::PrintComplete, UI_MSG_SEV_INFO,
      "Print finished", "Ready for collection" },
    { (uint16_t)DeviceEventCode::PurgeComplete, UI_MSG_SEV_INFO,
      "Purge complete", "Oxygen level reached target" },
    /* ---- error ---- */
    { (uint16_t)DeviceEventCode::WeightSensorErr, UI_MSG_SEV_ERROR,
      "Weight sensor fault", "Powder weight reading is invalid" },
    { (uint16_t)DeviceEventCode::GalvoCalFail, UI_MSG_SEV_ERROR,
      "Galvo calibration failed", "Service required" },
    { (uint16_t)DeviceEventCode::MotionStallguard, UI_MSG_SEV_ERROR,
      "Motion axis stalled", "An axis lost steps during travel" },
    { (uint16_t)DeviceEventCode::TempSensorErr, UI_MSG_SEV_ERROR,
      "Temperature sensor fault", "Reading out of valid range" },
    { (uint16_t)DeviceEventCode::NtpSyncFail, UI_MSG_SEV_ERROR,
      "Clock sync failed", "Device time may drift" },
    { (uint16_t)DeviceEventCode::DoorSensorConflict, UI_MSG_SEV_ERROR,
      "Door sensor conflict", "Door switches disagree" },
    { (uint16_t)DeviceEventCode::JobFileErr, UI_MSG_SEV_ERROR,
      "Job file error", "Download failed or checksum mismatch" },
    { (uint16_t)DeviceEventCode::PowderInsufficient, UI_MSG_SEV_ERROR,
      "Not enough powder", "Refill before starting this job" },
    { (uint16_t)DeviceEventCode::GalvoNotCalibrated, UI_MSG_SEV_ERROR,
      "Galvo not calibrated", "Run a self test before printing" },
    { (uint16_t)DeviceEventCode::PrintError, UI_MSG_SEV_ERROR,
      "Print error", "The job was terminated" },
    /* ---- critical ---- */
    { (uint16_t)DeviceEventCode::DoorOpen, UI_MSG_SEV_CRITICAL,
      "Door is open", "Close the door to continue" },
    { (uint16_t)DeviceEventCode::O2High, UI_MSG_SEV_CRITICAL,
      "Oxygen level too high", "Chamber purge is insufficient" },
    { (uint16_t)DeviceEventCode::Pm25High, UI_MSG_SEV_CRITICAL,
      "PM2.5 too high", "Airborne powder above the limit" },
    { (uint16_t)DeviceEventCode::Overtemp, UI_MSG_SEV_CRITICAL,
      "Chamber overheating", "Temperature above the safe limit" },
    { (uint16_t)DeviceEventCode::LaserInterlock, UI_MSG_SEV_CRITICAL,
      "Laser interlock tripped", "Check the machine locally" },
    { (uint16_t)DeviceEventCode::FilterOvertemp, UI_MSG_SEV_CRITICAL,
      "Filter overheating", "Stop and inspect the filter" },
    { (uint16_t)DeviceEventCode::FilterRequired, UI_MSG_SEV_CRITICAL,
      "Filter must be replaced", "Printing is blocked until replaced" },
    /* ---- emergency ---- */
    { (uint16_t)DeviceEventCode::Smoke, UI_MSG_SEV_EMERGENCY,
      "Smoke detected", "Evacuate and check the machine" },
    { (uint16_t)DeviceEventCode::Estop, UI_MSG_SEV_EMERGENCY,
      "Emergency stop engaged", "Release the button to recover" },
    { (uint16_t)DeviceEventCode::FireSuppress, UI_MSG_SEV_EMERGENCY,
      "Fire suppression triggered", "Do not open the machine" },
    { (uint16_t)DeviceEventCode::Esd, UI_MSG_SEV_EMERGENCY,
      "ESD protection triggered", "Check grounding before restarting" },
    { (uint16_t)DeviceEventCode::FirstLayerReferenceRetrying, UI_MSG_SEV_ERROR,
      "Reference capture failed", "Retrying bare reference capture" },
    { (uint16_t)DeviceEventCode::FirstLayerReferenceReady, UI_MSG_SEV_INFO,
      "Reference captured", "Starting first-layer check" },
    { (uint16_t)DeviceEventCode::FirstLayerVisualRecoat, UI_MSG_SEV_ERROR,
      "First layer not passed", "Recoating and retrying first-layer check" },
    { (uint16_t)DeviceEventCode::FirstLayerVisualPassed, UI_MSG_SEV_INFO,
      "First layer passed", "Continuing normal printing" },
    { (uint16_t)DeviceEventCode::OpticalPathCheckStarted, UI_MSG_SEV_INFO,
      "Optical path check", "Verifying red-dot visibility at five points" },
    { (uint16_t)DeviceEventCode::OpticalPathCheckPassed, UI_MSG_SEV_INFO,
      "Optical path passed", "Starting the print job" },
    { (uint16_t)DeviceEventCode::OpticalPathCheckFail, UI_MSG_SEV_CRITICAL,
      "Optical path abnormal", "Print blocked. Check galvo / red laser / camera" },
    { (uint16_t)DeviceEventCode::PlateHeightFlatnessWarn, UI_MSG_SEV_ERROR,
      "Plate flatness check", "Height measure did not pass in 5 rounds; inspect the plate" },
    /* ---- UI 本地 ---- */
    { (uint16_t)DeviceEventCode::UiWearMask, UI_MSG_SEV_CRITICAL,
      "Wear a mask before opening", "Cabin powder may become airborne" },
    { (uint16_t)DeviceEventCode::UiPurifyBeforeOpen, UI_MSG_SEV_INFO,
      "Purify before collecting", "Run air purify before opening the door" },
    { (uint16_t)DeviceEventCode::UiDebugSample, UI_MSG_SEV_INFO,
      "Debug test message", "Injected from the debug page" },
};

ui_message_t          g_msgs[kCapacity];
int                   g_count = 0;
uint32_t              g_seq   = 0;
bool                  g_inited = false;
ui_message_observer_t g_observers[kMaxObservers] = { nullptr };

/** 浮层顺序（口罩钉首、其余等级降序、同级比新）的下标表，随每次变更重排。 */
int g_order[kCapacity];
int g_order_count = 0;

const CatalogEntry *catalog_find(uint16_t code)
{
    for(const CatalogEntry &e : kCatalog) {
        if(e.code == code) {
            return &e;
        }
    }
    return nullptr;
}

int find_index(uint16_t code)
{
    for(int i = 0; i < g_count; i++) {
        if(g_msgs[i].code == code) {
            return i;
        }
    }
    return -1;
}

/** a 是否该排在 b 前面。 */
bool ranks_before(const ui_message_t &a, const ui_message_t &b)
{
    const bool a_pin = (a.code == (uint16_t)DeviceEventCode::UiWearMask);
    const bool b_pin = (b.code == (uint16_t)DeviceEventCode::UiWearMask);
    if(a_pin != b_pin) {
        return a_pin;
    }
    if(a.severity != b.severity) {
        return a.severity > b.severity;
    }
    return a.seq > b.seq;
}

/** 这条还需不需要提请用户注意：没看过，或者是还没解除的告警级。
 *  后半句不用另外判断解除与否——CONDITION 解除时 ui_alarm_state 会 retract 掉条目，
 *  所以还在表里就等于还没解除。 */
bool needs_attention(const ui_message_t &m)
{
    return m.unread || m.severity != UI_MSG_SEV_INFO;
}

uint32_t collapse_delay(ui_msg_severity_t sev)
{
    return (sev == UI_MSG_SEV_INFO) ? kCollapseInfoMs : kCollapseAlarmMs;
}

void rebuild_order(void)
{
    g_order_count = 0;
    for(int i = 0; i < g_count; i++) {
        if(g_msgs[i].visual != UI_MSG_SHOWN) {
            continue;
        }
        /* 条数上限 24，插入排序足够，还能保证同权重时先来的在前 */
        int pos = g_order_count;
        while(pos > 0 && ranks_before(g_msgs[i], g_msgs[g_order[pos - 1]])) {
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

void remove_at(int idx)
{
    for(int i = idx; i < g_count - 1; i++) {
        g_msgs[i] = g_msgs[i + 1];
    }
    g_count--;
}

/** 队列满时腾一格：优先丢最旧的已读消息，全未读就丢最旧的那条。 */
bool evict_one(void)
{
    int victim = -1;
    for(int i = 0; i < g_count; i++) {
        if(!g_msgs[i].unread && (victim < 0 || g_msgs[i].seq < g_msgs[victim].seq)) {
            victim = i;
        }
    }
    if(victim < 0) {
        for(int i = 0; i < g_count; i++) {
            if(victim < 0 || g_msgs[i].seq < g_msgs[victim].seq) {
                victim = i;
            }
        }
    }
    if(victim < 0) {
        return false;
    }
    remove_at(victim);
    return true;
}

/** 总线线程送来的一条事件，堆上暂存，等切回 UI 线程再消费。 */
struct PendingEvent {
    uint16_t code;
    uint8_t  active;
    int64_t  ts_ms;
};

void apply_event_async(void *raw)
{
    PendingEvent *p = static_cast<PendingEvent *>(raw);
    if(p->active) {
        if(p->code == static_cast<uint16_t>(DeviceEventCode::FirstLayerReferenceRetrying)) {
            ui_toast_show("Reference capture failed, retrying...",
                          user_color_status_err, 1800);
        } else if(p->code == static_cast<uint16_t>(DeviceEventCode::FirstLayerReferenceReady)) {
            ui_toast_show("Reference captured, starting first-layer check",
                          user_color_status_ok, 1800);
        } else if(p->code == static_cast<uint16_t>(DeviceEventCode::FirstLayerVisualRecoat)) {
            ui_toast_show("First layer check failed, recoating...",
                          user_color_status_err, 1800);
        } else if(p->code == static_cast<uint16_t>(DeviceEventCode::FirstLayerVisualPassed)) {
            ui_toast_show("First layer check passed",
                          user_color_status_ok, 1800);
        }
    }
    if(p->active) {
        /* active=true 是一次真事件，走 emit：哪怕这条之前被关掉或收起过也要重新弹。 */
        ui_message_emit(p->code, p->ts_ms);
    } else {
        ui_message_retract(p->code);
    }
    delete p;
}

/**
 * sync 与 emit 的共同实现。四个对外接口只是「文案从哪来」和「renew 与否」的组合。
 *
 * renew=false（sync）时的两条铁律：
 *   - 已存在的条目**不碰 visual**。收起过的保持收起，否则打印流程那种每秒好几次的
 *     无脑同步、和 30s 快照对齐，会把刚收进铃铛的 toast 一次次顶回来。
 *   - 已存在的条目**不动 seq 与 unread**，排序和红点都不该被一次状态复述打扰。
 * 但仍然要 notify_observers()：title/body 存的是调用方缓冲的指针，内容可能已原位变了。
 */
void write_entry(uint16_t code, ui_msg_severity_t severity,
                 const char *title, const char *body, int64_t ts_ms, bool renew)
{
    if(!title) {
        return;
    }
    int idx = find_index(code);
    if(idx < 0) {
        if(g_count >= kCapacity && !evict_one()) {
            return;
        }
        idx = g_count++;
        std::memset(&g_msgs[idx], 0, sizeof(g_msgs[idx]));
        renew = true;   /* 从无到有：无论哪个入口都算一次新发生，该弹 */
    } else if(g_msgs[idx].visual == UI_MSG_CLOSED_BY_USER) {
        /* 用户亲手关掉之后这事又被提起：重新露面。**只认这一种**，自动收起
         * 不在此列——那是系统行为，不代表用户处理过。 */
        renew = true;
    }

    ui_message_t &m = g_msgs[idx];
    m.code     = code;
    m.severity = severity;
    m.title    = title;
    m.body     = body ? body : "";
    m.ts_ms    = ts_ms;
    if(renew) {
        m.seq        = ++g_seq;
        m.unread     = true;
        m.visual     = UI_MSG_SHOWN;
        m.shown_tick = lv_tick_get();
    }
    notify_observers();
}

void write_from_catalog(uint16_t code, int64_t ts_ms, bool renew)
{
    const CatalogEntry *cat = catalog_find(code);
    if(!cat) {
        return;   /* 码表里没有的码没法出文案，丢掉 */
    }
    write_entry(code, cat->severity, cat->title, cat->body, ts_ms, renew);
}

/** 扫描定时器：把停留到时限的消息收进铃铛。一支全局定时器扫全表，而不是每条挂一个
 *  one-shot——条目会被 evict/retract 删掉，那样会留下一堆指向空槽的定时器。 */
void collapse_scan_cb(lv_timer_t *)
{
    bool changed = false;
    for(int i = 0; i < g_count; i++) {
        ui_message_t &m = g_msgs[i];
        if(m.visual != UI_MSG_SHOWN) {
            continue;
        }
        if(lv_tick_elaps(m.shown_tick) >= collapse_delay(m.severity)) {
            m.visual = UI_MSG_AUTO_COLLAPSED;
            changed = true;
        }
    }
    if(changed) {
        notify_observers();
    }
}

void on_device_event(const middleware::DeviceEventPayload &p)
{
    /* register_* 回调跑在总线线程上，而落到这儿的每一次变更都会让通知浮层
     * 重建 LVGL 对象，必须先切回 UI 线程（同 control.cpp 的 on_powder_low_async）。
     * payload 有个 64 位时间戳，塞不进 lv_async_call 的那个指针，只能堆上过一手。 */
    lv_async_call(apply_event_async,
                  new PendingEvent{ p.code, p.active, p.event_ts_ms });
}

} /* namespace */

/* ---- 等级观感：四档阶梯 蓝 → 黄 → 橙 → 红 ----
 * 旧稿只定了两端（3058:7058 info 蓝 / 3058:7233 alert 红），中间两档是自己填的；
 * 浅色稿 6064:71936 的变量表补上了这两档（Color 3 #FFC942 / Color 2 #FF7843），
 * 现已换成稿值——与原先自填的 #FFD426 / #FF8A3D 只差一点，色相阶梯不变。
 * 第四档红稿上仍没有样例，沿用自填的 #FF4242。
 * 之所以不是"红 + 更深的红"：暗背景下暗色视觉后退，最严重那档反而最不显眼。
 * 四档语义见 doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md 第 7 节
 * （1 information / 2 warning / 3 fault / 4 trip=安全停机）。 */
lv_color_t ui_message_severity_color(ui_msg_severity_t sev)
{
    switch(sev) {
        case UI_MSG_SEV_INFO:      return lv_color_hex(0x485DFF);  /* information */
        case UI_MSG_SEV_ERROR:     return lv_color_hex(0xFFC942);  /* warning */
        case UI_MSG_SEV_CRITICAL:  return lv_color_hex(0xFF7843);  /* fault */
        case UI_MSG_SEV_EMERGENCY: return lv_color_hex(0xFF4242);  /* trip */
    }
    return lv_color_hex(0xFFC942);   /* 未知等级按不低于 warning */
}

/* 按档位取原生尺寸那一版：调用方拿它去 ui_user_make_icon_img(target)，
 * target == src_sz 时缩放整段被跳过，徽标才是逐像素干净的。原先只有一张 36px
 * 位图，1024 档要缩到 0.39 倍，细描边糊成一团。
 * 尺寸表在 tools/gen_notice_badge_icons.py，改那边要同步这里。 */
const lv_image_dsc_t *ui_message_severity_icon(ui_msg_severity_t sev)
{
    const bool info = (sev == UI_MSG_SEV_INFO);
    switch(g_ui->profile_id) {
        case UI_RES_1920: return info ? &ic_notice_info_27 : &ic_notice_alert_27;
        case UI_RES_1280: return info ? &ic_notice_info_18 : &ic_notice_alert_18;
        default:          return info ? &ic_notice_info_14 : &ic_notice_alert_14;
    }
}

void ui_message_store_init(void)
{
    if(!g_inited) {
        std::memset(g_msgs, 0, sizeof(g_msgs));
        g_count       = 0;
        g_order_count = 0;
        g_inited      = true;
        /* 自动收起的扫描定时器，进程内只起一支、不再撤（本模块是单例，随进程存活）。 */
        lv_timer_create(collapse_scan_cb, kCollapseScanMs, nullptr);
    }
    ui::UiMsgProxy::instance().register_device_event(
        [](const middleware::DeviceEventPayload &p) { on_device_event(p); });
}

void ui_message_sync(uint16_t code, int64_t ts_ms)
{
    write_from_catalog(code, ts_ms, false);
}

void ui_message_emit(uint16_t code, int64_t ts_ms)
{
    write_from_catalog(code, ts_ms, true);
}

void ui_message_sync_ex(uint16_t code, ui_msg_severity_t severity,
                        const char *title, const char *body, int64_t ts_ms)
{
    write_entry(code, severity, title, body, ts_ms, false);
}

void ui_message_emit_ex(uint16_t code, ui_msg_severity_t severity,
                        const char *title, const char *body, int64_t ts_ms)
{
    write_entry(code, severity, title, body, ts_ms, true);
}

void ui_message_debug_push(uint16_t code)
{
    const CatalogEntry *cat = catalog_find(code);
    if(!cat) {
        return;
    }
    /* ui_message_t 只存指针，带前缀的标题得有个活得够久的家。注入表就那几条，
     * 固定槽位够用；满了复用最老的一格（重复注入同一码会命中同一格）。 */
    constexpr int kSimSlots = 6;
    static char     slots[kSimSlots][96];
    static uint16_t slot_code[kSimSlots];
    static int      next_slot = 0;

    int s = -1;
    for(int i = 0; i < kSimSlots; i++) {
        if(slot_code[i] == code) {
            s = i;
            break;
        }
    }
    if(s < 0) {
        s = next_slot;
        next_slot = (next_slot + 1) % kSimSlots;
        slot_code[s] = code;
    }
    std::snprintf(slots[s], sizeof(slots[s]), "[SIM] %s", cat->title);

    ui_message_emit_ex(code, cat->severity, slots[s], cat->body, 0);
}

void ui_message_retract(uint16_t code)
{
    const int idx = find_index(code);
    if(idx < 0) {
        return;
    }
    remove_at(idx);
    notify_observers();
}

bool ui_message_dismiss(uint16_t code)
{
    const int idx = find_index(code);
    if(idx < 0 || g_msgs[idx].severity != UI_MSG_SEV_INFO) {
        return false;   /* 告警不给关：稿上只有信息卡带关闭钮 */
    }
    if(g_msgs[idx].visual != UI_MSG_SHOWN) {
        return false;
    }
    g_msgs[idx].visual = UI_MSG_CLOSED_BY_USER;
    notify_observers();
    return true;
}

bool ui_message_collapse(uint16_t code)
{
    const int idx = find_index(code);
    if(idx < 0 || g_msgs[idx].visual != UI_MSG_SHOWN) {
        return false;
    }
    g_msgs[idx].visual = UI_MSG_AUTO_COLLAPSED;
    notify_observers();
    return true;
}

int ui_message_visible_count(void)
{
    return g_order_count;
}

const ui_message_t *ui_message_visible_at(int idx)
{
    if(idx < 0 || idx >= g_order_count) {
        return nullptr;
    }
    return &g_msgs[g_order[idx]];
}

int ui_message_attention_count(void)
{
    int n = 0;
    for(int i = 0; i < g_count; i++) {
        if(needs_attention(g_msgs[i])) {
            n++;
        }
    }
    return n;
}

const ui_message_t *ui_message_attention_at(int idx)
{
    if(idx < 0) {
        return nullptr;
    }
    /* 按 seq 降序取第 idx 个。面板最多要三条，全表也才 24 个，选择法足够，
     * 也省得再维护一张随每次变更重排的下标表（浮层那张 g_order 是另一套顺序：
     * 它按等级排、且只收 SHOWN 的，两者不能共用）。 */
    uint32_t upper = 0xFFFFFFFFu;
    for(int rank = 0; rank <= idx; rank++) {
        int best = -1;
        for(int i = 0; i < g_count; i++) {
            if(!needs_attention(g_msgs[i]) || g_msgs[i].seq >= upper) {
                continue;
            }
            if(best < 0 || g_msgs[i].seq > g_msgs[best].seq) {
                best = i;
            }
        }
        if(best < 0) {
            return nullptr;   /* 不足 idx+1 条 */
        }
        if(rank == idx) {
            return &g_msgs[best];
        }
        upper = g_msgs[best].seq;
    }
    return nullptr;
}

bool ui_message_needs_attention(void)
{
    for(int i = 0; i < g_count; i++) {
        if(needs_attention(g_msgs[i])) {
            return true;
        }
    }
    return false;
}

int ui_message_total_count(void)
{
    return g_count;
}

const ui_message_t *ui_message_at(int idx)
{
    if(idx < 0 || idx >= g_count) {
        return nullptr;
    }
    return &g_msgs[idx];
}

int ui_message_unread_count(void)
{
    int n = 0;
    for(int i = 0; i < g_count; i++) {
        if(g_msgs[i].unread) {
            n++;
        }
    }
    return n;
}

void ui_message_mark_all_read(void)
{
    bool changed = false;
    for(int i = 0; i < g_count; i++) {
        if(g_msgs[i].unread) {
            g_msgs[i].unread = false;
            changed = true;
        }
    }
    if(changed) {
        notify_observers();
    }
}

void ui_message_add_observer(ui_message_observer_t cb)
{
    if(!cb) {
        return;
    }
    for(int i = 0; i < kMaxObservers; i++) {
        if(g_observers[i] == cb) {
            return;   /* 幂等：页面重建时重复注册不会叠加 */
        }
        if(!g_observers[i]) {
            g_observers[i] = cb;
            return;
        }
    }
}
