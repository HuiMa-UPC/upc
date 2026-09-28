#include "page_user_settings.hpp"
#include "ui_theme.hpp"
#include "ui_backlight.hpp"
#include "ui_user_widgets.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "ui_arc_spinner.hpp"
#include "page_manager.hpp"
#include "settings_icons.h"
#include "ui_msg_proxy.hpp"
#include "debug_msg_proxy.hpp"
#include "control.hpp"
#include "msg_runtime.hpp"
#include "config/cal_config.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
 *   - Debug 行与 About 里的 Galvo/MCU OTA 默认隐藏。About 里连点 Software Version
 *     右侧版本号三次 → 开发者密码框（"888"）→ 本次运行内显示这两处；再点 Debug 进调试页。
 *   - 软件版本号迁至 About 子页。
 *   - Display & Sound：LCD 背光滑条（sysfs `/sys/class/backlight`，1~100%）。
 *   - Advanced Print：打印前门控，默认开，与调试页同一套 Glavo 开关；
 *     Transfer Box 与调试页同一枚本地开关，默认开（取件后走转运盒三步）；
 *     Calibration 内的 Work Height 是测高收到约 1mm 后再向上补的目标高度；
 *     Height Calibration 的 Start 与调试页 Laser 卡相同。
 *   - 子导航图标暂用 LV_SYMBOL 占位，待 SVG 资源到位再替换。
 *   - 软件版本 UPC_SW_VERSION：根 CMakeLists.txt 的 UPC_PRODUCT_VERSION。
 */

#ifndef UPC_SW_VERSION
#error "UPC_SW_VERSION not defined — set UPC_PRODUCT_VERSION in root CMakeLists.txt"
#endif

namespace {

constexpr const char *kDevPassword = "888";

/* ---- 子导航定义 ---- */
/**
 * 子导航项，顺序即稿 6089:11322 左栏从上到下。
 *
 * 相对旧版的三处变动：**新增 Account 与 Storage**；**去掉 Device & Account**——
 * 它装的是 Galvo OTA / MCU OTA 两行，与账号无关，已并进 About；`Display & Sound`
 * 改名 `Display & Brightness`（稿 image1 那帧写的是「显示与声音」，image3~6 都写
 * 「显示与亮度」，且里面确实只有亮度与熄屏、没有声音，以后者为准）。
 */
enum NavId {
    NAV_ACCOUNT = 0,
    NAV_NETWORK,
    NAV_DISPLAY,
    NAV_STORAGE,
    NAV_PRINT,
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
    { &settings_icon_account, "Account"                },
    { &settings_icon_network, "Network & Connectivity" },
    { &settings_icon_display, "Display & Brightness"   },
    { &settings_icon_storage, "Storage"                },
    { &settings_icon_print,   "Advanced Print"         },
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

/* ---- About 页 MCU 固件版本（串口请求应答：UI_Mcu_Version_Ntf → GLAVO_Mcu_Version_Ntf） ----
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

/* ---- About 页振镜控制板固件版本（TCP 0x0F 首包缓存：UI_Galvo_Version_Ntf → GLAVO_Galvo_Version_Ntf）
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
lv_obj_t *g_nav_arrows[NAV_COUNT] = { nullptr };  /**< 右箭头，只在选中项显示 */
/** 右下浮动返回钮。**只有熄屏选择子页用它**（稿 6240:92007）；其余子页仍是顶部
 *  返回条（make_subpage_header），两种并存是稿上就有的，不要顺手统一。 */
lv_obj_t *g_float_back = nullptr;
lv_obj_t *g_detail     = nullptr;   /**< 右侧详情容器（按选中重建） */
lv_obj_t *g_wifi_list  = nullptr;   /**< 网络详情里那段可滚的列表容器 */
lv_obj_t *g_page       = nullptr;   /**< 设置页根：子页浮动返回钮挂这里 */
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

/* 开发者入口进程级解锁：连点 Software Version 三次并输对密码后置位。
 * 置位后左栏 Debug 与 About 的两枚 OTA 才出现；再点 Debug 直接进调试页。
 * 进程(重启 App)后静态量复位，需重新连点并输密码。 */
bool g_dev_unlocked = false;
/** 连点版本号的间隔上限（ms）；超时重新计数，避免零散点击攒满三次。 */
constexpr uint32_t kDevTapGapMs = 2000;
uint8_t  g_sw_ver_taps = 0;
uint32_t g_sw_ver_tap_tick = 0;

/** Advanced Print 门控。开=执行该项检测，和调试页同一条 Glavo 开关。 */
lv_obj_t *g_gate_outer = nullptr;
lv_obj_t *g_gate_inner = nullptr;
lv_obj_t *g_gate_plate = nullptr;
lv_obj_t *g_gate_optical = nullptr;
lv_obj_t *g_gate_height = nullptr;
lv_obj_t *g_gate_o2 = nullptr;
lv_obj_t *g_gate_hold_z = nullptr;
lv_obj_t *g_gate_transfer = nullptr;
lv_obj_t *g_height_calib_lbl = nullptr;
lv_obj_t *g_plate_work_z_ta = nullptr;
/** 回读改开关时挡住 VALUE_CHANGED，避免再发一帧 Set。 */
bool g_gate_applying = false;

enum class PrintGate : uint8_t {
    OuterDoor = 0,
    InnerVision,
    PlatePresent,
    OpticalPath,
    PlateHeight,
    Oxygen,
    HoldPrepZ,
    TransferBox,
};

void apply_dev_entry_visibility(void);
void show_detail(NavId id);
void bind_print_gate_observers(void);
void clear_print_gate_observers(void);
void request_print_gate_state(void);

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
    cfg.light_theme    = true;
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

/* ---- 密码弹窗给键盘让位 ----
 *
 * 稿 6423:51743 上密码弹窗是屏幕居中的，但点输入框会弹出键盘，卡片就被挡住了——
 * 文本键盘占屏幕下半的 42%，密码卡底边（居中时约在 0.72 屏高）正落在里面，Cancel /
 * Connect 两枚按钮会被盖掉。
 *
 * 改稿前的做法是**静态**顶对齐（`LV_ALIGN_TOP_MID` + USER_PAD_LG），不管键盘在不在
 * 都贴着屏幕上沿，居中稿因此落不了地。现在改成动态：键盘弹出时在"屏幕减去键盘"的
 * 剩余矩形里居中，收起后回到整屏居中。
 *
 * 用矩形而不是高度，是因为两种键盘遮的方向不同：文本键盘是下半屏横条（要往上让），
 * 数字键盘是右侧整列竖板（要往左让）。下面按遮挡矩形贴哪条边来判断让哪个方向。 */
void dlg_dodge_keyboard(bool shown, const lv_area_t *obstructed, void * /*ud*/)
{
    if(!g_dlg || !g_dlg->card) {
        return;
    }
    if(!shown || !obstructed) {
        lv_obj_align(g_dlg->card, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    const int32_t scr_w = lv_display_get_horizontal_resolution(disp);
    const int32_t scr_h = lv_display_get_vertical_resolution(disp);
    const int32_t ob_w  = lv_area_get_width(obstructed);
    const int32_t ob_h  = lv_area_get_height(obstructed);

    /* 横条（宽≈满屏）→ 往上让；竖板 → 往没被占的那侧让。 */
    if(ob_w * 2 >= scr_w) {
        const int32_t free_h = obstructed->y1;
        lv_obj_align(g_dlg->card, LV_ALIGN_TOP_MID, 0, (free_h - lv_obj_get_height(g_dlg->card)) / 2);
    } else {
        const bool on_right = (obstructed->x1 * 2 > scr_w - ob_w);
        const int32_t free_w = on_right ? obstructed->x1 : scr_w - obstructed->x2;
        const int32_t x = on_right ? (free_w - lv_obj_get_width(g_dlg->card)) / 2
                                   : obstructed->x2 + (free_w - lv_obj_get_width(g_dlg->card)) / 2;
        lv_obj_align(g_dlg->card, LV_ALIGN_TOP_LEFT, x,
                     (scr_h - lv_obj_get_height(g_dlg->card)) / 2);
    }
    (void)ob_h;
}

void dlg_close(void)
{
    /* 先注销再收键盘：hide 会回调 dlg_dodge_keyboard，而这里正要把 g_dlg 释放掉。 */
    ui_keyboard_set_visibility_cb(nullptr, nullptr);
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
    cfg.light_theme = true;
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

/* 输入框统一样式。稿 6423:51893 是满宽、高 64、圆角 14、底 `#333145 @5%`、无描边，
 * placeholder 与输入的字都是深墨（前者压到 40%）。 */
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
    lv_obj_set_style_radius(ta, USER_LIGHT_RADIUS * 7 / 12, 0);   /* 稿 14@1280 */
    lv_obj_set_style_bg_color(ta, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(ta, 13, 0);                           /* 稿 #333145 @5% */
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_text_color(ta, user_light_text, 0);
    lv_obj_set_style_text_color(ta, user_light_text, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_opa(ta, LV_OPA_40, LV_PART_TEXTAREA_PLACEHOLDER);
    ui_keyboard_attach(ta, kb_mode);
    return ta;
}

/* 稿 6423:51893 页脚那两枚等宽按钮：Cancel 是 `#333145 @10%` 底配深墨字，
 * Connect 是深墨实心配白字，各占半宽、圆角 12。
 * 不走 ui_user_make_confirm_footer_btn：那枚是「取消 / 危险红」的语义配色，
 * 这里的主按钮是中性深墨而不是红。 */
lv_obj_t *make_pwd_dlg_btn(lv_obj_t *footer, const char *text, bool primary,
                           lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(btn, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(btn, primary ? LV_OPA_COVER : LV_OPA_10, 0);
    lv_obj_set_style_bg_opa(btn, primary ? LV_OPA_80 : LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, primary ? lv_color_white() : user_light_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_dlg_btn(), 0);
    lv_obj_center(lbl);
    return btn;
}

/* ---- 开发者密码弹窗 → 通过则进调试页 ---- */
void on_dev_confirm(lv_event_t *)
{
    if(!g_pwd_ta) {
        return;
    }
    const char *input = lv_textarea_get_text(g_pwd_ta);
    if(input && std::strcmp(input, kDevPassword) == 0) {
        g_dev_unlocked = true;
        g_sw_ver_taps = 0;
        dlg_close();
        apply_dev_entry_visibility();
        /* 密码是在 About 里连点版本号呼出的：解锁后留在本页，OTA 行随详情重建出现。 */
        if(g_selected == NAV_ABOUT) {
            show_detail(NAV_ABOUT);
        }
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

/* 两个密码弹窗共用的左对齐灰字说明行（稿 6423:51893 的「输入网络密码」那行）。 */
lv_obj_t *make_pwd_dlg_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);
    lv_obj_set_style_text_font(lbl, user_font_text_sm(), 0);
    return lbl;
}

void open_dev_password_dialog(void)
{
    if(g_dlg) {
        return;
    }
    /* 与 Wi-Fi 密码弹窗同一套外壳（稿只给了后者，这里对齐它）：白卡、560 宽、
     * 页脚两枚等宽钮。它走数字键盘，弹出的是右侧竖板，让位方向由 dlg_dodge_keyboard 判。 */
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("Enter Developer Mode", USER_WIFI_DLG_W, true);
    cfg.light_theme = true;
    g_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_dlg) {
        return;
    }
    ui_keyboard_set_visibility_cb(dlg_dodge_keyboard, nullptr);

    make_pwd_dlg_hint(g_dlg->body, "Enter access password");

    g_pwd_ta = make_dialog_textarea(g_dlg->body, "Password", LV_KEYBOARD_MODE_NUMBER);
    /* 键盘对勾（LV_EVENT_READY）即校验进入，无需再点 Enter */
    lv_obj_add_event_cb(g_pwd_ta, on_dev_confirm, LV_EVENT_READY, nullptr);

    g_pwd_err = lv_label_create(g_dlg->body);
    lv_label_set_text(g_pwd_err, "");
    lv_obj_set_style_text_color(g_pwd_err, ui_color_danger, 0);
    lv_obj_set_style_text_font(g_pwd_err, user_font_caption(), 0);
    lv_obj_add_flag(g_pwd_err, LV_OBJ_FLAG_HIDDEN);

    make_pwd_dlg_btn(g_dlg->footer, "Cancel", false, on_dlg_cancel);
    make_pwd_dlg_btn(g_dlg->footer, "Enter", true, on_dev_confirm);
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

/* 稿 6423:51743（整屏）/ 51893（弹窗本体）：560×334 白卡，SSID 居中作标题，
 * 下面一行左对齐灰字，满宽输入框，页脚 Cancel / Connect 两枚等宽钮。 */
void open_wifi_connect_dialog(const char *ssid)
{
    if(g_dlg) {
        return;
    }
    g_connect_ssid = ssid;
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user(ssid ? ssid : "Wi-Fi", USER_WIFI_DLG_W, true);
    cfg.light_theme = true;
    g_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_dlg) {
        return;
    }
    ui_keyboard_set_visibility_cb(dlg_dodge_keyboard, nullptr);

    make_pwd_dlg_hint(g_dlg->body, "Enter network password");

    g_pwd_ta = make_dialog_textarea(g_dlg->body, "Password", LV_KEYBOARD_MODE_TEXT_LOWER);

    make_pwd_dlg_btn(g_dlg->footer, "Cancel", false, on_dlg_cancel);
    make_pwd_dlg_btn(g_dlg->footer, "Connect", true, on_wifi_connect_confirm);
}

/* ---- 小构件 ---- */
/** 右栏分组标题（`Current Wi-Fi` / `Available Wi-Fi` 这类）。
 *  稿 3361:18547/18558 是「一级标题」20、纯白；这里字号跟稿，颜色仍用 dim 保持
 *  与下方行内容的层次（稿靠字重区分，Montserrat 只有一个字重，做不出来）。 */
/** 分区标题（稿 6101:14249 是 Poppins SemiBold 22 / #333145）。 */
lv_obj_t *make_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, user_light_text, 0);
    /* 稿 22；与浅色档弹窗页脚钮同档，走 user_font_dlg_btn() */
    lv_obj_set_style_text_font(l, user_font_dlg_btn(), 0);
    return l;
}

/**
 * 右栏卡片的统一外观（稿 6101:14257 那一款，整页所有行卡都是它）：
 * 白底实色 + 1px 白 10% 描边 + `0 2 6 rgba(0,0,0,.04)` 投影 + 圆角 16 + 内距 24。
 *
 * 白底压白描边等于看不见，稿上就这么画的，照留；真正撑出边界的是那道投影。
 * 与历史页/消息页卡片同款，改一处三页要同步看。
 */
void apply_card_style(lv_obj_t *obj)
{
    lv_obj_set_style_radius(obj, USER_HIST_CARD_RADIUS, 0);      /* 稿 16 */
    lv_obj_set_style_bg_color(obj, user_light_card, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_white(), 0);
    lv_obj_set_style_border_opa(obj, 26, 0);                     /* 稿 白 10% */
    lv_obj_set_style_shadow_width(obj, 6, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 2, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(obj, 10, 0);                     /* 稿 4% ≈ 10/255 */
    lv_obj_set_style_pad_all(obj, USER_PAD, 0);                  /* 稿 24 */
    lv_obj_set_style_pad_column(obj, USER_PAD, 0);
    /* 按下压一档灰：不能改 bg_opa，白卡透下去会露出页面底 */
    lv_obj_set_style_bg_color(obj, user_light_more_bg, LV_STATE_PRESSED);
}

/**
 * 竖排白卡容器。
 *
 * 替掉本页原先各处的 `ui_user_make_frosted_card()`——那是深色档的磨砂卡
 * （半透明白 + 白字），整屏翻浅之后压在 #FAFAFA 上就只剩一片白。
 * 调试页已改用自有 `make_dbg_light_card()`。
 */
lv_obj_t *make_light_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    apply_card_style(card);
    lv_obj_set_style_bg_color(card, user_light_card, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(card, user_light_text, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
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
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    apply_card_style(btn);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(btn, user_light_text, 0);
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
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);   /* 稿「一级标题」20 */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(lbl, 1);

    if(connected) {
        lv_obj_t *tag = lv_label_create(btn);
        lv_label_set_text(tag, "Connected");
        lv_obj_set_style_text_font(tag, user_font_caption(), 0);
        lv_obj_set_style_text_color(tag, user_color_status_ok, 0);
    }

    /* 右箭头独立按钮：已连接=详情子页；未连接=空页面。落在此区域的点击不触发整条行。
     * 卡片内距已是 24，钮本身只要够大即可，故取行高档的 USER_NAV_ITEM_H 见方。 */
    lv_obj_t *arrow_btn = lv_button_create(btn);
    lv_obj_remove_style_all(arrow_btn);
    lv_obj_set_size(arrow_btn, USER_NAV_ITEM_H / 2, USER_NAV_ITEM_H / 2);
    lv_obj_set_style_radius(arrow_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(arrow_btn, user_light_text, 0);
    lv_obj_set_style_bg_opa(arrow_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(arrow_btn, 26, LV_STATE_PRESSED);
    lv_obj_add_event_cb(arrow_btn,
                        connected ? on_connected_wifi_click : on_wifi_arrow_empty,
                        LV_EVENT_CLICKED, const_cast<char *>(ssid));

    lv_obj_t *arrow = lv_label_create(arrow_btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_text(), 0);
    lv_obj_set_style_text_color(arrow, user_light_text, 0);
    lv_obj_set_style_text_opa(arrow, LV_OPA_50, 0);
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
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    apply_card_style(btn);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(btn, user_light_text, 0);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);   /* 稿「一级标题」20 */
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_text(), 0);
    lv_obj_set_style_text_color(arrow, user_light_text, 0);
    lv_obj_set_style_text_opa(arrow, LV_OPA_50, 0);
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
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    apply_card_style(row);
    lv_obj_set_style_bg_color(row, user_light_card, LV_STATE_PRESSED);  /* 不可点，不给按压反馈 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);   /* 稿「一级标题」20 */
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val, user_font_title(), 0);
    lv_obj_set_style_text_color(val, user_light_text, 0);
    lv_obj_set_style_text_opa(val, LV_OPA_50, 0);
    if(out_val) {
        *out_val = val;
    }
    return row;
}

/** 可点的键值行：[标题 ... 值(50%) ›]。稿 6235:91471 的「熄屏时间 5分钟 ›」那一款。 */
lv_obj_t *make_value_row_clickable(lv_obj_t *parent, const char *label,
                                   const char *value, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    apply_card_style(btn);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(btn);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val, user_font_title(), 0);
    lv_obj_set_style_text_color(val, user_light_text, 0);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_text(), 0);
    lv_obj_set_style_text_color(arrow, user_light_text, 0);
    lv_obj_set_style_text_opa(arrow, LV_OPA_50, 0);
    return btn;
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
    lv_obj_set_style_bg_color(back, user_light_card, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, user_light_more_bg, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bi = lv_label_create(back);
    lv_label_set_text(bi, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bi, user_light_text, 0);
    lv_obj_set_style_text_font(bi, user_font_text(), 0);
    lv_obj_center(bi);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, user_light_text, 0);
    lv_obj_set_style_text_font(t, user_font_dlg_btn(), 0);   /* 同分区标题 22 */
    return row;
}

/* ---- 详情面板 ---- */

/** 单个分段按钮的选中/未选中样式。 */
void wifi_seg_set_style(lv_obj_t *btn, bool selected)
{
    if(!btn) {
        return;
    }
    /* 稿 6101:14231：选中段是**白色实块**（不是半透明的深色），未选中全透。 */
    lv_obj_set_style_bg_color(btn, user_light_card, 0);
    lv_obj_set_style_bg_opa(btn, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(btn, user_light_text, 0);
    lv_obj_set_style_text_opa(btn, selected ? LV_OPA_COVER : LV_OPA_50, 0);
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
    /* 关闭时隐藏下方 WiFi 列表 */
    if(g_wifi_list_box) {
        if(on) lv_obj_remove_flag(g_wifi_list_box, LV_OBJ_FLAG_HIDDEN);
        else   lv_obj_add_flag(g_wifi_list_box, LV_OBJ_FLAG_HIDDEN);
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
    /* 稿 14 = 外框 16 减去 2 的内距，两个圆角才同心 */
    lv_obj_set_style_radius(btn, USER_HIST_CARD_RADIUS - 2, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, user_font_title(), 0);
    lv_obj_center(l);
    return btn;
}

/** WiFi 开关：矩形分段控件（On | Off），选中段高亮。 */
void make_wifi_seg_toggle(lv_obj_t *parent)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    /* 稿 6101:14231 是 256×72、圆角 16、内距 2、段间 4，底 rgba(51,49,69,0.04)。
     * 行高是稿的 80 之后旧式的 *16/5 正好算出 256，宽度不用另给；高度那项 *4/5
     * 只有 64，所以 1280 档单独走 USER_SETTINGS_SEG_H。
     * **它不跟其余卡片一样满 660 宽**——稿上这一处就是这么窄的。 */
    lv_obj_set_size(seg, USER_NAV_ITEM_H * 16 / 5,
                    USER_SETTINGS_SEG_H > 0 ? USER_SETTINGS_SEG_H
                                            : USER_NAV_ITEM_H * 4 / 5);
    lv_obj_set_style_radius(seg, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(seg, user_light_text, 0);
    lv_obj_set_style_bg_opa(seg, 10, 0);            /* 稿 4% */
    lv_obj_set_style_pad_all(seg, 2, 0);
    lv_obj_set_style_pad_column(seg, 4, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);

    g_wifi_seg_on  = make_wifi_seg_btn(seg, "On",  on_wifi_seg_on);
    g_wifi_seg_off = make_wifi_seg_btn(seg, "Off", on_wifi_seg_off);
    wifi_seg_refresh();
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
     * 主网络页：WiFi 开关段 +（可隐藏的）列表容器 + 底部 Advanced Settings。
     * 列表整块纵向滚动——扫到的网络条数不定，超出一屏就得能划。
     * **右内距不要在这里改**：它是右栏的稿值 64，改了整页横向就偏了
     * （旧版曾在此加宽到 79 给悬浮翻页钮让位，钮已按新稿删掉）。
     */
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    g_wifi_list = parent;

    /* WiFi 开关区：上方 "WiFi" 标签 + 下方矩形分段开关（On | Off） */
    lv_obj_t *sw_sec = lv_obj_create(parent);
    lv_obj_remove_style_all(sw_sec);
    lv_obj_set_width(sw_sec, LV_PCT(100));
    lv_obj_set_height(sw_sec, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sw_sec, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sw_sec, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(sw_sec, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sw_sec, LV_OBJ_FLAG_SCROLLABLE);

    /* 分区标题走 make_caption，与本页其余分区同款（稿 6101:14249，22/#333145）。
     * 这里原先是自己写的白字 20——白字压在 #FAFAFA 上等于看不见。 */
    make_caption(sw_sec, "Wi-Fi");

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

    /* WiFi 关时：隐藏列表容器（重建后也保持隐藏） */
    if(!g_wifi_on) {
        lv_obj_add_flag(g_wifi_list_box, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- 网络子页：高级设置 / 已保存网络 / WiFi 详情 ---- */
void build_wifi_advanced(lv_obj_t *parent)
{
    make_subpage_header(parent, "Advanced Settings", on_net_back_main);

    lv_obj_t *card = make_light_card(parent);
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
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        apply_card_style(row);
        lv_obj_set_style_bg_color(row, user_light_card, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(icon, user_light_text, 0);
        lv_obj_set_style_text_font(icon, user_font_text(), 0);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, ssid);
        lv_obj_set_style_text_color(lbl, user_light_text, 0);
        lv_obj_set_style_text_font(lbl, user_font_title(), 0);   /* 稿「一级标题」20 */
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
    lv_obj_set_style_text_color(l, user_light_text, 0);
    lv_obj_set_style_text_opa(l, LV_OPA_50, 0);
    lv_obj_set_style_text_font(l, user_font_title(), 0);
    lv_obj_set_style_margin_top(l, USER_PAD_LG, 0);
}

void build_wifi_connected_detail(lv_obj_t *parent)
{
    make_subpage_header(parent, wifi_current_ssid(), on_net_back_main);

    lv_obj_t *card = make_light_card(parent);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, USER_PAD, 0);

    make_link_row(card, "Disconnect", on_disconnect_network, nullptr);
    make_link_row(card, "Forget Network", on_forget_network, nullptr);
    make_value_row(card, "IP Address", wifi_ip_text(), &g_ip_val_lbl);
}

/* ---- 网络子页切换（清空详情区后重建目标子视图） ---- */
void net_reset(void)
{
    g_wifi_list = nullptr;
    g_wifi_list_box = nullptr;
    g_wifi_seg_on = nullptr;
    g_wifi_seg_off = nullptr;
    g_ip_val_lbl = nullptr;
    lv_obj_clean(g_detail);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);
}

void net_show_main(void)        { g_net_subpage = NET_MAIN;     net_reset(); build_network_detail(g_detail); }
void net_show_advanced(void)    { g_net_subpage = NET_ADVANCED; net_reset(); build_wifi_advanced(g_detail); }
void net_show_saved(void)       { g_net_subpage = NET_SAVED;    net_reset(); build_saved_networks(g_detail); }
void net_show_wifi_detail(void) { g_net_subpage = NET_DETAIL;   net_reset(); build_wifi_connected_detail(g_detail); }
void net_show_empty(void)       { g_net_subpage = NET_EMPTY;    net_reset(); build_wifi_empty(g_detail); }

/** 连点 Software Version 右侧版本号三次，呼出开发者密码。已解锁则忽略。 */
void on_sw_version_click(lv_event_t * /*e*/)
{
    if(g_dev_unlocked) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if(g_sw_ver_taps == 0 || lv_tick_elaps(g_sw_ver_tap_tick) > kDevTapGapMs) {
        g_sw_ver_taps = 1;
    } else {
        ++g_sw_ver_taps;
    }
    g_sw_ver_tap_tick = now;
    if(g_sw_ver_taps >= 3) {
        g_sw_ver_taps = 0;
        open_dev_password_dialog();
    }
}

/** About 只读行：[标题 ... 值(dim)]，返回值标签供异步刷新用 */
lv_obj_t *about_add_row(lv_obj_t *card, const char *label, const char *value)
{
    lv_obj_t *row = ui_user_make_setting_row(card, label, nullptr, nullptr);
    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, user_light_text, 0);
    lv_obj_set_style_text_opa(val, LV_OPA_50, 0);
    lv_obj_set_style_text_font(val, user_font_title(), 0);
    return val;
}

void build_about_detail(lv_obj_t *parent)
{
    lv_obj_t *card = make_light_card(parent);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *sw_ver = about_add_row(card, "Software Version", UPC_SW_VERSION);
    /* 版本号本身可点：连点三次呼出开发者密码。行标题不响应。 */
    lv_obj_add_flag(sw_ver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sw_ver, 16);
    lv_obj_add_event_cb(sw_ver, on_sw_version_click, LV_EVENT_CLICKED, nullptr);
    about_add_row(card, "Device Model",     about_device_model_text());
    /* MCU 版本由串口异步返回：先填缓存（首次为 "--"），应答/超时后由 mcu_ver_* 刷新 */
    g_mcu_ver_lbl = about_add_row(card, "MCU Version", g_mcu_ver_text);
    /* 振镜板版本来自 TCP 0x0F 首包缓存：先填缓存，进页 Req 后由 galvo_ver_* 刷新 */
    g_galvo_ver_lbl = about_add_row(card, "Galvo Version", g_galvo_ver_text);

    /* 两枚 OTA 入口原本挂在「Device & Account」下。日常隐藏，与左栏 Debug 一起
     * 在连点版本号并输对密码后才出现。 */
    if(g_dev_unlocked) {
        ui_user_make_setting_row(card, "Galvo OTA", "Start", on_glavo_ota_entry_click);
        ui_user_make_setting_row(card, "MCU OTA", "Start", on_mcu_ota_entry_click);
    }
}

lv_obj_t *g_bl_pct_lbl = nullptr;

void backlight_refresh_pct_label(int percent)
{
    if(!g_bl_pct_lbl) {
        return;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(g_bl_pct_lbl, buf);
}

void on_backlight_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    if(!sl) {
        return;
    }
    const int32_t v = lv_slider_get_value(sl);
    (void)ui::backlight_set_percent(static_cast<int>(v));
    backlight_refresh_pct_label(static_cast<int>(v));
}

/* ---- 熄屏时间（稿 6235:91471 / 6240:92007）----
 *
 * **纯 UI 占位**：选项只存在这个静态变量里，不落盘、也不真的熄屏。
 * 工程里目前连背光超时都没有（背光亮度本身也不落盘，重启回默认），要真做得先定
 * 熄屏策略（空闲计时器 + 背光置 0 或 sysfs blank + 触摸唤醒），那是另一件事。
 */
constexpr const char *kSleepOptions[] = {
    "30 sec", "1 min", "2 min", "3 min", "4 min", "5 min", "Never",
};
constexpr int kSleepOptionCount =
    static_cast<int>(sizeof(kSleepOptions) / sizeof(kSleepOptions[0]));
int g_sleep_idx = 5;   /**< 默认 5 min，与稿上的选中项一致 */

void disp_show_main(void);
void disp_show_sleep(void);

void on_sleep_row_click(lv_event_t *)   { disp_show_sleep(); }
void on_sleep_back(lv_event_t *)        { disp_show_main(); }

void on_sleep_pick(lv_event_t *e)
{
    const int idx = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if(idx >= 0 && idx < kSleepOptionCount) {
        g_sleep_idx = idx;
    }
    disp_show_main();
}

/** 亮度卡（稿 6235:91455：column / 内距 24 / 行距 24）+ 熄屏时间行。 */
void build_display_detail(lv_obj_t *parent)
{
    const int32_t slider_touch_h = (USER_PROGRESS_H * 2 > 24) ? USER_PROGRESS_H * 2 : 24;
    const int32_t knob_sz = (USER_PROGRESS_H * 3 > 28) ? USER_PROGRESS_H * 3 : 28;

    /* ---- Screen Brightness ---- */
    lv_obj_t *sec = lv_obj_create(parent);
    lv_obj_remove_style_all(sec);
    lv_obj_set_width(sec, LV_PCT(100));
    lv_obj_set_height(sec, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec, USER_PAD_SM, 0);   /* 稿 标题↔卡片 16 */
    lv_obj_set_style_bg_opa(sec, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(sec, "Screen Brightness");

    lv_obj_t *card = lv_obj_create(sec);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    apply_card_style(card);
    lv_obj_set_style_bg_color(card, user_light_card, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    int pct = ui::backlight_get_percent();
    if(pct < 5)   { pct = (pct < 0) ? 80 : 5; }
    if(pct > 100) { pct = 100; }

    /* 稿 6235:91455 的滑条左右各有一枚小/大太阳图标盒；工程没有那两个矢量资源，
     * 用同尺寸的百分比标签顶上——信息量只多不少，且亮度值本来就该看得见。 */
    g_bl_pct_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(g_bl_pct_lbl, user_light_text, 0);
    lv_obj_set_style_text_opa(g_bl_pct_lbl, LV_OPA_50, 0);
    lv_obj_set_style_text_font(g_bl_pct_lbl, user_font_title(), 0);
    backlight_refresh_pct_label(pct);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, slider_touch_h);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, user_light_text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, 26, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    /* 稿上填充段是深色 #333145，不是主色蓝 */
    lv_obj_set_style_bg_color(slider, user_light_text, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, user_light_card, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, user_light_divider, LV_PART_KNOB);
    lv_obj_set_style_width(slider, knob_sz, LV_PART_KNOB);
    lv_obj_set_style_height(slider, knob_sz, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, USER_PAD);
    lv_obj_add_event_cb(slider, on_backlight_slider, LV_EVENT_VALUE_CHANGED, nullptr);

    /* ---- Sleep Timeout ---- */
    lv_obj_t *sec2 = lv_obj_create(parent);
    lv_obj_remove_style_all(sec2);
    lv_obj_set_width(sec2, LV_PCT(100));
    lv_obj_set_height(sec2, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sec2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec2, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(sec2, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sec2, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(sec2, "Sleep Timeout");
    make_value_row_clickable(sec2, "Sleep Timeout", kSleepOptions[g_sleep_idx],
                             on_sleep_row_click);
}

/** 熄屏时间选择子页（稿 6240:92007）：一张白卡里 7 行单选，选中项右侧蓝勾。 */
void build_sleep_detail(lv_obj_t *parent)
{
    lv_obj_t *sec = lv_obj_create(parent);
    lv_obj_remove_style_all(sec);
    lv_obj_set_width(sec, LV_PCT(100));
    lv_obj_set_height(sec, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(sec, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(sec, "Sleep Timeout");

    /* 稿上 7 行是**一整张卡**、行间以分隔线断开，不是 7 张卡 */
    lv_obj_t *card = lv_obj_create(sec);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    apply_card_style(card);
    lv_obj_set_style_bg_color(card, user_light_card, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card, 0, 0);   /* 内距落到每一行上，分隔线才能通栏 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < kSleepOptionCount; i++) {
        lv_obj_t *row = lv_button_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, USER_PAD, 0);
        lv_obj_set_style_bg_color(row, user_light_more_bg, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, on_sleep_pick, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, kSleepOptions[i]);
        lv_obj_set_style_text_font(lbl, user_font_title(), 0);
        lv_obj_set_style_text_color(lbl, user_light_text, 0);
        lv_obj_set_flex_grow(lbl, 1);

        if(i == g_sleep_idx) {
            lv_obj_t *tick = lv_label_create(row);
            lv_label_set_text(tick, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(tick, user_font_text(), 0);
            lv_obj_set_style_text_color(tick, user_light_accent, 0);
        }
        if(i + 1 < kSleepOptionCount) {
            /* 那个 helper 默认上深色档的分隔色，压在白卡上太重，换浅色档的 */
            lv_obj_t *ln = ui_user_make_divider_line(card, LV_PCT(100));
            lv_obj_set_style_bg_color(ln, user_light_divider, 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
        }
    }

    /* 稿 6240:92007 右下那枚浮动返回钮。**只有这一页用**，其余子页仍是顶部返回条。 */
    if(g_page) {
        lv_obj_t *back = lv_button_create(g_page);
        lv_obj_remove_style_all(back);
        lv_obj_add_flag(back, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(back, USER_LIGHT_BTN_SZ, USER_LIGHT_BTN_SZ);
        apply_card_style(back);
        lv_obj_set_style_pad_all(back, 0, 0);
        lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_LG, -USER_PAD_LG);
        lv_obj_add_event_cb(back, on_sleep_back, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *ico = lv_label_create(back);
        lv_label_set_text(ico, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(ico, user_font_title(), 0);
        lv_obj_set_style_text_color(ico, user_light_text, 0);
        lv_obj_center(ico);
        g_float_back = back;
    }
}

/* ============================================================================
 *  存储管理（稿 6251:94362）
 *
 *  **只做前端**：容量走现成的 UI_Storage_Usage_Req（media 侧只实现了主机那一卷，
 *  SD 卡回 present=0）；格式化那三条消息是新留的契约，后端未实现，发出去没人应答。
 *  后端要补什么见 doc/STORAGE_BACKEND_API.md。
 * ========================================================================= */

struct VolCard {
    lv_obj_t *bar   = nullptr;   /**< 容量条的填充段 */
    lv_obj_t *value = nullptr;   /**< 右下角的剩余量文字 */
};
VolCard g_vol_cards[2];          /**< 0=Internal 1=SdCard，与稿上两张卡同序 */
lv_obj_t *g_fmt_usb_btn = nullptr;   /**< U 盘格式化钮，没插盘时置灰 */
bool g_usb_present = false;
ui_modal_dialog_t *g_fmt_dlg = nullptr;
middleware::StorageVolume g_fmt_target = middleware::StorageVolume::Internal;

void fmt_dlg_close(void)
{
    if(g_fmt_dlg) {
        ui_modal_dialog_close(g_fmt_dlg);
        g_fmt_dlg = nullptr;
    }
}

void bytes_to_text(uint64_t bytes, char *buf, size_t n)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if(gb >= 1.0) { std::snprintf(buf, n, "%.0f GB", gb); }
    else          { std::snprintf(buf, n, "%.0f MB", gb * 1024.0); }
}

/** 一张容量卡（稿 6285:12447：column / 内距 20 / 行距 12）。 */
void make_volume_card(lv_obj_t *parent, int slot, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    apply_card_style(card);
    lv_obj_set_style_bg_color(card, user_light_card, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card, USER_PAD_SM + 4, 0);   /* 稿 20 */
    lv_obj_set_style_pad_row(card, USER_PAD_SM * 3 / 4, 0);   /* 稿 12 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, user_font_title(), 0);
    lv_obj_set_style_text_color(t, user_light_text, 0);

    lv_obj_t *track = lv_obj_create(card);
    lv_obj_remove_style_all(track);
    lv_obj_set_width(track, LV_PCT(100));
    lv_obj_set_height(track, USER_PROGRESS_H / 2 > 4 ? USER_PROGRESS_H / 2 : 4);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, user_light_text, 0);
    lv_obj_set_style_bg_opa(track, 26, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_height(fill, LV_PCT(100));
    lv_obj_set_width(fill, 0);
    lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fill, user_light_text, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, "Free space");
    lv_obj_set_style_text_font(cap, user_font_text(), 0);
    lv_obj_set_style_text_color(cap, user_light_text, 0);
    lv_obj_set_style_text_opa(cap, LV_OPA_50, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, user_font_title(), 0);
    lv_obj_set_style_text_color(val, user_light_text, 0);

    g_vol_cards[slot].bar   = fill;
    g_vol_cards[slot].value = val;
}

/** 收到容量回执：更新对应那张卡。present=0 时显示 `--` 并把条清零。 */
void storage_apply_usage(const middleware::StorageUsagePayload &p)
{
    const int slot = (p.volume == static_cast<uint8_t>(middleware::StorageVolume::SdCard))
                     ? 1 : 0;
    if(p.volume == static_cast<uint8_t>(middleware::StorageVolume::UsbDisk)) {
        return;   /* U 盘不在概览里，只在格式化那一行 */
    }
    VolCard &c = g_vol_cards[slot];
    if(!c.value || !c.bar) {
        return;
    }
    if(!p.present || p.total_bytes == 0) {
        lv_label_set_text(c.value, "--");
        lv_obj_set_width(c.bar, 0);
        return;
    }
    char buf[32];
    bytes_to_text(p.total_bytes - p.used_bytes, buf, sizeof(buf));
    lv_label_set_text(c.value, buf);
    const int32_t pct = static_cast<int32_t>(p.used_bytes * 100u / p.total_bytes);
    lv_obj_set_width(c.bar, lv_pct(pct > 100 ? 100 : pct));
}

void on_fmt_cancel(lv_event_t *) { fmt_dlg_close(); }

void on_fmt_confirm(lv_event_t *)
{
    fmt_dlg_close();
    ui::UiMsgProxy::instance().send_storage_format(g_fmt_target);
    /* 后端未实现，这条发出去不会有回执；toast 明说，免得用户以为在跑 */
    ui_toast_show("Format requested (backend pending)", user_color_accent, 2500);
}

/** 格式化二次确认（稿 6283:26740）：复用历史页那个浅色档弹窗。 */
void open_format_confirm(middleware::StorageVolume vol, const char *what)
{
    if(g_fmt_dlg) {
        return;
    }
    g_fmt_target = vol;
    char title[64];
    std::snprintf(title, sizeof(title), "Format %s?", what);

    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user(title, USER_DLG_W, true);
    cfg.light_theme = true;
    g_fmt_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_fmt_dlg) {
        return;
    }
    lv_obj_t *body = ui_user_make_body_label(g_fmt_dlg->body,
                                             "All data will be erased.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    /* 同历史页删除弹窗：稿是 rgba(51,49,69,0.5)，用实色 + 50% 文字不透明度压出来 */
    lv_obj_set_style_text_color(body, user_light_text, 0);
    lv_obj_set_style_text_opa(body, LV_OPA_50, 0);
    lv_obj_set_style_text_font(body, user_font_title(), 0);

    ui_user_make_confirm_footer_btn(g_fmt_dlg->footer, "Cancel", false,
                                    user_color_panel_alt, true,
                                    on_fmt_cancel, nullptr);
    ui_user_make_confirm_footer_btn(g_fmt_dlg->footer, "Confirm", true,
                                    user_color_status_err, true,
                                    on_fmt_confirm, nullptr);
}

void on_fmt_internal(lv_event_t *)
{
    open_format_confirm(middleware::StorageVolume::Internal, "internal storage");
}
void on_fmt_usb(lv_event_t *)
{
    if(!g_usb_present) {
        ui_toast_show("No USB drive", user_color_text_dim, 1500);
        return;
    }
    open_format_confirm(middleware::StorageVolume::UsbDisk, "USB drive");
}
void on_album_click(lv_event_t *) { ui_toast_show("Not implemented", user_color_text_dim, 1200); }
void on_model_click(lv_event_t *) { ui_toast_show("Not implemented", user_color_text_dim, 1200); }

/** 格式化行：[标题 / 副标题 ... 红描边按钮]（稿 6285:12401）。 */
lv_obj_t *make_format_row(lv_obj_t *parent, const char *title, const char *sub,
                          lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    apply_card_style(row);
    lv_obj_set_style_bg_color(row, user_light_card, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, user_font_title(), 0);
    lv_obj_set_style_text_color(t, user_light_text, 0);

    lv_obj_t *s = lv_label_create(col);
    lv_label_set_text(s, sub);
    lv_obj_set_style_text_font(s, user_font_text(), 0);
    lv_obj_set_style_text_color(s, user_light_text, 0);
    lv_obj_set_style_text_opa(s, LV_OPA_50, 0);

    /* 稿 image5：红描边白底（image6 那版深色实心是设计删改时的残留，以 image5 为准） */
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, user_color_status_err, 0);
    lv_obj_set_style_bg_color(btn, user_color_status_err, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 26, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_ver(btn, USER_PAD_SM / 2, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Format");
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_set_style_text_color(bl, user_color_status_err, 0);
    lv_obj_center(bl);
    return btn;
}

/** 存储管理详情页：存储概览 + 格式化 + 文件管理 三段。 */
void build_storage_detail(lv_obj_t *parent)
{
    g_vol_cards[0] = VolCard{};
    g_vol_cards[1] = VolCard{};

    /* ---- Storage Overview ---- */
    lv_obj_t *s1 = lv_obj_create(parent);
    lv_obj_remove_style_all(s1);
    lv_obj_set_width(s1, LV_PCT(100));
    lv_obj_set_height(s1, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s1, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(s1, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s1, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(s1, "Storage Overview");

    lv_obj_t *cards = lv_obj_create(s1);
    lv_obj_remove_style_all(cards);
    lv_obj_set_width(cards, LV_PCT(100));
    lv_obj_set_height(cards, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cards, USER_PAD_SM, 0);   /* 稿 两卡间距 16 */
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cards, LV_OBJ_FLAG_SCROLLABLE);
    make_volume_card(cards, 0, "Internal Storage");
    make_volume_card(cards, 1, "SD Card");

    /* ---- Format ---- */
    lv_obj_t *s2 = lv_obj_create(parent);
    lv_obj_remove_style_all(s2);
    lv_obj_set_width(s2, LV_PCT(100));
    lv_obj_set_height(s2, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s2, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(s2, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s2, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(s2, "Format");
    make_format_row(s2, "Format internal storage",
                    "Erases all data on the device", on_fmt_internal);
    g_fmt_usb_btn = make_format_row(s2, "Format USB drive",
                                    "Erases all data on the USB drive", on_fmt_usb);
    if(!g_usb_present) {
        lv_obj_set_style_opa(g_fmt_usb_btn, LV_OPA_40, 0);
    }

    /* ---- File Management（两行只画不接，去向待定）---- */
    lv_obj_t *s3 = lv_obj_create(parent);
    lv_obj_remove_style_all(s3);
    lv_obj_set_width(s3, LV_PCT(100));
    lv_obj_set_height(s3, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s3, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(s3, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s3, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(s3, "File Management");
    make_link_row(s3, "Album", on_album_click, nullptr);
    make_link_row(s3, "Models", on_model_click, nullptr);

    /* 进页拉一次两个卷的容量（SD 卡那支后端未实现，会回 present=0） */
    ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::Internal);
    ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::SdCard);
}

void disp_reset(void)
{
    if(g_float_back) {
        lv_obj_del(g_float_back);
        g_float_back = nullptr;
    }
    g_bl_pct_lbl = nullptr;
    lv_obj_clean(g_detail);
}

void disp_show_main(void)  { disp_reset(); build_display_detail(g_detail); }
void disp_show_sleep(void) { disp_reset(); build_sleep_detail(g_detail); }

void build_placeholder_detail(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text_fmt(l, "%s\n(coming soon)", title);
    lv_obj_set_style_text_color(l, user_light_text, 0);
    lv_obj_set_style_text_opa(l, LV_OPA_50, 0);
    lv_obj_set_style_text_font(l, user_font_title(), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

void set_gate_switch(lv_obj_t *sw, bool on)
{
    if(!sw) {
        return;
    }
    g_gate_applying = true;
    if(on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }
    g_gate_applying = false;
}

void clear_print_gate_observers(void)
{
    ui::DebugMsgProxy::instance().set_settings_print_gate_observers(
        {}, {}, {}, {}, {}, {}, {}, {});
    ui::DebugMsgProxy::instance().register_plate_work_z(nullptr);
}

void apply_plate_work_z_text(float mm)
{
    if(!g_plate_work_z_ta || lv_obj_has_state(g_plate_work_z_ta, LV_STATE_FOCUSED)) {
        return;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(mm));
    lv_textarea_set_text(g_plate_work_z_ta, buf);
}

void send_plate_work_z_from_ta(void)
{
    if(!g_plate_work_z_ta) {
        return;
    }
    const char *s = lv_textarea_get_text(g_plate_work_z_ta);
    if(!s || s[0] == '\0') {
        return;
    }
    char *end = nullptr;
    const float v = std::strtof(s, &end);
    if(end == s) {
        return;
    }
    ui::DebugMsgProxy::instance().send_plate_work_z_set(v);
}

void on_plate_work_z_ready(lv_event_t * /*e*/)
{
    send_plate_work_z_from_ta();
}

void bind_print_gate_observers(void)
{
    ui::DebugMsgProxy::instance().set_settings_print_gate_observers(
        [](bool skip_on) { set_gate_switch(g_gate_outer, !skip_on); },
        [](bool detect_on) { set_gate_switch(g_gate_inner, detect_on); },
        [](bool detect_on) { set_gate_switch(g_gate_plate, detect_on); },
        [](bool detect_on) { set_gate_switch(g_gate_optical, detect_on); },
        [](bool on) { set_gate_switch(g_gate_height, on); },
        [](bool skip_on) { set_gate_switch(g_gate_o2, !skip_on); },
        [](const middleware::HeightCalibProgressPayload& p) {
            if(!g_height_calib_lbl) {
                return;
            }
            char buf[64];
            if(p.stage == 0) {
                std::snprintf(buf, sizeof(buf), "Starting %u pts", p.total_count);
            } else if(p.stage == 1) {
                std::snprintf(buf, sizeof(buf), "h=%.2f (%u/%u)",
                              static_cast<double>(p.current_height_mm),
                              p.collected_count, p.total_count);
            } else if(p.stage == 2) {
                std::snprintf(buf, sizeof(buf), "Done");
            } else {
                std::snprintf(buf, sizeof(buf), "Failed");
            }
            lv_label_set_text(g_height_calib_lbl, buf);
        },
        [](bool on) { set_gate_switch(g_gate_hold_z, on); });
    ui::DebugMsgProxy::instance().register_plate_work_z(apply_plate_work_z_text);
}

void request_print_gate_state(void)
{
    auto& proxy = ui::DebugMsgProxy::instance();
    proxy.request_skip_outer_door_state();
    proxy.request_inner_door_occlusion_state();
    proxy.request_plate_present_state();
    proxy.request_optical_path_check_state();
    proxy.request_plate_height_adj_state();
    proxy.request_skip_o2_gate_state();
    proxy.request_hold_prep_z_state();
    proxy.request_plate_work_z_state();
}

void on_print_gate_changed(lv_event_t *e)
{
    if(g_gate_applying) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target_obj(e);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    const auto gate = static_cast<PrintGate>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    auto& proxy = ui::DebugMsgProxy::instance();
    switch(gate) {
    case PrintGate::OuterDoor:    proxy.send_skip_outer_door_set(!on); break;
    case PrintGate::InnerVision:  proxy.send_inner_door_occlusion_set(on); break;
    case PrintGate::PlatePresent: proxy.send_plate_present_set(on); break;
    case PrintGate::OpticalPath:  proxy.send_optical_path_check_set(on); break;
    case PrintGate::PlateHeight:  proxy.send_plate_height_adj_set(on); break;
    case PrintGate::Oxygen:       proxy.send_skip_o2_gate_set(!on); break;
    case PrintGate::HoldPrepZ:    proxy.send_hold_prep_z_set(on); break;
    case PrintGate::TransferBox:  control_set_transfer_box_flow(on); break;
    }
}

void on_settings_height_calib_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_height_calib_start(
        -5.0f, 12.0f, 1.0f, "/userdata/visual/height_lut.txt");
    if(g_height_calib_lbl) {
        lv_label_set_text(g_height_calib_lbl, "Starting...");
    }
    ui_toast_show("Height calibration started", user_light_accent, 1500);
}

lv_obj_t *make_gate_row(lv_obj_t *card, const char *label, PrintGate gate,
                        bool last, bool checked = true)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, USER_PAD, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if(!last) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, user_light_divider, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_20, 0);
    }

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);
    lv_obj_set_style_text_color(lbl, user_light_text, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    if(checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(sw, user_light_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_print_gate_changed, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(gate)));
    return sw;
}

void build_print_detail(lv_obj_t *parent)
{
    g_gate_outer = g_gate_inner = g_gate_plate = nullptr;
    g_gate_optical = g_gate_height = g_gate_o2 = g_gate_hold_z = nullptr;
    g_gate_transfer = nullptr;
    g_height_calib_lbl = nullptr;
    g_plate_work_z_ta = nullptr;

    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    lv_obj_t *sec = lv_obj_create(parent);
    lv_obj_remove_style_all(sec);
    lv_obj_set_width(sec, LV_PCT(100));
    lv_obj_set_height(sec, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(sec, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(sec, "Print Checks");

    lv_obj_t *card = lv_obj_create(sec);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    apply_card_style(card);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    g_gate_outer = make_gate_row(card, "Outer Door Check", PrintGate::OuterDoor, false);
    g_gate_inner = make_gate_row(card, "Inner Door Vision", PrintGate::InnerVision, false);
    g_gate_plate = make_gate_row(card, "Plate Present", PrintGate::PlatePresent, false);
    g_gate_optical = make_gate_row(card, "Optical Path Check", PrintGate::OpticalPath, false);
    g_gate_height = make_gate_row(card, "Plate Height Adj", PrintGate::PlateHeight, false);
    g_gate_o2 = make_gate_row(card, "Oxygen Check", PrintGate::Oxygen, false);
    g_gate_hold_z = make_gate_row(card, "Keep Z Position", PrintGate::HoldPrepZ, false, false);
    g_gate_transfer = make_gate_row(card, "Transfer Box", PrintGate::TransferBox, true,
                                    control_transfer_box_flow_enabled());

    lv_obj_t *sec2 = lv_obj_create(parent);
    lv_obj_remove_style_all(sec2);
    lv_obj_set_width(sec2, LV_PCT(100));
    lv_obj_set_height(sec2, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sec2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec2, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(sec2, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sec2, LV_OBJ_FLAG_SCROLLABLE);
    make_caption(sec2, "Calibration");

    /* Calibration 内：测高收到约 1mm 之后的目标高度。填 3 则再向上 2mm。 */
    lv_obj_t *zcard = lv_obj_create(sec2);
    lv_obj_remove_style_all(zcard);
    lv_obj_set_width(zcard, LV_PCT(100));
    lv_obj_set_height(zcard, LV_SIZE_CONTENT);
    apply_card_style(zcard);
    lv_obj_set_flex_flow(zcard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zcard, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(zcard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *zlabel = lv_label_create(zcard);
    lv_label_set_text(zlabel, "Work Height (mm)");
    lv_obj_set_style_text_font(zlabel, user_font_title(), 0);
    lv_obj_set_style_text_color(zlabel, user_light_text, 0);
    lv_obj_set_flex_grow(zlabel, 1);

    g_plate_work_z_ta = lv_textarea_create(zcard);
    lv_textarea_set_one_line(g_plate_work_z_ta, true);
    lv_textarea_set_accepted_chars(g_plate_work_z_ta, "0123456789.-");
    lv_textarea_set_text(g_plate_work_z_ta, "1");
    lv_obj_set_width(g_plate_work_z_ta, 120);
    lv_obj_set_height(g_plate_work_z_ta, UI_INPUT_H);
    lv_obj_set_style_text_font(g_plate_work_z_ta, user_font_text(), 0);
    lv_obj_set_style_text_align(g_plate_work_z_ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(g_plate_work_z_ta, USER_LIGHT_RADIUS * 7 / 12, 0);
    lv_obj_set_style_bg_color(g_plate_work_z_ta, user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(g_plate_work_z_ta, 13, 0);
    lv_obj_set_style_border_width(g_plate_work_z_ta, 0, 0);
    lv_obj_set_style_text_color(g_plate_work_z_ta, user_light_text, 0);
    ui_keyboard_attach(g_plate_work_z_ta, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(g_plate_work_z_ta, on_plate_work_z_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(g_plate_work_z_ta, on_plate_work_z_ready, LV_EVENT_DEFOCUSED, nullptr);

    lv_obj_t *cal = lv_obj_create(sec2);
    lv_obj_remove_style_all(cal);
    lv_obj_set_width(cal, LV_PCT(100));
    lv_obj_set_height(cal, LV_SIZE_CONTENT);
    apply_card_style(cal);
    lv_obj_set_flex_flow(cal, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cal, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *info = lv_obj_create(cal);
    lv_obj_remove_style_all(info);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(info);
    lv_label_set_text(title, "Height Calibration");
    lv_obj_set_style_text_font(title, user_font_title(), 0);
    lv_obj_set_style_text_color(title, user_light_text, 0);
    g_height_calib_lbl = lv_label_create(info);
    lv_label_set_text(g_height_calib_lbl, "Idle");
    lv_obj_set_style_text_font(g_height_calib_lbl, user_font_caption(), 0);
    lv_obj_set_style_text_color(g_height_calib_lbl, user_light_text, 0);
    lv_obj_set_style_text_opa(g_height_calib_lbl, LV_OPA_50, 0);

    lv_obj_t *btn = lv_button_create(cal);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, USER_BTN_PRIMARY_H);
    lv_obj_set_style_bg_color(btn, user_light_accent, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_add_event_cb(btn, on_settings_height_calib_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Start");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, user_font_text(), 0);
    lv_obj_center(bl);

    bind_print_gate_observers();
    request_print_gate_state();
}

void show_detail(NavId id)
{
    if(!g_detail) {
        return;
    }
    /* 清掉上一次的浮动返回钮（只有熄屏选择子页会创建） */
    if(g_float_back) {
        lv_obj_del(g_float_back);
        g_float_back = nullptr;
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
    g_bl_pct_lbl = nullptr;
    g_gate_outer = g_gate_inner = g_gate_plate = nullptr;
    g_gate_optical = g_gate_height = g_gate_o2 = g_gate_hold_z = nullptr;
    g_gate_transfer = nullptr;
    g_height_calib_lbl = nullptr;
    g_plate_work_z_ta = nullptr;
    clear_print_gate_observers();
    mcu_ver_stop_timer();
    lv_obj_clean(g_detail);
    /* 复位详情容器：默认不滚动（网络详情会按需开启）。
     * 内距不动——那是右栏的稿值 72/64，由 page_user_settings_create 一次设好。 */
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);

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
    case NAV_DISPLAY: build_display_detail(g_detail); break;
    case NAV_STORAGE: build_storage_detail(g_detail); break;
    case NAV_PRINT:   build_print_detail(g_detail); break;
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
        /* 图标与文字的明暗由 LV_STATE_CHECKED 的 image_opa / text_opa 管，
         * 见 make_nav_item，这里不用再逐个着色。
         * 右箭头只在选中项露面（Debug 那枚是外链箭头、常驻，见 make_nav_item）。 */
        if(g_nav_arrows[i] && i != NAV_DEBUG) {
            if(active) {
                lv_obj_remove_flag(g_nav_arrows[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_nav_arrows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void apply_dev_entry_visibility(void)
{
    if(!g_nav_btns[NAV_DEBUG]) {
        return;
    }
    if(g_dev_unlocked) {
        lv_obj_remove_flag(g_nav_btns[NAV_DEBUG], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_nav_btns[NAV_DEBUG], LV_OBJ_FLAG_HIDDEN);
    }
}

void on_nav_click(lv_event_t *e)
{
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    /* Debug 仅解锁后可见；点进去不再要第二次密码。 */
    if(id == NAV_DEBUG) {
        if(g_dev_unlocked) {
            ui::PageManager::instance().switch_to("user_debug");
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
    lv_obj_set_height(btn, USER_NAV_ITEM_H);   /* 稿 80 = 24 + 32 图标 + 24 */
    /* 稿 6089:11402：**未选中无底、圆角 8**；选中才是白卡（圆角 16 + 1px 白 10%
     * 描边 + 0 2 6 rgba(0,0,0,.04) 投影）。两态圆角不同，所以不能只切 bg_opa。 */
    lv_obj_set_style_radius(btn, USER_BTN_RADIUS, 0);
    lv_obj_set_style_radius(btn, USER_HIST_CARD_RADIUS, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_light_card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, 26, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(btn, 26, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(btn, 6, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_ofs_y(btn, 2, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(btn, 10, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 选中与否只差不透明度（同历史页/消息页 Tab 的处理）：稿上是字重差，
     * Montserrat 单字重追不了，只落地这一半。 */
    lv_obj_set_style_text_color(btn, user_light_text, 0);
    lv_obj_set_style_text_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_text_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn, on_nav_click, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));

    /* 稿 3361:18469 是 32 见方。旧式取行高的 ~5/9（A8 图形本身只占位图约 75%，
     * 不宜直接用文字行高，否则视觉过小）——行高改成稿的 80 之后那个比例给出 44、
     * 比稿大一圈，所以 1280 档直接用稿值，其余档位仍走旧式。 */
    const int32_t icon_sz = (USER_SETTINGS_NAV_W > 0) ? 32
                                                      : USER_NAV_ITEM_H * 5 / 9;
    lv_obj_t *icon = lv_image_create(btn);
    lv_image_set_src(icon, d.icon);
    lv_obj_set_size(icon, icon_sz, icon_sz);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_image_recolor(icon, user_light_text, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    /* 图标跟着文字一起明暗：未选中压到 50%（同 text_opa） */
    lv_obj_set_style_image_opa(icon, LV_OPA_50, 0);
    lv_obj_set_style_image_opa(icon, LV_OPA_COVER, LV_STATE_CHECKED);
    g_nav_icons[id] = icon;

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, d.label);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);   /* 稿「一级标题」20 */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(lbl, 1);

    /* **只有选中项才出右箭头**（稿 6089:11402；未选中那几项右侧是空的）。
     * Debug 是例外：它不进右栏、是跳出去开发者页，稿上给的是外链箭头 ↗，故常驻。
     * LVGL 符号字库里没有斜向箭头，退而用同向的 ›——语义（有去处）保住了。 */
    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_text(), 0);
    lv_obj_set_style_text_color(arrow, user_light_text, 0);
    if(id != NAV_DEBUG) {
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_opa(arrow, LV_OPA_50, 0);
    }
    g_nav_arrows[id] = arrow;

    g_nav_btns[id] = btn;
    return btn;
}

/**
 * 左栏：一条满高的渐变底带 + 里面 300 宽的导航列。
 *
 * 稿 6240:92194 给底带的是**竖向渐变** `#FAFAFA → #F4F4F4(50%) → #FAFAFA`，
 * 内距 48/32/0，总宽 364 = 32 + 300 + 32（整屏 x 128..492，右缘正是右栏的起点）。
 * LVGL 的 bg_grad 只有两个色标，中间那档做不出来，所以拆成上下两半各一段渐变：
 * 上半 FAFAFA→F4F4F4、下半 F4F4F4→FAFAFA，接缝在中点、与稿的 50% 色标重合。
 */
lv_obj_t *build_subnav(lv_obj_t *parent)
{
    const bool spec = (USER_SETTINGS_NAV_W > 0);   /* 有稿的档位（1280） */

    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_remove_style_all(band);
    lv_obj_set_width(band, LV_SIZE_CONTENT);
    lv_obj_set_height(band, LV_PCT(100));
    lv_obj_set_style_pad_hor(band, spec ? USER_PAD_LG : USER_PAD, 0);   /* 稿 32 */
    lv_obj_set_style_pad_top(band, USER_PAD * 2, 0);                    /* 稿 48 */
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    if(spec) {
        lv_obj_t *half_top = lv_obj_create(band);
        lv_obj_remove_style_all(half_top);
        lv_obj_add_flag(half_top, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(half_top, LV_PCT(100), LV_PCT(50));
        lv_obj_align(half_top, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(half_top, user_light_bg, 0);
        lv_obj_set_style_bg_grad_color(half_top, user_light_rail_mid, 0);
        lv_obj_set_style_bg_grad_dir(half_top, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(half_top, LV_OPA_COVER, 0);
        lv_obj_remove_flag(half_top, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(half_top);

        lv_obj_t *half_bot = lv_obj_create(band);
        lv_obj_remove_style_all(half_bot);
        lv_obj_add_flag(half_bot, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(half_bot, LV_PCT(100), LV_PCT(50));
        lv_obj_align(half_bot, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(half_bot, user_light_rail_mid, 0);
        lv_obj_set_style_bg_grad_color(half_bot, user_light_bg, 0);
        lv_obj_set_style_bg_grad_dir(half_bot, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(half_bot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(half_bot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(half_bot);
    }

    lv_obj_t *nav = lv_obj_create(band);
    lv_obj_remove_style_all(nav);
    /* 稿 300 定宽；该字段为 0 的档位沿用旧式百分比。 */
    if(spec) lv_obj_set_width(nav, USER_SETTINGS_NAV_W);
    else     lv_obj_set_width(nav, LV_PCT(34));
    lv_obj_set_height(nav, LV_PCT(100));
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nav, USER_PAD_SM, 0);   /* 稿 16 */
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(nav, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nav, LV_SCROLLBAR_MODE_OFF);

    for(int i = 0; i < NAV_COUNT; i++) {
        make_nav_item(nav, static_cast<NavId>(i));
    }
    apply_dev_entry_visibility();
    return band;
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
    } else if(g_selected == NAV_PRINT && g_gate_outer) {
        bind_print_gate_observers();
        request_print_gate_state();
        set_gate_switch(g_gate_transfer, control_transfer_box_flow_enabled());
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
    g_float_back = nullptr;
    g_mcu_ver_lbl = nullptr;
    g_gate_outer = g_gate_inner = g_gate_plate = nullptr;
    g_gate_optical = g_gate_height = g_gate_o2 = g_gate_hold_z = nullptr;
    g_gate_transfer = nullptr;
    g_height_calib_lbl = nullptr;
    g_plate_work_z_ta = nullptr;
    clear_print_gate_observers();
    for(int i = 0; i < NAV_COUNT; i++) {
        g_nav_btns[i]  = nullptr;
        g_nav_icons[i] = nullptr;
        g_nav_arrows[i] = nullptr;
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

    /* 存储容量回执：刷新存储页那两张卡（只在该页停留时才有对象可刷）。 */
    ui::UiMsgProxy::instance().register_storage_usage(
        [](const middleware::StorageUsagePayload &p) {
            if(g_selected == NAV_STORAGE) {
                storage_apply_usage(p);
            }
        });

    /* U 盘热插拔：决定「格式化 U 盘」那枚钮能不能点。这条后端**已实现**。
     * 与文件页共用 add_usb_hotplug_listener，勿用单槽 register，否则后注册的会顶掉这边。 */
    ui::UiMsgProxy::instance().add_usb_hotplug_listener(
        [](const middleware::UsbHotplugPayload &p) {
            g_usb_present = (p.present != 0 && p.mounted != 0);
            if(g_fmt_usb_btn) {
                lv_obj_set_style_opa(g_fmt_usb_btn,
                                     g_usb_present ? LV_OPA_COVER : LV_OPA_40, 0);
            }
        });

    /* 格式化进度 / 终态：后端**未实现**，这两个回调目前不会被触发；先接好线，
     * 后端一上来就能用。见 doc/STORAGE_BACKEND_API.md。 */
    ui::UiMsgProxy::instance().register_storage_format_progress(
        [](const middleware::StorageFormatProgressPayload &p) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Formatting... %u%%",
                          static_cast<unsigned>(p.percent));
            ui_toast_show(buf, user_color_accent, 1000);
        });
    ui::UiMsgProxy::instance().register_storage_format_result(
        [](const middleware::StorageFormatRspPayload &p) {
            if(p.result == 0) {
                ui_toast_show("Format complete", user_color_status_ok, 2000);
                if(g_selected == NAV_STORAGE) {
                    ui::UiMsgProxy::instance().send_storage_usage(
                        static_cast<middleware::StorageVolume>(p.volume));
                }
            } else {
                ui_toast_show("Format failed", ui_color_danger, 2500);
            }
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
    /* 稿 6089:11322 的两栏在内容区 1152 里正好闭合，**内距全落在各自栏里**、
     * body 本身零内距：
     *   左栏渐变带 364（= 32 + 300 + 32，见 build_subnav）
     *   右栏 788（= 64 + 660 + 64），起点 x=492 正是渐变带的右缘
     * 旧版那套「body 左内距 36 + 列间距 36 + 右留白 176」是给悬浮翻页钮让位的，
     * 钮已按新稿去掉，这三个量一并收掉。 */
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    build_subnav(body);

    g_detail = lv_obj_create(body);
    lv_obj_remove_style_all(g_detail);
    lv_obj_set_height(g_detail, LV_PCT(100));
    lv_obj_set_flex_grow(g_detail, 1);
    lv_obj_set_flex_flow(g_detail, LV_FLEX_FLOW_COLUMN);
    /* 稿 6285:12390 的右栏内距 72/64/0，分区之间 48（组内标题↔内容才是 16）。 */
    const bool spec = (USER_SETTINGS_NAV_W > 0);
    lv_obj_set_style_pad_top(g_detail, spec ? USER_PAD * 3 : USER_PAD, 0);
    lv_obj_set_style_pad_hor(g_detail, spec ? USER_PAD_LG * 2 : USER_PAD, 0);
    lv_obj_set_style_pad_bottom(g_detail, USER_PAD, 0);
    lv_obj_set_style_pad_row(g_detail, spec ? USER_PAD * 2 : USER_PAD, 0);
    lv_obj_set_style_bg_opa(g_detail, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);

    update_nav_highlight();
    show_detail(NAV_NETWORK);

    return page;
}
