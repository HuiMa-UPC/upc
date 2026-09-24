#include "ui_device_state.hpp"

#include "ui_msg_proxy.hpp"
#include "ui_theme.hpp"
#include "config/cal_config.hpp"

#include <cstring>

namespace {

constexpr int kMaxObservers = 4;

ui_device_snapshot_t      g_snap;
bool                      g_inited      = false;
bool                      g_status_lock = false;
ui_device_state_observer_t g_observers[kMaxObservers] = { nullptr };

void notify_observers(void)
{
    for(int i = 0; i < kMaxObservers; i++) {
        if(g_observers[i]) {
            g_observers[i](&g_snap);
        }
    }
}

void apply_env(const middleware::EnvSnapshotPayload &p)
{
    std::strncpy(g_snap.device_name, p.device_name, sizeof(g_snap.device_name) - 1);
    g_snap.device_name[sizeof(g_snap.device_name) - 1] = '\0';
    if(!g_status_lock) {
        g_snap.status = static_cast<ui_device_status_t>(p.status);
    }
    g_snap.wifi_connected = p.wifi_connected != 0;
    g_snap.signal_bars    = p.signal_bars;
    g_snap.pm25_indoor    = p.pm25_indoor;
    g_snap.pm25_cabin     = p.pm25_cabin;
    g_snap.temperature_c  = p.temperature_c;
    g_snap.oxygen_percent = p.oxygen_percent;
    g_snap.pm10_indoor    = p.pm10_indoor;
    g_snap.humidity_percent = p.humidity_percent;
    g_snap.pressure_kpa   = p.pressure_kpa;
    g_snap.powder_bits    = p.powder_bits;
    if(p.door_state_valid) {
        g_snap.door_state_valid = true;
        g_snap.outer_door_open  = (p.outer_door_open != 0);
        g_snap.inner_door_open  = (p.inner_door_open != 0);
    }
    notify_observers();
}

/* WiFi 连接态直接跟 GLAVO_Wifi_State_Ntf 走。EnvSnapshotPayload 里虽然也带
 * wifi_connected，但那条只在 MCU 上报传感器时才发（没接 MCU 时根本不发），
 * 顶栏图标会一直停在初值。 */
void apply_wifi_state(const middleware::WifiStatePayload &p)
{
    const bool connected = (p.connected != 0);
    if(g_snap.wifi_connected == connected && g_snap.signal_bars == p.signal_bars) {
        return;
    }
    g_snap.wifi_connected = connected;
    g_snap.signal_bars    = p.signal_bars;
    notify_observers();
}

} /* namespace */

void ui_device_state_init(void)
{
    if(!g_inited) {
        std::memset(&g_snap, 0, sizeof(g_snap));
        std::strcpy(g_snap.device_name, upc::config::kDeviceDisplayName);
        g_snap.status         = UI_DEVICE_STATUS_IDLE;
        g_snap.wifi_connected = false;
        g_snap.signal_bars    = 0;
        /* 环境指标默认无数据（<0）→ 首帧环境到达前统一显示 "--" */
        g_snap.pm25_indoor    = -1.0f;
        g_snap.pm25_cabin     = -1.0f;
        g_snap.temperature_c  = -1.0f;
        g_snap.oxygen_percent = -1.0f;
        g_snap.pm10_indoor    = -1.0f;
        g_snap.humidity_percent = -1.0f;
        g_snap.pressure_kpa   = -1.0f;
        g_snap.powder_bits    = 0u;
        g_snap.outer_door_open  = false;
        g_snap.inner_door_open  = false;
        g_snap.door_state_valid = false;
        g_inited = true;
    }
    ui::UiMsgProxy::instance().register_env_update(
        [](const middleware::EnvSnapshotPayload &p) { apply_env(p); });
    ui::UiMsgProxy::instance().add_wifi_state_listener(
        [](const middleware::WifiStatePayload &p) { apply_wifi_state(p); });
    /* 讨一次当前状态：后端只在变化时推，订阅晚于首帧就得靠这条把图标点亮。 */
    ui::UiMsgProxy::instance().send_wifi_info();
}

const ui_device_snapshot_t *ui_device_state_get(void)
{
    return &g_snap;
}

void ui_device_state_set_status(ui_device_status_t s)
{
    g_snap.status = s;
    /* 打印中/暂停/结果态/吹粉的状态由前端说了算，锁住不让环境推送覆盖 */
    g_status_lock = (s == UI_DEVICE_STATUS_PRINTING ||
                     s == UI_DEVICE_STATUS_PRINT_DONE ||
                     s == UI_DEVICE_STATUS_PRINT_TERMINATED ||
                     s == UI_DEVICE_STATUS_PAUSED ||
                     s == UI_DEVICE_STATUS_PURIFYING ||
                     s == UI_DEVICE_STATUS_PURIFY_DONE);
    notify_observers();
}

void ui_device_state_set_outer_door_open(bool open)
{
    g_snap.outer_door_open  = open;
    g_snap.door_state_valid = true;
    notify_observers();
}

void ui_device_state_add_observer(ui_device_state_observer_t cb)
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

const char *ui_device_status_text(ui_device_status_t s)
{
    switch(s) {
    case UI_DEVICE_STATUS_READY:    return "Ready";
    case UI_DEVICE_STATUS_PRINTING: return "Printing";
    case UI_DEVICE_STATUS_ERROR:    return "Error";
    case UI_DEVICE_STATUS_PRINT_DONE:       return "Print Complete";
    case UI_DEVICE_STATUS_PRINT_TERMINATED: return "Print Terminated";
    case UI_DEVICE_STATUS_PAUSED:           return "Paused";
    case UI_DEVICE_STATUS_PURIFYING:        return "Air Purifying";
    case UI_DEVICE_STATUS_PURIFY_DONE:      return "Purify Complete";
    case UI_DEVICE_STATUS_IDLE:
    default:                        return "Idle";
    }
}

lv_color_t ui_device_status_color(ui_device_status_t s)
{
    switch(s) {
    case UI_DEVICE_STATUS_PRINTING: return user_color_accent;
    case UI_DEVICE_STATUS_ERROR:    return ui_color_danger;
    case UI_DEVICE_STATUS_READY:    return user_color_status_ok;
    case UI_DEVICE_STATUS_PRINT_DONE:       return user_color_status_ok;
    case UI_DEVICE_STATUS_PRINT_TERMINATED: return user_color_status_err;
    /* 暂停与吹粉都属于"设备还在这单活儿里"，跟 Printing 同用主色蓝 */
    case UI_DEVICE_STATUS_PAUSED:
    case UI_DEVICE_STATUS_PURIFYING:
    case UI_DEVICE_STATUS_PURIFY_DONE:      return user_color_accent;
    case UI_DEVICE_STATUS_IDLE:
    default:                        return user_color_text_dim;
    }
}
