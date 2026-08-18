#include "page_user_settings.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "ui_arc_spinner.hpp"
#include "page_manager.hpp"
#include "settings_icons.h"
#include "ui_msg_proxy.hpp"
#include "msg_runtime.hpp"
#include "config/cal_config.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/**
 * @file page_user_settings.cpp
 * @brief 设置页（两栏：左侧子导航 + 右侧详情），对齐设计稿。
 *
 * 布局（顶部那条设备信息由外壳的常驻条 ui_device_bar 画，本页不再自建 header；
 * content_panel_ 的 pad_top 已为它让出位置）：
 *   ┌───────────────────────┬──────────────────────────────┐
 *   │ Network & Connectivity│  Wi-Fi   [ On | Off ]         │
 *   │ Display & Sound       │  Current Wi-Fi                 │
 *   │ Advanced Print        │   [SSID]                  >    │
 *   │ Device & Account      │  Available Wi-Fi               │
 *   │ Privacy & Security    │   [SSID]                  >    │
 *   │ About                 │   ...（可滚动）          ▲▼    │
 *   │ Debug              ↗  │                                │
 *   └───────────────────────┴──────────────────────────────┘
 *
 * 前端阶段说明（后端由同事接）：
 *   - Wi-Fi 列表来自后端扫描推送（GLAVO_Wifi_Scan_Rsp），首次结果到达前显示 Searching。
 *   - Debug 行点击 → 开发者密码框（"888"）→ switch_to("user_debug")。
 *   - 软件版本号迁至 About 子页。
 *   - 子导航图标暂用 LV_SYMBOL 占位，待 SVG 资源到位再替换。
 *   - 软件版本 UPC_SW_VERSION：根 CMakeLists.txt 的 UPC_PRODUCT_VERSION。
 */

#ifndef UPC_SW_VERSION
#error "UPC_SW_VERSION not defined — set UPC_PRODUCT_VERSION in root CMakeLists.txt"
#endif

namespace {

constexpr const char *kDevPassword = "888";

/* ---- 子导航定义 ---- */
enum NavId {
    NAV_NETWORK = 0,
    NAV_DISPLAY,
    NAV_PRINT,
    NAV_DEVICE,
    NAV_PRIVACY,
    NAV_ABOUT,
    NAV_DEBUG,
    NAV_COUNT,
};

struct NavDef {
    const lv_image_dsc_t *icon;   /**< A8 图标（settings_icons.*，运行时 recolor） */
    const char           *label;
};

const NavDef kNavDefs[NAV_COUNT] = {
    { &settings_icon_network, "Network & Connectivity" },
    { &settings_icon_display, "Display & Sound"        },
    { &settings_icon_print,   "Advanced Print"         },
    { &settings_icon_device,  "Device & Account"       },
    { &settings_icon_privacy, "Privacy & Security"     },
    { &settings_icon_about,   "About"                  },
    { &settings_icon_debug,   "Debug"                  },
};

/* Wi-Fi 连接态占位（后端未上报时用于避免误导） */
const char *const kCurrentWifi = "--";

/* 前端占位：当前 IP / 已保存网络（后端网络信息接口到位后替换） */
const char *const kIpAddress = "--";
const char *const kSavedWifi[] = {
    "SnowX_WIFI_2.4G",
    "Sweden_5G",
    "Home_Net",
};

const char *about_device_model_text()
{
    static bool loaded = false;
    static std::string model = "SNOWPOD test03";
    if(!loaded) {
        loaded = true;
        upc::config::CalConfigOptions cal_cfg{};
        if(upc::config::load_cal_config_options(upc::config::kCalDefaultPath, &cal_cfg) &&
           cal_cfg.has_config_section &&
           cal_cfg.has_device_model &&
           !cal_cfg.device_model.empty()) {
            model = cal_cfg.device_model;
        }
    }
    return model.c_str();
}

/* ---- About 页 MCU 固件版本（串口请求应答：UI_Mcu_Version_Req → GLAVO_Mcu_Version_Ntf） ----
 * 串口默认关闭 ACK 重传（GLAVO_SERIAL_ACK_ENABLE=0），丢包不会由后端补发，故在此
 * 做一次超时重发。显示格式 V<major>.<minor>.<patch>.<laser>.<z>（见协议 TLV_REPORT_VERSION）。 */
constexpr uint32_t kMcuVerTimeoutMs = 2000;   /**< 单次查询等待上限 */
constexpr uint8_t  kMcuVerMaxRetry  = 1;      /**< 超时后重发次数 */

lv_obj_t   *g_mcu_ver_lbl   = nullptr;   /**< 值标签；仅 About 面板存活期间有效 */
lv_timer_t *g_mcu_ver_timer = nullptr;   /**< 超时/重发定时器（一次性，自删后置空） */
char        g_mcu_ver_text[32] = "--";   /**< 显示缓存，跨次进入 About 复用 */
bool        g_mcu_ver_valid = false;     /**< 是否已成功取到过版本 */
uint8_t     g_mcu_ver_retry = 0;         /**< 本轮已重发次数 */

void mcu_ver_send_and_arm(void);

void mcu_ver_stop_timer(void)
{
    if(g_mcu_ver_timer) {
        lv_timer_delete(g_mcu_ver_timer);
        g_mcu_ver_timer = nullptr;
    }
}

void mcu_ver_timeout_cb(lv_timer_t * /*t*/)
{
    g_mcu_ver_timer = nullptr;   /* 一次性定时器已由 LVGL 自删 */
    if(g_mcu_ver_retry < kMcuVerMaxRetry) {
        g_mcu_ver_retry++;
        mcu_ver_send_and_arm();
        return;
    }
    /* 仅在从未取到过版本时才显示 Unknown：已有缓存值不被本次失败覆盖 */
    if(!g_mcu_ver_valid && g_mcu_ver_lbl) {
        lv_label_set_text(g_mcu_ver_lbl, "Unknown");
    }
}

void mcu_ver_send_and_arm(void)
{
    ui::UiMsgProxy::instance().send_mcu_version_query();
    mcu_ver_stop_timer();
    g_mcu_ver_timer = lv_timer_create(mcu_ver_timeout_cb, kMcuVerTimeoutMs, nullptr);
    lv_timer_set_repeat_count(g_mcu_ver_timer, 1);
}

/** 进入 About 子页：发起一次查询（标签先显示缓存值 / "--"） */
void mcu_ver_query_on_enter(void)
{
    g_mcu_ver_retry = 0;
    mcu_ver_send_and_arm();
}

/** GLAVO_Mcu_Version_Ntf 回调：写缓存并刷新标签（UI 线程，随 MsgPort::process 投递） */
void mcu_ver_apply(const middleware::McuVersionPayload &p)
{
    mcu_ver_stop_timer();
    std::snprintf(g_mcu_ver_text, sizeof(g_mcu_ver_text), "V%u.%u.%u.%u.%u",
                  static_cast<unsigned>(p.major), static_cast<unsigned>(p.minor),
                  static_cast<unsigned>(p.patch), static_cast<unsigned>(p.laser_type),
                  static_cast<unsigned>(p.z_motor_type));
    g_mcu_ver_valid = true;
    if(g_mcu_ver_lbl) {
        lv_label_set_text(g_mcu_ver_lbl, g_mcu_ver_text);
    }
}

/* ---- About 页振镜控制板固件版本（TCP 0x0F 首包缓存：UI_Galvo_Version_Req → GLAVO_Galvo_Version_Ntf）
 * 版本只在 TCP 连上后由下位机主动上报，进 About 时向 Glavo 读缓存，无串口式超时重发。
 * 显示格式 V<(v>>16)>.<(v>>8)&0xff>.<(v&0xff)>（如 0x00030004 → V3.0.4）。 */
lv_obj_t *g_galvo_ver_lbl = nullptr;           /**< 值标签；仅 About 面板存活期间有效 */
char      g_galvo_ver_text[32] = "--";         /**< 显示缓存，跨次进入 About 复用 */
bool      g_galvo_ver_valid = false;           /**< 是否已成功取到过版本 */

/** 进入 About 子页：请求一次缓存版本（标签先显示缓存值 / "--"） */
void galvo_ver_query_on_enter(void)
{
    ui::UiMsgProxy::instance().send_galvo_version_query();
}

/** GLAVO_Galvo_Version_Ntf 回调：写缓存并刷新标签 */
void galvo_ver_apply(const middleware::GalvoVersionPayload &p)
{
    if(!p.valid) {
        if(!g_galvo_ver_valid && g_galvo_ver_lbl) {
            lv_label_set_text(g_galvo_ver_lbl, "Unknown");
        }
        return;
    }
    const uint32_t v = p.fw_version_le;
    std::snprintf(g_galvo_ver_text, sizeof(g_galvo_ver_text), "V%u.%u.%u",
                  static_cast<unsigned>((v >> 16) & 0xFFu),
                  static_cast<unsigned>((v >> 8) & 0xFFu),
                  static_cast<unsigned>(v & 0xFFu));
    g_galvo_ver_valid = true;
    if(g_galvo_ver_lbl) {
        lv_label_set_text(g_galvo_ver_lbl, g_galvo_ver_text);
    }
}

/* 后端网络状态缓存（GLAVO_Wifi_State_Ntf；未上线时回退占位）。待后端对齐。 */
middleware::WifiStatePayload g_wifi_state = {};
bool g_wifi_state_valid = false;
const char *g_connect_ssid = nullptr;   /**< 连接密码框对应的 SSID */

/* WiFi 开关意图状态：用户点开关即改；后端状态到达时同步。跨重建保持，
 * 避免整页重建把开关重置成默认"开"。 */
bool g_wifi_on = true;

/* 已保存网络列表缓存（GLAVO_Wifi_Saved_Rsp；未上线时回退占位 kSavedWifi）。 */
enum NetSub { NET_MAIN = 0, NET_ADVANCED, NET_SAVED, NET_DETAIL, NET_EMPTY };
int g_net_subpage = NET_MAIN;
middleware::WifiEntryPayload g_saved_wifi[16] = {};
uint32_t g_saved_count = 0;
bool     g_saved_valid = false;

/* 扫描结果缓存（GLAVO_Wifi_Scan_Rsp；首次结果到达前列表显示 Searching）。 */
middleware::WifiEntryPayload g_scan_wifi[32] = {};
uint32_t g_scan_count = 0;
bool     g_scan_valid = false;

/* 上次用于建列表的 SSID/连接态签名：后端每次轮询都会因信号浮动重推，只有条目
 * 真变了才重建，否则整页重建会把用户正在看的滚动位置顶回顶部。 */
std::string g_scan_list_sig;

/* Advanced / Wi-Fi 详情页的 IP 值标签：状态推送到达时就地改文字（这两个子页
 * 是静态构建的，不重建就永远显示进页那一刻的 IP）。离开子页时置空。 */
lv_obj_t *g_ip_val_lbl = nullptr;

/* 停在网络页时的周期重扫（进页只扫一次的话，附近网络变化要等用户来回切页才可见）。 */
lv_timer_t *g_wifi_rescan_timer = nullptr;
constexpr uint32_t kWifiRescanMs = 8000;

/** 当前 IP：后端有效则用真实值，否则占位。 */
const char *wifi_ip_text(void)
{
    return (g_wifi_state_valid && g_wifi_state.ip_addr[0]) ? g_wifi_state.ip_addr
                                                           : kIpAddress;
}

/** 当前已连 SSID：后端有效则用真实值，否则占位。 */
const char *wifi_current_ssid(void)
{
    return (g_wifi_state_valid && g_wifi_state.ssid[0]) ? g_wifi_state.ssid
                                                        : kCurrentWifi;
}

/** 某 SSID 是否已保存（扫描项 saved 标记 或 已保存列表命中）→ 连接时免密。 */
bool wifi_ssid_saved(const char *ssid)
{
    if(!ssid || !ssid[0]) {
        return false;
    }
    for(uint32_t i = 0; i < g_scan_count; i++) {
        if(g_scan_wifi[i].saved && std::strcmp(g_scan_wifi[i].ssid, ssid) == 0) {
            return true;
        }
    }
    if(g_saved_valid) {
        for(uint32_t i = 0; i < g_saved_count; i++) {
            if(std::strcmp(g_saved_wifi[i].ssid, ssid) == 0) {
                return true;
            }
        }
    }
    return false;
}

/** 扫描缓存的 SSID + 连接态签名（列表只显示这两样，信号强弱不参与）。 */
std::string scan_list_signature(void)
{
    std::string sig;
    for(uint32_t i = 0; i < g_scan_count; i++) {
        sig += g_scan_wifi[i].ssid;
        sig += g_scan_wifi[i].connected ? ":C;" : ":.;";
    }
    return sig;
}

void wifi_rescan_timer_cb(lv_timer_t * /*t*/)
{
    if(g_wifi_on) {
        ui::UiMsgProxy::instance().send_wifi_scan();
    }
}

void wifi_rescan_start(void)
{
    if(!g_wifi_rescan_timer) {
        g_wifi_rescan_timer = lv_timer_create(wifi_rescan_timer_cb, kWifiRescanMs, nullptr);
    }
}

void wifi_rescan_stop(void)
{
    if(g_wifi_rescan_timer) {
        lv_timer_delete(g_wifi_rescan_timer);
        g_wifi_rescan_timer = nullptr;
    }
}

lv_obj_t *g_nav_btns[NAV_COUNT]  = { nullptr };
lv_obj_t *g_nav_icons[NAV_COUNT] = { nullptr };
lv_obj_t *g_detail     = nullptr;   /**< 右侧详情容器（按选中重建） */
lv_obj_t *g_wifi_list  = nullptr;   /**< 翻页按钮的滚动目标（网络详情=整个详情容器） */
lv_obj_t *g_page       = nullptr;   /**< 设置页根：翻页按钮悬浮父级（整屏居中用） */
lv_obj_t *g_scroll_btns= nullptr;   /**< Wi-Fi 翻页按钮悬浮层（仅网络详情存在） */
lv_obj_t *g_wifi_list_box = nullptr;/**< 主网络页可隐藏的列表容器（WiFi 关时隐藏） */
lv_obj_t *g_wifi_seg_on  = nullptr; /**< WiFi 开关矩形分段：On 段 */
lv_obj_t *g_wifi_seg_off = nullptr; /**< WiFi 开关矩形分段：Off 段 */
int       g_selected   = NAV_NETWORK;

/* ---- 网络子页导航（页内子视图 + 返回箭头） ---- */
void net_show_main(void);
void net_show_advanced(void);
void net_show_saved(void);
void net_show_wifi_detail(void);
void net_show_empty(void);

/* ---- 弹窗（密码 / Wi-Fi 连接，复用同一句柄，互斥打开） ---- */
ui_modal_dialog_t *g_dlg     = nullptr;
lv_obj_t          *g_pwd_ta  = nullptr;
lv_obj_t          *g_pwd_err = nullptr;

/* 开发者密码进程级解锁标记：本次 App 运行期间输对一次即置位，后续进 Debug
 * 免密；进程(重启 App)后静态量复位，需重新输入。 */
bool g_dev_unlocked = false;

struct OtaDialogState {
    ui_modal_dialog_t *dlg = nullptr;
    lv_obj_t *status_lbl = nullptr;
    lv_obj_t *bar = nullptr;
};

enum class OtaTarget : uint8_t {
    Glavo = 0,
    McuSerial = 1,
};

OtaDialogState g_glavo_ota_ui{};
OtaDialogState g_mcu_ota_ui{};
bool g_ota_proxy_wired = false;

struct OtaUiUpdate {
    OtaTarget target;
    middleware::McuOtaStatusPayload status;
};

void ota_close_dialog(OtaTarget target);

OtaDialogState *ota_state(OtaTarget target)
{
    return target == OtaTarget::Glavo ? &g_glavo_ota_ui : &g_mcu_ota_ui;
}

const char *ota_state_text(middleware::McuOtaState state)
{
    switch(state) {
    case middleware::McuOtaState::Preparing:
        return "Preparing OTA...";
    case middleware::McuOtaState::Transferring:
        return "Transferring firmware...";
    case middleware::McuOtaState::Verifying:
        return "Verifying on MCU...";
    case middleware::McuOtaState::Success:
        return "OTA successful";
    case middleware::McuOtaState::Failed:
        return "OTA failed";
    case middleware::McuOtaState::Cancelled:
        return "OTA cancelled";
    default:
        return "OTA...";
    }
}

void ota_ui_apply_async(void *user)
{
    auto *upd = static_cast<OtaUiUpdate *>(user);
    if(!upd) {
        return;
    }
    OtaDialogState *state = ota_state(upd->target);
    if(state->status_lbl) {
        char buf[96];
        const int pct = (upd->status.progress * 100) / 255;
        std::snprintf(buf, sizeof(buf), "%s (%d%%)",
                      ota_state_text(upd->status.state), pct);
        lv_label_set_text(state->status_lbl, buf);
    }
    if(state->bar) {
        const int pct = (upd->status.progress * 100) / 255;
        lv_bar_set_value(state->bar, pct, LV_ANIM_OFF);
    }
    delete upd;
}

void wire_ota_proxy_once()
{
    if(g_ota_proxy_wired) {
        return;
    }
    g_ota_proxy_wired = true;
    ui::UiMsgProxy::instance().register_mcu_ota_status(
        [](const middleware::McuOtaStatusPayload &st) {
            auto *upd = new OtaUiUpdate{};
            upd->target = OtaTarget::Glavo;
            upd->status = st;
            lv_async_call(ota_ui_apply_async, upd);
        });
    ui::UiMsgProxy::instance().register_serial_mcu_ota_status(
        [](const middleware::McuOtaStatusPayload &st) {
            auto *upd = new OtaUiUpdate{};
            upd->target = OtaTarget::McuSerial;
            upd->status = st;
            lv_async_call(ota_ui_apply_async, upd);
        });
}

void on_glavo_ota_close(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_mcu_ota_cancel();
    ota_close_dialog(OtaTarget::Glavo);
}

void on_mcu_ota_close(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_serial_mcu_ota_cancel();
    ota_close_dialog(OtaTarget::McuSerial);
}

void open_ota_dialog(OtaTarget target, const char *title)
{
    OtaDialogState *state = ota_state(target);
    if(state->dlg) {
        return;
    }
    wire_ota_proxy_once();

    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user(title, USER_DLG_W, false);
    cfg.with_close_btn = true;
    state->dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!state->dlg) {
        return;
    }
    if(state->dlg->close_btn) {
        lv_obj_add_event_cb(state->dlg->close_btn,
                            target == OtaTarget::Glavo ? on_glavo_ota_close : on_mcu_ota_close,
                            LV_EVENT_CLICKED, nullptr);
    }

    state->status_lbl = ui_user_make_body_label(state->dlg->body, "Starting...");
    state->bar = ui_user_make_progress_bar(
        state->dlg->body, LV_PCT(100), USER_PROGRESS_H);
    lv_bar_set_value(state->bar, 0, LV_ANIM_OFF);
}

void ota_close_dialog(OtaTarget target)
{
    OtaDialogState *state = ota_state(target);
    if(state->dlg) {
        ui_modal_dialog_close(state->dlg);
        state->dlg = nullptr;
        state->status_lbl = nullptr;
        state->bar = nullptr;
    }
}

void on_glavo_ota_entry_click(lv_event_t *)
{
    open_ota_dialog(OtaTarget::Glavo, "Galvo OTA");
    ui::UiMsgProxy::instance().send_mcu_ota_start();
}

void on_mcu_ota_entry_click(lv_event_t *)
{
    open_ota_dialog(OtaTarget::McuSerial, "MCU Serial OTA");
    ui::UiMsgProxy::instance().send_serial_mcu_ota_start();
}

void dlg_close(void)
{
    ui_keyboard_hide();
    if(g_dlg) {
        ui_modal_dialog_close(g_dlg);
        g_dlg     = nullptr;
        g_pwd_ta  = nullptr;
        g_pwd_err = nullptr;
    }
}

/* ---- 连接中模态（spinner，持续到 GLAVO_Wifi_Connect_Rsp 返回才关闭） ---- */
ui_modal_dialog_t *g_conn_dlg     = nullptr;
ui_arc_spinner_t  *g_conn_spinner = nullptr;

void close_connecting_dialog(void)
{
    if(g_conn_spinner) {
        ui_arc_spinner_destroy(g_conn_spinner);
        g_conn_spinner = nullptr;
    }
    if(g_conn_dlg) {
        ui_modal_dialog_close(g_conn_dlg);
        g_conn_dlg = nullptr;
    }
}

void open_connecting_dialog(const char *ssid)
{
    close_connecting_dialog();   /* 互斥，避免叠加 */
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("Connecting", 0, false);
    g_conn_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_conn_dlg) {
        return;
    }
    lv_obj_set_flex_flow(g_conn_dlg->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_conn_dlg->body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_conn_dlg->body, USER_PAD, 0);

    g_conn_spinner = ui_arc_spinner_create(g_conn_dlg->body, 0);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "Connecting to %s...", ssid ? ssid : "");
    ui_user_make_body_label(g_conn_dlg->body, buf);
}

void on_dlg_cancel(lv_event_t *)
{
    dlg_close();
}

/* 输入框统一样式 */
lv_obj_t *make_dialog_textarea(lv_obj_t *parent, const char *placeholder,
                               lv_keyboard_mode_t kb_mode)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, UI_INPUT_H);
    lv_obj_set_style_text_font(ta, user_font_text(), 0);
    lv_obj_set_style_bg_color(ta, user_color_bg, 0);
    lv_obj_set_style_border_color(ta, user_color_divider, 0);
    lv_obj_set_style_text_color(ta, user_color_text, 0);
    ui_keyboard_attach(ta, kb_mode);
    return ta;
}

/* ---- 开发者密码弹窗 → 通过则进调试页 ---- */
void on_dev_confirm(lv_event_t *)
{
    if(!g_pwd_ta) {
        return;
    }
    const char *input = lv_textarea_get_text(g_pwd_ta);
    if(input && std::strcmp(input, kDevPassword) == 0) {
        g_dev_unlocked = true;   /* 本次运行期间免密 */
        dlg_close();
        ui::PageManager::instance().switch_to("user_debug");
        return;
    }
    if(g_pwd_err) {
        lv_label_set_text(g_pwd_err, "Wrong password");
        lv_obj_remove_flag(g_pwd_err, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_pwd_ta) {
        lv_textarea_set_text(g_pwd_ta, "");
    }
}

/* 两个密码弹窗共用的确认钮（Enter / Connect）。
 * 底子是 user_style_btn_primary()（accent 实心 + accent 外发光），但那套样式的圆角是
 * 全圆胶囊，与其余弹窗页脚的圆角矩形不一致，所以在调用点把 radius 压回 USER_BTN_RADIUS。
 * 不直接改 user_style_btn_primary()：调试页那几颗按钮还在原样用它的胶囊形。 */
lv_obj_t *make_pwd_dlg_ok_btn(lv_obj_t *footer, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_add_style(btn, user_style_btn_primary(), 0);
    lv_obj_set_size(btn, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_BTN_RADIUS, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

void open_dev_password_dialog(void)
{
    if(g_dlg) {
        return;
    }
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("Enter Developer Mode", 0, true);
    g_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_dlg) {
        return;
    }
    /* 顶部对齐，给底部 42% 键盘让位 */
    lv_obj_align(g_dlg->card, LV_ALIGN_TOP_MID, 0, USER_PAD_LG);

    lv_obj_t *hint = ui_user_make_body_label(g_dlg->body, "Enter access password:");
    lv_obj_set_width(hint, LV_PCT(100));

    g_pwd_ta = make_dialog_textarea(g_dlg->body, "Password", LV_KEYBOARD_MODE_NUMBER);
    /* 键盘对勾（LV_EVENT_READY）即校验进入，无需再点 Enter */
    lv_obj_add_event_cb(g_pwd_ta, on_dev_confirm, LV_EVENT_READY, nullptr);

    g_pwd_err = lv_label_create(g_dlg->body);
    lv_label_set_text(g_pwd_err, "");
    lv_obj_set_style_text_color(g_pwd_err, ui_color_danger, 0);
    lv_obj_set_style_text_font(g_pwd_err, user_font_caption(), 0);
    lv_obj_add_flag(g_pwd_err, LV_OBJ_FLAG_HIDDEN);

    ui_user_make_footer_ghost_btn(g_dlg->footer, "Cancel", on_dlg_cancel);

    make_pwd_dlg_ok_btn(g_dlg->footer, "Enter", on_dev_confirm);
}

/* ---- Wi-Fi 连接密码弹窗 ---- */
void on_wifi_connect_confirm(lv_event_t *)
{
    const char *pwd = g_pwd_ta ? lv_textarea_get_text(g_pwd_ta) : "";
    const char *ssid = g_connect_ssid;
    ui::UiMsgProxy::instance().send_wifi_connect(ssid, pwd);
    dlg_close();
    open_connecting_dialog(ssid);   /* 持续显示直到连接结果返回 */
}

void open_wifi_connect_dialog(const char *ssid)
{
    if(g_dlg) {
        return;
    }
    g_connect_ssid = ssid;
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(ssid ? ssid : "Wi-Fi", 0, true);
    g_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_dlg) {
        return;
    }
    lv_obj_align(g_dlg->card, LV_ALIGN_TOP_MID, 0, USER_PAD_LG);

    lv_obj_t *hint = ui_user_make_body_label(g_dlg->body, "Enter Wi-Fi password:");
    lv_obj_set_width(hint, LV_PCT(100));

    g_pwd_ta = make_dialog_textarea(g_dlg->body, "Password", LV_KEYBOARD_MODE_TEXT_LOWER);

    ui_user_make_footer_ghost_btn(g_dlg->footer, "Cancel", on_dlg_cancel);

    make_pwd_dlg_ok_btn(g_dlg->footer, "Connect", on_wifi_connect_confirm);
}

/* ---- 小构件 ---- */
lv_obj_t *make_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, user_color_text_dim, 0);
    lv_obj_set_style_text_font(l, user_font_caption(), 0);
    return l;
}

void on_wifi_item_click(lv_event_t *e)
{
    const char *ssid = static_cast<const char *>(lv_event_get_user_data(e));
    if(wifi_ssid_saved(ssid)) {
        /* 已保存网络：直接连接，无需再次输入密码（后端复用已存凭据） */
        ui::UiMsgProxy::instance().send_wifi_connect(ssid, "");
        open_connecting_dialog(ssid);   /* 持续显示直到连接结果返回 */
    } else {
        open_wifi_connect_dialog(ssid);
    }
}

/* 已连接项：点右箭头 → WiFi 详情子页（忘记网络 / IP 地址） */
void on_connected_wifi_click(lv_event_t *)
{
    net_show_wifi_detail();
}

/* 未连接项：点右箭头 → 空页面（占位） */
void on_wifi_arrow_empty(lv_event_t *)
{
    net_show_empty();
}

/** Wi-Fi 列表项卡片：图标 + SSID + 右箭头。
 *  未连接：点整条连接（已保存免密直连 / 否则弹密码框）。
 *  已连接：整条不响应，断开/忘记/IP 走右箭头详情页。 */
lv_obj_t *make_wifi_item(lv_obj_t *parent, const char *ssid, bool connected)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_NAV_ITEM_H);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_color_text, 0);
    lv_obj_set_style_bg_opa(btn, 10, 0);             /* ≈4% 白底（对齐设计稿） */
    lv_obj_set_style_bg_opa(btn, 28, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(btn, user_color_text, 0);
    /* 未连接=点整条连接（弹密码框/免密直连）；已连接项整条不响应，
     * 断开请走右箭头详情页的 Disconnect。 */
    if(!connected) {
        lv_obj_add_event_cb(btn, on_wifi_item_click,
                            LV_EVENT_CLICKED, const_cast<char *>(ssid));
    }

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, ssid);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(lbl, 1);

    if(connected) {
        lv_obj_t *tag = lv_label_create(btn);
        lv_label_set_text(tag, "Connected");
        lv_obj_set_style_text_font(tag, user_font_caption(), 0);
        lv_obj_set_style_text_color(tag, user_color_status_ok, 0);
    }

    /* 右箭头独立按钮：已连接=详情子页；未连接=空页面。落在此区域的点击不触发整条行。
     * 触发区放大：方形宽度取行高、整行高度，形成舒适的点按目标。 */
    lv_obj_t *arrow_btn = lv_button_create(btn);
    lv_obj_remove_style_all(arrow_btn);
    lv_obj_set_size(arrow_btn, USER_NAV_ITEM_H, LV_PCT(100));
    lv_obj_set_style_radius(arrow_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(arrow_btn, user_color_text, 0);
    lv_obj_set_style_bg_opa(arrow_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(arrow_btn, 28, LV_STATE_PRESSED);
    lv_obj_add_event_cb(arrow_btn,
                        connected ? on_connected_wifi_click : on_wifi_arrow_empty,
                        LV_EVENT_CLICKED, const_cast<char *>(ssid));

    lv_obj_t *arrow = lv_label_create(arrow_btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_text(), 0);
    lv_obj_set_style_text_color(arrow, user_color_text_dim, 0);
    lv_obj_center(arrow);
    return btn;
}

/** 通用条形链接行：整宽按钮 [标题 ... 右箭头]，点击回调 cb。 */
lv_obj_t *make_link_row(lv_obj_t *parent, const char *label,
                        lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_NAV_ITEM_H);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_color_text, 0);
    lv_obj_set_style_bg_opa(btn, 10, 0);
    lv_obj_set_style_bg_opa(btn, 28, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(btn, user_color_text, 0);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_caption(), 0);
    lv_obj_set_style_text_color(arrow, user_color_text_dim, 0);
    return btn;
}

/** 只读键值行：整宽 [标题 ... 值(dim)]，无箭头、不可点。
 *  out_val 非空时回传值标签，供异步刷新就地改文字。 */
lv_obj_t *make_value_row(lv_obj_t *parent, const char *label, const char *value,
                         lv_obj_t **out_val = nullptr)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, USER_NAV_ITEM_H);
    lv_obj_set_style_radius(row, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(row, user_color_text, 0);
    lv_obj_set_style_bg_opa(row, 10, 0);
    lv_obj_set_style_pad_hor(row, USER_PAD, 0);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    lv_obj_set_style_text_color(val, user_color_text_dim, 0);
    if(out_val) {
        *out_val = val;
    }
    return row;
}

/** 子页头部：返回圆钮 + 标题。 */
lv_obj_t *make_subpage_header(lv_obj_t *parent, const char *title,
                             lv_event_cb_t back_cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(row);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, USER_BACK_BTN_H, USER_BACK_BTN_H);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bi = lv_label_create(back);
    lv_label_set_text(bi, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bi, user_color_text, 0);
    lv_obj_set_style_text_font(bi, user_font_text(), 0);
    lv_obj_center(bi);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, user_color_text, 0);
    lv_obj_set_style_text_font(t, user_font_text(), 0);
    return row;
}

/* ---- 详情面板 ---- */

/** 单个分段按钮的选中/未选中样式。 */
void wifi_seg_set_style(lv_obj_t *btn, bool selected)
{
    if(!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, user_color_text, 0);
    lv_obj_set_style_bg_opa(btn, selected ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(btn, selected ? user_color_text
                                              : user_color_text_dim, 0);
}

/** 按 g_wifi_on 刷新两段高亮。 */
void wifi_seg_refresh(void)
{
    wifi_seg_set_style(g_wifi_seg_on,  g_wifi_on);
    wifi_seg_set_style(g_wifi_seg_off, !g_wifi_on);
}

/** 应用 WiFi 开/关（原胶囊开关逻辑，供矩形分段两段复用）。 */
void wifi_apply_enabled(bool on)
{
    g_wifi_on = on;                                    /* 记住开关意图，跨重建保持 */
    ui::UiMsgProxy::instance().send_wifi_enable(on);
    ui_toast_show(on ? "Wi-Fi On" : "Wi-Fi Off", user_color_accent, 1200);
    if(on) {
        ui::UiMsgProxy::instance().send_wifi_scan();   /* 开启后请求扫描 */
    } else {
        g_scan_valid = false;                          /* 关闭后清空扫描缓存 */
    }
    /* 关闭时隐藏下方 WiFi 列表与翻页按钮 */
    if(g_wifi_list_box) {
        if(on) lv_obj_remove_flag(g_wifi_list_box, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(g_wifi_list_box, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_scroll_btns) {
        if(on) lv_obj_remove_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
    }
    wifi_seg_refresh();
}

void on_wifi_seg_on(lv_event_t *)  { wifi_apply_enabled(true);  }
void on_wifi_seg_off(lv_event_t *) { wifi_apply_enabled(false); }

/** 单个矩形分段按钮。 */
lv_obj_t *make_wifi_seg_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, user_font_text(), 0);
    lv_obj_center(l);
    return btn;
}

/** WiFi 开关：矩形分段控件（On | Off），选中段高亮。 */
void make_wifi_seg_toggle(lv_obj_t *parent)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, USER_NAV_ITEM_H * 16 / 5, USER_NAV_ITEM_H * 4 / 5);
    lv_obj_set_style_radius(seg, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(seg, user_color_text, 0);
    lv_obj_set_style_bg_opa(seg, 10, 0);
    lv_obj_set_style_pad_all(seg, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_column(seg, USER_PAD_SM / 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);

    g_wifi_seg_on  = make_wifi_seg_btn(seg, "On",  on_wifi_seg_on);
    g_wifi_seg_off = make_wifi_seg_btn(seg, "Off", on_wifi_seg_off);
    wifi_seg_refresh();
}

void on_wifi_scroll_up(lv_event_t *)
{
    if(g_wifi_list) {
        /* bounded：夹到内容边界，避免翻过头出现空白（无边界滚动） */
        lv_obj_scroll_by_bounded(g_wifi_list, 0, USER_NAV_ITEM_H * 2, LV_ANIM_ON);
    }
}

void on_wifi_scroll_down(lv_event_t *)
{
    if(g_wifi_list) {
        lv_obj_scroll_by_bounded(g_wifi_list, 0, -USER_NAV_ITEM_H * 2, LV_ANIM_ON);
    }
}

/** 右侧竖排圆形翻页按钮（对齐设计稿磨砂上下钮） */
lv_obj_t *make_scroll_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    const int32_t sz = USER_BACK_BTN_H * 5 / 4;   /* 比返回钮略大一圈 */
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

/* ---- 网络子页：回调 ---- */
void on_advanced_click(lv_event_t *)        { net_show_advanced(); }
void on_saved_networks_click(lv_event_t *)
{
    net_show_saved();
    /* 预留端口：进页面时请求后端已保存列表（回来经 register_wifi_saved 刷新） */
    ui::UiMsgProxy::instance().send_wifi_saved_query();
}
void on_net_back_main(lv_event_t *)         { net_show_main(); }
void on_net_back_advanced(lv_event_t *)     { net_show_advanced(); }

void on_forget_network(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_wifi_forget(wifi_current_ssid()); /* 预留端口 */
    ui_toast_show("Network forgotten", user_color_accent, 1500);
    net_show_main();
}

void on_disconnect_network(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_wifi_disconnect(wifi_current_ssid());
    ui_toast_show("Disconnecting...", user_color_accent, 1500);
    net_show_main();
}

void on_forget_saved(lv_event_t *e)
{
    const char *ssid = static_cast<const char *>(lv_event_get_user_data(e));
    ui::UiMsgProxy::instance().send_wifi_forget(ssid);
    ui::UiMsgProxy::instance().send_wifi_saved_query();  /* 请求后端回推最新已保存列表 */
    ui_toast_show("Network forgotten", user_color_accent, 1500);
    net_show_saved();   /* 先按当前缓存刷新，后端回推后再重建 */
}

void build_network_detail(lv_obj_t *parent)
{
    /*
     * 主网络页：WiFi 开关行 +（可隐藏的）列表容器 + 底部 Advanced Settings。
     * 列表整块纵向滚动；翻页按钮做成整屏右侧垂直居中的悬浮层，不随内容滚动。
     */
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_right(parent, USER_BACK_BTN_H * 5 / 4 + USER_PAD, 0); /* 给悬浮翻页按钮留位 */
    g_wifi_list = parent;   /* 翻页按钮滚动整个详情 */

    /* WiFi 开关区：上方 "WiFi" 标签 + 下方矩形分段开关（On | Off） */
    lv_obj_t *sw_sec = lv_obj_create(parent);
    lv_obj_remove_style_all(sw_sec);
    lv_obj_set_width(sw_sec, LV_PCT(100));
    lv_obj_set_height(sw_sec, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sw_sec, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sw_sec, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(sw_sec, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sw_sec, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sw_lbl = lv_label_create(sw_sec);
    lv_label_set_text(sw_lbl, "WiFi");
    lv_obj_set_style_text_color(sw_lbl, user_color_text, 0);
    lv_obj_set_style_text_font(sw_lbl, user_font_text(), 0);

    make_wifi_seg_toggle(sw_sec);

    /* 可隐藏的列表容器（WiFi 关时隐藏） */
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, USER_PAD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    g_wifi_list_box = box;

    const char *cur_ssid = wifi_current_ssid();
    const bool  cur_connected = g_wifi_state_valid && g_wifi_state.connected;

    if(cur_connected) {
        make_caption(box, "Current Wi-Fi");
        make_wifi_item(box, cur_ssid, true);
    }

    make_caption(box, "Available Wi-Fi");
    if(g_scan_valid) {
        /* 后端扫描结果：跳过已连接项，避免与 Current 重复 */
        uint32_t shown = 0;
        for(uint32_t i = 0; i < g_scan_count; i++) {
            const char *ssid = g_scan_wifi[i].ssid;
            if(ssid[0] == '\0') {
                continue;
            }
            if(cur_connected && std::strcmp(ssid, cur_ssid) == 0) {
                continue;
            }
            make_wifi_item(box, ssid, false);
            shown++;
        }
        if(shown == 0) {
            make_caption(box, "No networks found");
        }
    } else {
        /* 首个扫描结果还没到：给等待提示，别拿假 SSID 充数 */
        make_caption(box, "Searching for networks...");
    }

    make_link_row(box, "Advanced Settings", on_advanced_click, nullptr);

    /*
     * 翻页按钮：悬浮在整屏右侧、垂直居中，不随内容滚动。
     * 用 FLOATING 脱离布局/滚动；挂在整页根 g_page 上，故按整屏居中。
     */
    if(g_page) {
        lv_obj_t *col = lv_obj_create(g_page);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_add_flag(col, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, USER_PAD_LG, 0);   /* 上下钮间距放大 */
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(col, LV_ALIGN_RIGHT_MID, -USER_PAD, 0);
        make_scroll_btn(col, LV_SYMBOL_UP, on_wifi_scroll_up);
        make_scroll_btn(col, LV_SYMBOL_DOWN, on_wifi_scroll_down);
        g_scroll_btns = col;
    }

    /* WiFi 关时：隐藏列表容器与翻页按钮（重建后也保持隐藏） */
    if(!g_wifi_on) {
        lv_obj_add_flag(g_wifi_list_box, LV_OBJ_FLAG_HIDDEN);
        if(g_scroll_btns) {
            lv_obj_add_flag(g_scroll_btns, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- 网络子页：高级设置 / 已保存网络 / WiFi 详情 ---- */
void build_wifi_advanced(lv_obj_t *parent)
{
    make_subpage_header(parent, "Advanced Settings", on_net_back_main);

    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, USER_PAD, 0);

    make_link_row(card, "Saved Networks", on_saved_networks_click, nullptr);
    make_value_row(card, "IP Address", wifi_ip_text(), &g_ip_val_lbl);
}

void build_saved_networks(lv_obj_t *parent)
{
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    make_subpage_header(parent, "Saved Networks", on_net_back_advanced);

    /* 后端一旦回过（g_saved_valid），就以真实数据为准——即使为空也显示空，
     * 不再回退占位（否则忘记后仍显示假数据、看似"没反应"）。 */
    const bool use_live = g_saved_valid;
    const uint32_t n = use_live
        ? g_saved_count
        : static_cast<uint32_t>(sizeof(kSavedWifi) / sizeof(kSavedWifi[0]));

    for(uint32_t i = 0; i < n; i++) {
        const char *ssid = use_live ? g_saved_wifi[i].ssid : kSavedWifi[i];

        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, USER_NAV_ITEM_H);
        lv_obj_set_style_radius(row, USER_RADIUS, 0);
        lv_obj_set_style_bg_color(row, user_color_text, 0);
        lv_obj_set_style_bg_opa(row, 10, 0);
        lv_obj_set_style_pad_hor(row, USER_PAD, 0);
        lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(icon, user_color_text, 0);
        lv_obj_set_style_text_font(icon, user_font_text(), 0);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, ssid);
        lv_obj_set_style_text_color(lbl, user_color_text, 0);
        lv_obj_set_style_text_font(lbl, user_font_text(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(lbl, 1);

        lv_obj_t *fb = lv_button_create(row);
        lv_obj_remove_style_all(fb);
        lv_obj_set_style_pad_hor(fb, USER_PAD_SM, 0);
        lv_obj_set_style_pad_ver(fb, USER_PAD_SM / 2, 0);
        lv_obj_set_style_radius(fb, USER_RADIUS, 0);
        lv_obj_set_style_bg_opa(fb, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(fb, 28, LV_STATE_PRESSED);
        lv_obj_add_event_cb(fb, on_forget_saved, LV_EVENT_CLICKED,
                            const_cast<char *>(ssid));
        lv_obj_t *fl = lv_label_create(fb);
        lv_label_set_text(fl, "Forget");
        lv_obj_set_style_text_color(fl, user_color_accent, 0);
        lv_obj_set_style_text_font(fl, user_font_caption(), 0);
    }
}

/* 未连接项点箭头的空页面（占位，后续填内容） */
void build_wifi_empty(lv_obj_t *parent)
{
    make_subpage_header(parent, "Wi-Fi", on_net_back_main);

    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "(coming soon)");
    lv_obj_set_style_text_color(l, user_color_text_dim, 0);
    lv_obj_set_style_text_font(l, user_font_text(), 0);
    lv_obj_set_style_margin_top(l, USER_PAD_LG, 0);
}

void build_wifi_connected_detail(lv_obj_t *parent)
{
    make_subpage_header(parent, wifi_current_ssid(), on_net_back_main);

    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, USER_PAD, 0);

    make_link_row(card, "Disconnect", on_disconnect_network, nullptr);
    make_link_row(card, "Forget Network", on_forget_network, nullptr);
    make_value_row(card, "IP Address", wifi_ip_text(), &g_ip_val_lbl);
}

/* ---- 网络子页切换（清空详情区后重建目标子视图） ---- */
void net_reset(void)
{
    if(g_scroll_btns) {
        lv_obj_del(g_scroll_btns);
        g_scroll_btns = nullptr;
    }
    g_wifi_list = nullptr;
    g_wifi_list_box = nullptr;
    g_wifi_seg_on = nullptr;
    g_wifi_seg_off = nullptr;
    g_ip_val_lbl = nullptr;
    lv_obj_clean(g_detail);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_right(g_detail, 0, 0);
}

void net_show_main(void)        { g_net_subpage = NET_MAIN;     net_reset(); build_network_detail(g_detail); }
void net_show_advanced(void)    { g_net_subpage = NET_ADVANCED; net_reset(); build_wifi_advanced(g_detail); }
void net_show_saved(void)       { g_net_subpage = NET_SAVED;    net_reset(); build_saved_networks(g_detail); }
void net_show_wifi_detail(void) { g_net_subpage = NET_DETAIL;   net_reset(); build_wifi_connected_detail(g_detail); }
void net_show_empty(void)       { g_net_subpage = NET_EMPTY;    net_reset(); build_wifi_empty(g_detail); }

/** About 只读行：[标题 ... 值(dim)]，返回值标签供异步刷新用 */
lv_obj_t *about_add_row(lv_obj_t *card, const char *label, const char *value)
{
    lv_obj_t *row = ui_user_make_setting_row(card, label, nullptr, nullptr);
    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, user_color_text_dim, 0);
    lv_obj_set_style_text_font(val, user_font_text(), 0);
    return val;
}

void build_about_detail(lv_obj_t *parent)
{
    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    about_add_row(card, "Software Version", UPC_SW_VERSION);
    about_add_row(card, "Device Model",     about_device_model_text());
    /* MCU 版本由串口异步返回：先填缓存（首次为 "--"），应答/超时后由 mcu_ver_* 刷新 */
    g_mcu_ver_lbl = about_add_row(card, "MCU Version", g_mcu_ver_text);
    /* 振镜板版本来自 TCP 0x0F 首包缓存：先填缓存，进页 Req 后由 galvo_ver_* 刷新 */
    g_galvo_ver_lbl = about_add_row(card, "Galvo Version", g_galvo_ver_text);
}

void build_device_detail(lv_obj_t *parent)
{
    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    ui_user_make_setting_row(card, "Glavo OTA", "Start", on_glavo_ota_entry_click);
    ui_user_make_setting_row(card, "MCU OTA", "Start", on_mcu_ota_entry_click);
}

void build_placeholder_detail(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text_fmt(l, "%s\n(coming soon)", title);
    lv_obj_set_style_text_color(l, user_color_text_dim, 0);
    lv_obj_set_style_text_font(l, user_font_text(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

void show_detail(NavId id)
{
    if(!g_detail) {
        return;
    }
    /* 清掉上一次的悬浮翻页层（仅网络详情会创建） */
    if(g_scroll_btns) {
        lv_obj_del(g_scroll_btns);
        g_scroll_btns = nullptr;
    }
    g_wifi_list = nullptr;
    g_wifi_list_box = nullptr;
    g_wifi_seg_on = nullptr;
    g_wifi_seg_off = nullptr;
    g_ip_val_lbl = nullptr;
    g_net_subpage = NET_MAIN;   /* 切换左侧导航时复位子页跟踪 */
    /* About 面板即将销毁：先断开异步刷新用的标签指针并停掉版本查询定时器，
     * 否则离开 About 后到达的应答/超时会写已释放的 label。 */
    g_mcu_ver_lbl = nullptr;
    g_galvo_ver_lbl = nullptr;
    mcu_ver_stop_timer();
    lv_obj_clean(g_detail);
    /* 复位详情容器：默认不滚动、无右内边距（网络详情会按需开启） */
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_right(g_detail, 0, 0);

    if(id != NAV_NETWORK) {
        wifi_rescan_stop();
    }

    switch(id) {
    case NAV_NETWORK:
        build_network_detail(g_detail);
        /* 进入网络页：请求当前状态 + 触发一次扫描（结果经 register_wifi_scan 回填），
         * 之后由 wifi_rescan_timer 周期重扫，停在本页也能跟上附近网络变化。 */
        ui::UiMsgProxy::instance().send_wifi_info();
        ui::UiMsgProxy::instance().send_wifi_scan();
        wifi_rescan_start();
        break;
    case NAV_DEVICE:  build_device_detail(g_detail);  break;
    case NAV_ABOUT:
        build_about_detail(g_detail);
        /* 进入 About：向 MCU 查询一次固件版本；向 Glavo 读振镜板 TCP 首包缓存 */
        mcu_ver_query_on_enter();
        galvo_ver_query_on_enter();
        break;
    default:          build_placeholder_detail(g_detail, kNavDefs[id].label); break;
    }
}

void update_nav_highlight(void)
{
    for(int i = 0; i < NAV_COUNT; i++) {
        if(!g_nav_btns[i]) {
            continue;
        }
        const bool active = (i == g_selected);
        if(active) {
            lv_obj_add_state(g_nav_btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_nav_btns[i], LV_STATE_CHECKED);
        }
        /* 图标随选中态着色：选中白、未选灰（与 btn 文字色一致） */
        if(g_nav_icons[i]) {
            lv_obj_set_style_image_recolor(g_nav_icons[i],
                active ? user_color_text : user_color_text_dim, 0);
        }
    }
}

void on_nav_click(lv_event_t *e)
{
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    /* Debug：首次需密码门，本次运行已解锁则直接进调试页（不切换右侧详情） */
    if(id == NAV_DEBUG) {
        if(g_dev_unlocked) {
            ui::PageManager::instance().switch_to("user_debug");
        } else {
            open_dev_password_dialog();
        }
        return;
    }
    g_selected = id;
    update_nav_highlight();
    show_detail(static_cast<NavId>(id));
}

lv_obj_t *make_nav_item(lv_obj_t *parent, NavId id)
{
    const NavDef &d = kNavDefs[id];
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_NAV_ITEM_H);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_color_text, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, 14, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, 14, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(btn, user_color_text_dim, 0);
    lv_obj_set_style_text_color(btn, user_color_text, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn, on_nav_click, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));

    /* 图标尺寸取导航行高的 ~55%（A8 图形本身只占位图约 75%，故不宜再用文字行高，
     * 否则视觉过小）。行高足够（1024档 72 / 1280档 68），不会裁切。 */
    const int32_t icon_sz = USER_NAV_ITEM_H * 5 / 9;
    lv_obj_t *icon = lv_image_create(btn);
    lv_image_set_src(icon, d.icon);
    lv_obj_set_size(icon, icon_sz, icon_sz);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_image_recolor(icon, user_color_text_dim, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    g_nav_icons[id] = icon;

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, d.label);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_caption(), 0);
    lv_obj_set_style_text_color(arrow, user_color_text_dim, 0);

    g_nav_btns[id] = btn;
    return btn;
}

lv_obj_t *build_subnav(lv_obj_t *parent)
{
    lv_obj_t *nav = lv_obj_create(parent);
    lv_obj_remove_style_all(nav);
    lv_obj_set_width(nav, LV_PCT(34));
    lv_obj_set_height(nav, LV_PCT(100));
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nav, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(nav, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nav, LV_SCROLLBAR_MODE_OFF);

    for(int i = 0; i < NAV_COUNT; i++) {
        make_nav_item(nav, static_cast<NavId>(i));
    }
    return nav;
}

} /* namespace */

void page_user_settings_refresh(bool /*force*/)
{
    /* 进页（含从其它页切回）：网络页要重新拉一遍状态并重启周期重扫，
     * 否则缓存里可能是上次离开时的旧状态。其余子导航为静态内容。 */
    if(g_selected == NAV_NETWORK) {
        ui::UiMsgProxy::instance().send_wifi_info();
        ui::UiMsgProxy::instance().send_wifi_scan();
        wifi_rescan_start();
    }
}

void page_user_settings_dismiss_dialog(void)
{
    wifi_rescan_stop();   /* 离开设置页：停掉周期重扫 */
    dlg_close();
    ota_close_dialog(OtaTarget::Glavo);
    ota_close_dialog(OtaTarget::McuSerial);
    close_connecting_dialog();
    mcu_ver_stop_timer();   /* 离开设置页：不再重发版本查询 */
}

lv_obj_t *page_user_settings_create(lv_obj_t *parent)
{
    g_selected   = NAV_NETWORK;
    g_wifi_list  = nullptr;
    g_wifi_list_box = nullptr;
    g_wifi_seg_on = nullptr;
    g_wifi_seg_off = nullptr;
    g_page       = nullptr;
    g_scroll_btns = nullptr;
    g_mcu_ver_lbl = nullptr;
    for(int i = 0; i < NAV_COUNT; i++) {
        g_nav_btns[i]  = nullptr;
        g_nav_icons[i] = nullptr;
    }

    /* 缓存后端网络状态：IP 就地改标签，连接态/SSID 变了才重建列表页——整页重建
     * 会打断用户操作，所以不能每条推送都建。 */
    ui::UiMsgProxy::instance().add_wifi_state_listener(
        [](const middleware::WifiStatePayload &s) {
            const bool was_valid = g_wifi_state_valid;
            const bool conn_changed =
                !was_valid ||
                (g_wifi_state.connected != s.connected) ||
                (std::strcmp(g_wifi_state.ssid, s.ssid) != 0);

            g_wifi_state = s;
            g_wifi_state_valid = true;
            g_wifi_on = (s.enabled != 0);
            wifi_seg_refresh();

            if(g_ip_val_lbl) {
                lv_label_set_text(g_ip_val_lbl, wifi_ip_text());
            }
            if(!conn_changed || g_selected != NAV_NETWORK) {
                return;
            }
            if(g_net_subpage == NET_MAIN) {
                net_show_main();          /* Current Wi-Fi 段随连接态变化 */
            } else if(g_net_subpage == NET_DETAIL && !s.connected) {
                net_show_main();          /* 已断开，详情页无内容可看 */
            }
        });

    /* 缓存扫描结果；若正停在网络主页且条目有变化则用真实列表重建可用网络。 */
    ui::UiMsgProxy::instance().register_wifi_scan(
        [](const middleware::WifiEntryPayload *items, uint32_t count) {
            const uint32_t cap = static_cast<uint32_t>(
                sizeof(g_scan_wifi) / sizeof(g_scan_wifi[0]));
            const uint32_t nn = count > cap ? cap : count;
            for(uint32_t i = 0; i < nn; i++) {
                g_scan_wifi[i] = items[i];
            }
            g_scan_count = nn;
            const bool first = !g_scan_valid;
            g_scan_valid = true;

            std::string sig = scan_list_signature();
            const bool list_changed = first || (sig != g_scan_list_sig);
            g_scan_list_sig = std::move(sig);
            if(list_changed &&
               g_selected == NAV_NETWORK && g_net_subpage == NET_MAIN) {
                net_show_main();   /* 用真实扫描列表重建 */
            }
        });

    /* 预留端口：缓存后端已保存网络列表；若正停在 Saved Networks 页则刷新。 */
    ui::UiMsgProxy::instance().register_wifi_saved(
        [](const middleware::WifiEntryPayload *items, uint32_t count) {
            const uint32_t cap = static_cast<uint32_t>(
                sizeof(g_saved_wifi) / sizeof(g_saved_wifi[0]));
            const uint32_t nn = count > cap ? cap : count;
            for(uint32_t i = 0; i < nn; i++) {
                g_saved_wifi[i] = items[i];
            }
            g_saved_count = nn;
            g_saved_valid = true;
            if(g_selected == NAV_NETWORK && g_net_subpage == NET_SAVED) {
                net_show_saved();   /* 用真实列表重建（不再重发请求，避免回环） */
            }
        });

    /* MCU 固件版本应答：写缓存并刷新 About 页 MCU Version 行。 */
    ui::UiMsgProxy::instance().register_mcu_version(
        [](const middleware::McuVersionPayload &p) {
            if(p.valid) {
                mcu_ver_apply(p);
            }
        });

    /* 振镜控制板固件版本：TCP 0x0F 首包缓存，刷新 About 页 Galvo Version 行。 */
    ui::UiMsgProxy::instance().register_galvo_version(
        [](const middleware::GalvoVersionPayload &p) {
            galvo_ver_apply(p);
        });

    /* 连接结果：关闭"连接中"模态，成功轻提示；失败弹"连接失败"（如密码错/超时）。 */
    ui::UiMsgProxy::instance().register_wifi_connect_result(
        [](bool ok, const char * /*ssid*/) {
            close_connecting_dialog();
            if(ok) {
                ui_toast_show("Connected", user_color_status_ok, 1500);
            } else {
                ui_toast_show("Connect failed", ui_color_danger, 2500);
            }
        });

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page = page;   /* 翻页按钮悬浮层挂这里，整屏居中 */

    /* 主体：左子导航 + 右详情（顶部设备信息条由外壳常驻，见 page_manager） */
    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(body, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(body, USER_PAD, 0);
    lv_obj_set_style_pad_column(body, USER_PAD_LG, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    build_subnav(body);

    g_detail = lv_obj_create(body);
    lv_obj_remove_style_all(g_detail);
    lv_obj_set_height(g_detail, LV_PCT(100));
    lv_obj_set_flex_grow(g_detail, 1);
    lv_obj_set_flex_flow(g_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_detail, USER_PAD, 0);
    lv_obj_set_style_bg_opa(g_detail, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);

    update_nav_highlight();
    show_detail(NAV_NETWORK);

    return page;
}
