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
    { (uint16_t)DeviceEventCode::FirstLayerVisualRecoat, UI_MSG_SEV_ERROR,
      "First layer not passed", "Recoating and retrying first-layer check" },
    { (uint16_t)DeviceEventCode::FirstLayerVisualPassed, UI_MSG_SEV_INFO,
      "First layer passed", "Continuing normal printing" },
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

void rebuild_order(void)
{
    g_order_count = 0;
    for(int i = 0; i < g_count; i++) {
        if(g_msgs[i].dismissed) {
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
        if(p->code == static_cast<uint16_t>(DeviceEventCode::FirstLayerVisualRecoat)) {
            ui_toast_show("First layer check failed, recoating...",
                          user_color_status_err, 1800);
        } else if(p->code == static_cast<uint16_t>(DeviceEventCode::FirstLayerVisualPassed)) {
            ui_toast_show("First layer check passed",
                          user_color_status_ok, 1800);
        }
    }
    if(p->active) {
        ui_message_push(p->code, p->ts_ms);
    } else {
        ui_message_retract(p->code);
    }
    delete p;
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
 * 稿只定了两端（3058:7058 info 蓝 / 3058:7233 alert 红），中间两档自己填。
 * 之所以不是"红 + 更深的红"：暗背景下暗色视觉后退，最严重那档反而最不显眼。
 * 四档语义见 doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md 第 7 节
 * （1 information / 2 warning / 3 fault / 4 trip=安全停机）。 */
lv_color_t ui_message_severity_color(ui_msg_severity_t sev)
{
    switch(sev) {
        case UI_MSG_SEV_INFO:      return lv_color_hex(0x485DFF);  /* information */
        case UI_MSG_SEV_ERROR:     return lv_color_hex(0xFFD426);  /* warning */
        case UI_MSG_SEV_CRITICAL:  return lv_color_hex(0xFF8A3D);  /* fault */
        case UI_MSG_SEV_EMERGENCY: return lv_color_hex(0xFF4242);  /* trip */
    }
    return lv_color_hex(0xFFD426);   /* 未知等级按不低于 warning */
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
    }
    ui::UiMsgProxy::instance().register_device_event(
        [](const middleware::DeviceEventPayload &p) { on_device_event(p); });
}

void ui_message_push(uint16_t code, int64_t ts_ms)
{
    const CatalogEntry *cat = catalog_find(code);
    if(!cat) {
        return;   /* 码表里没有的码没法出文案，丢掉 */
    }

    int idx = find_index(code);
    if(idx >= 0 && !g_msgs[idx].dismissed) {
        /* 同一条已经挂在浮层上了，重复 push 视作无事发生。stateful 事件的
         * 重复 active=true 本来就是冗余边沿，而打印流程那两条是每次态刷新都
         * 无脑 push 的——不在这儿吸收掉，卡片会不停重建、已读也会被顶回未读。 */
        g_msgs[idx].ts_ms = ts_ms;
        return;
    }
    if(idx < 0) {
        if(g_count >= kCapacity && !evict_one()) {
            return;
        }
        idx = g_count++;
        std::memset(&g_msgs[idx], 0, sizeof(g_msgs[idx]));
    }

    ui_message_t &m = g_msgs[idx];
    m.code      = code;
    m.severity  = cat->severity;
    m.title     = cat->title;
    m.body      = cat->body;
    m.ts_ms     = ts_ms;
    m.seq       = ++g_seq;
    m.unread    = true;
    m.dismissed = false;   /* 再次触发要重新露面，哪怕上次被 × 掉过 */
    notify_observers();
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

    ui_message_push_ex(code, cat->severity, slots[s], cat->body, 0, true);
}

void ui_message_push_ex(uint16_t code, ui_msg_severity_t severity,
                        const char *title, const char *body,
                        int64_t ts_ms, bool renew)
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
        renew = true;   /* 新条目必然是新发生 */
    } else if(g_msgs[idx].dismissed) {
        renew = true;   /* 被 × 掉后再触发：重新露面并回未读 */
    }

    ui_message_t &m = g_msgs[idx];
    m.code      = code;
    m.severity  = severity;
    m.title     = title;
    m.body      = body ? body : "";
    m.ts_ms     = ts_ms;
    m.dismissed = false;
    if(renew) {
        m.seq    = ++g_seq;
        m.unread = true;
    }
    /* 非 renew 也要重排通知：title/body 指向调用方缓冲，内容可能已原位变化 */
    notify_observers();
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
        return false;   /* 告警不给关：稿上红卡没有 × */
    }
    if(g_msgs[idx].dismissed) {
        return false;
    }
    g_msgs[idx].dismissed = true;
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
