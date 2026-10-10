#include "page_user_settings.hpp"
#include "ui_theme.hpp"
#include "ui_backlight.hpp"
#include "ui_user_widgets.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "ui_arc_spinner.hpp"
#include "ui_prefs.hpp"
#include "page_manager.hpp"
#include "settings_icons.h"
#include "ui_msg_proxy.hpp"
#include "debug_msg_proxy.hpp"
#include "control.hpp"
#include "config/cal_config.hpp"
#include "xfr_paths.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

/**
 * @file page_user_settings.cpp
 * @brief 设置页（两栏：左侧分类导航 + 右侧详情），对齐稿 6403 / 6457 / 6655 一族
 *        （tools/pages/setting/neew/image1..18）。
 *
 * 布局（本页不显示外壳的常驻设备条，内容从屏顶起，见 PageManager::apply_device_bar）：
 *   ┌────────────────────────┬──────────────────────────────────┐
 *   │ [] Device Settings     │  Device Settings                 │
 *   │ [] Preferences         │  Account, network and display    │
 *   │ [] Nitrogen Generator  │  ┌────────────────────────────┐  │
 *   │ [] Storage             │  │ Account                  > │  │
 *   │ [] Privacy & Security  │  │ Wi-Fi           SSID     > │  │
 *   │ [] Advanced Print      │  │ Display & Language       > │  │
 *   │ [] About               │  └────────────────────────────┘  │
 *   │ [] Debug（解锁后）  >  │                                  │
 *   └────────────────────────┴──────────────────────────────────┘
 *
 * 右栏每页都是「标题 28 + 副标题 + 行卡片」，子页标题前多一枚返回箭头。页面树、
 * 只做界面的项、偏好落盘见 app/ui/UI.md「设置页布局」。要点：
 *   - Device Settings 下挂 Account（Coming soon）/ Wi-Fi / Display & Language 三个子页，
 *     子页状态机 DevSub。Wi-Fi 列表来自后端扫描推送（GLAVO_Wifi_Scan_Rsp），首次结果到达前
 *     显示 Searching；停在 Wi-Fi 子页时周期重扫。
 *   - Preferences / Privacy 的开关落盘（ui_prefs）；只有「完成后自动吹粉」与「自动录像」生效。
 *   - Debug 行与 About 里的 Galvo/MCU OTA 默认隐藏。About 里连点 Firmware Version 那一行
 *     三次 → 开发者密码框（"888"）→ 本次运行内显示这两处；再点 Debug 进调试页。
 *   - Advanced Print（稿外保留）：打印前门控，默认开，与调试页同一套 Glavo 开关；
 *     Transfer Box 与调试页同一枚本地开关，默认开（取件后走转运盒三步，自检阶段终止的除外）；
 *     Auto Print After Transfer 默认关（落盘，Xfr 读）：关则切片软件只下发、print/start 回 idle；
 *     Calibration 内的 Work Height 是测高收到约 1mm 后再向上补的目标高度；
 *     Height Calibration 的 Start 与调试页 Laser 卡相同。
 *   - 软件版本 UPC_SW_VERSION：根 CMakeLists.txt 的 UPC_PRODUCT_VERSION。
 */

#ifndef UPC_SW_VERSION
#error "UPC_SW_VERSION not defined — set UPC_PRODUCT_VERSION in root CMakeLists.txt"
#endif

namespace {

constexpr const char *kDevPassword = "888";

/* ---- 右栏几何 ----
 * 稿是 1280 档，下面按稿值用 USER_PAD 系列写，其余分辨率档位随主题缩放。
 * 纵向节奏（屏上 y；本页不显示设备条，内容从屏顶起）：
 *   标题行盒顶 56（字身中心 ≈71）→ 副标题 102 → 首张卡 160；
 *   卡底 → 下一分区标题 48，分区标题 → 卡 20。
 * 卡片一律 660 宽（右栏 788 − 左右内距 64×2）、圆角 16、纯白无描边无投影；
 * 卡内行间 1px 分隔线 @8%，左右各缩进 24。
 * 例外是熄屏时间那张选项卡（稿 6655:18619）：带淡投影、分隔线通栏 #E5E7EB，见 build_sleep_page。 */
#define SET_HEAD_TOP     (USER_PAD_LG + USER_PAD)        /**< 56：标题行盒顶 */
#define SET_HEAD_GAP     (USER_PAD * 3 / 2)              /**< 36：页头底 → 内容 */
#define SET_SECTION_GAP  (USER_PAD * 2)                  /**< 48：分区之间 */
#define SET_CAPTION_GAP  (USER_PAD_SM * 5 / 4)           /**< 20：分区标题 → 卡；弹窗正文行距 */
#define SET_ROW_SUB_H    (USER_PAD * 4)                  /**< 96：标题 + 副标题行 */
#define SET_ROW_H        (USER_PAD_LG * 5 / 2)           /**< 80：单行 */
#define SET_ROW_LIST_H   (SET_ROW_H - USER_PAD_SM / 4)   /**< 76：Wi-Fi 列表 / 网络详情 / 熄屏入口行 */
#define SET_ROW_OPT_H    (SET_ROW_H - USER_PAD_SM * 3 / 4) /**< 68：熄屏时间选项行 */
#define SET_ICON_SZ      USER_PAD                        /**< 24：行内图标盒（箭头 / Wi-Fi / 太阳） */
#define SET_SW_W         (USER_PAD * 3)                  /**< 72：开关宽 */
#define SET_SW_H         (USER_PAD_SM * 5 / 2)           /**< 40：开关高（圆钮 32，内缩 4） */
#define SET_SMALL_BTN_H  (USER_PAD * 2 - 1)              /**< 47：行尾描边小钮 */
#define SET_OPT_H        (USER_PAD * 4 + 2)              /**< 98：选择弹窗的选项块 */
#define SET_RADIO_SZ     (USER_PAD_SM * 5 / 2)           /**< 40：选项右侧圆钮 */
/* 淡投影（左栏选中项、熄屏时间选项卡）：稿 feOffset dy=2、stdDeviation=3、黑 4%。
 * LVGL 的 shadow_width 是边缘内外合计的渐变宽，按亮度圆钮那处的换算（σ5 → 12）取 8。 */
#define SET_SHADOW_W     (USER_PAD_SM / 2)               /**< 8 */
#define SET_SHADOW_Y     (USER_PAD_SM / 8)               /**< 2 */

/* 文字透明度（墨色 #333145 叠在白卡上）：副标题 55%，值 / 页副标题 / 分区标题 65% */
constexpr lv_opa_t kOpaSub   = 140;
constexpr lv_opa_t kOpaValue = 166;

/* ---- 子导航定义 ---- */
/**
 * 子导航项，从上到下。偏好设置排在制氮机之前：稿上 10 帧都是这个顺序（含 Device Settings
 * 主页 6403:1722），只有 6448:54075 / 6457:17430 两帧反过来——左栏几何取 6457:17430，顺序
 * 不随它。后接稿外的两项：Advanced Print（保留的打印前门控，排在 About 之前）与 Debug
 * （解锁后才出现，排最后）。
 */
enum NavId {
    NAV_DEVICE = 0,
    NAV_PREFS,
    NAV_NITROGEN,
    NAV_STORAGE,
    NAV_PRIVACY,
    NAV_PRINT,
    NAV_ABOUT,
    NAV_DEBUG,
    NAV_COUNT,
};

struct NavDef {
    const lv_image_dsc_t *icon;     /**< A8 图标（settings_icons.*，运行时 recolor） */
    const char           *label;    /**< 左栏标签，也是右栏一级页的标题 */
    const char           *subtitle; /**< 右栏一级页的副标题 */
};

const NavDef kNavDefs[NAV_COUNT] = {
    { &settings_icon_device,    "Device Settings",    "Account, network and display"                },
    { &settings_icon_prefs,     "Preferences",        "Default behavior for printing and recording" },
    { &settings_icon_nitrogen,  "Nitrogen Generator", "Connect the nitrogen generator"              },
    { &settings_icon_storage,   "Storage",            "Manage internal and external storage"        },
    { &settings_icon_privacy,   "Privacy & Security", "Terms, privacy and experience program"       },
    { &settings_icon_adv_print, "Advanced Print",     "Print gates and calibration"                 },
    { &settings_icon_about,     "About",              "Device info and maintenance"                 },
    { &settings_icon_debug,     "Debug",              nullptr                                       },
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

/** 设备序列号：设备 key 文件第一行，与 MQTT 的 device_id 同源（app/mqtt/lib/mqtt_config.cpp），
 *  环境变量 SNOWPOD_DEVICE_KEY_FILE 可改路径；读不到显示 "--"。 */
const char *about_serial_text()
{
    static bool loaded = false;
    static std::string sn = "--";
    if(!loaded) {
        loaded = true;
        const char *env = std::getenv("SNOWPOD_DEVICE_KEY_FILE");
        std::ifstream in((env && env[0]) ? env : "/userdata/snowpod/key");
        std::string line;
        if(in && std::getline(in, line)) {
            /* 同 mqtt_config：去首尾空白，再去一层外引号 */
            const size_t b = line.find_first_not_of(" \t\r\n");
            const size_t e = line.find_last_not_of(" \t\r\n");
            if(b != std::string::npos) {
                line = line.substr(b, e - b + 1);
                if(line.size() >= 2 && line.front() == '"' && line.back() == '"') {
                    line = line.substr(1, line.size() - 2);
                }
                if(!line.empty()) {
                    sn = line;
                }
            }
        }
    }
    return sn.c_str();
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

/* Wi-Fi 详情页的 IP 值标签：状态推送到达时就地改文字（该子页是静态构建的，不重建就
 * 永远显示进页那一刻的 IP）。离开子页时置空。 */
lv_obj_t *g_ip_val_lbl = nullptr;

/* 停在 Wi-Fi 子页时的周期重扫（进页只扫一次的话，附近网络变化要等用户来回切页才可见）。 */
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

/** Wi-Fi 当前是否已连上（后端没上报过时按未连）。 */
bool wifi_connected(void)
{
    return g_wifi_state_valid && g_wifi_state.connected;
}

/** Device Settings 主页 Wi-Fi 行的副标题：Off / Connected / Not connected。 */
const char *wifi_status_text(void)
{
    if(!g_wifi_on) {
        return "Off";
    }
    return wifi_connected() ? "Connected" : "Not connected";
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

lv_obj_t *g_nav_btns[NAV_COUNT] = { nullptr };
lv_obj_t *g_detail   = nullptr;   /**< 右侧详情容器（按选中重建） */
int       g_selected = NAV_DEVICE;

/* Device Settings 的子页（页内子视图，标题前带返回箭头）。切左栏时复位到 DEV_MAIN。 */
enum DevSub {
    DEV_MAIN = 0,
    DEV_ACCOUNT,
    DEV_WIFI,
    DEV_WIFI_DETAIL,   /**< 已连网络详情：断开 / 忘记 / IP（稿 6462:17768） */
    DEV_SAVED,         /**< 已保存网络（稿外保留） */
    DEV_DISPLAY,
    DEV_SLEEP,         /**< 熄屏时间选择 */
};
int g_dev_sub = DEV_MAIN;

/* 以下指针只在对应子页存活期间有效，换页时由 detail_reset() 统一置空：
 * 异步推送（Wi-Fi 状态、容量回执、插拔）都靠它们判断目标还在不在。 */
lv_obj_t *g_dev_wifi_sub    = nullptr;   /**< Device Settings 主页 Wi-Fi 行的副标题 */
lv_obj_t *g_dev_wifi_val    = nullptr;   /**< 同一行右侧的 SSID */
lv_obj_t *g_wifi_sw         = nullptr;   /**< Wi-Fi 子页的开关 */
lv_obj_t *g_wifi_box        = nullptr;   /**< Wi-Fi 子页开关卡以下那段（Wi-Fi 关时隐藏） */
lv_obj_t *g_rec_storage_val = nullptr;   /**< Preferences 页录像存储行的值 */

/* 开发者入口进程级解锁：连点 Firmware Version 三次并输对密码后置位。
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
lv_obj_t *g_gate_height = nullptr;
lv_obj_t *g_gate_o2 = nullptr;
lv_obj_t *g_gate_powder = nullptr;
lv_obj_t *g_gate_pm25 = nullptr;
lv_obj_t *g_gate_hold_z = nullptr;
lv_obj_t *g_gate_transfer = nullptr;
lv_obj_t *g_gate_auto_print = nullptr;
lv_obj_t *g_height_calib_lbl = nullptr;
lv_obj_t *g_plate_work_z_ta = nullptr;
/** 回读改开关时挡住 VALUE_CHANGED，避免再发一帧 Set。 */
bool g_gate_applying = false;

enum class PrintGate : uint8_t {
    OuterDoor = 0,
    InnerVision,
    PlateHeight,
    Oxygen,
    PowderLow,
    DoorPm,
    HoldPrepZ,
    TransferBox,
    AutoPrint,
};

void detail_reset(void);
void show_detail(NavId id);
void apply_dev_entry_visibility(void);
void dev_show(DevSub sub);
void enter_wifi_page(void);
void on_ext_presence_changed(void);
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

/* ============================================================================
 *  右栏通用件（页头 / 分区 / 列表卡行 / 开关 / 描边小钮 / 圆钮 / Coming soon）
 * ========================================================================= */

/** 后端未接的入口统一提示（只做界面的项，清单见 UI.md）。 */
void show_coming_soon(void)
{
    ui_toast_show("Coming soon", UI_TOAST_INFORM, 1500);
}

/** 透明排版容器：满宽、高随内容、不滚动、不吃点击（点击落到外层的行 / 按钮上）。 */
lv_obj_t *make_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

/** A8 图标（settings_icons.*）：sz 见方拉伸显示，按 color 着色。 */
lv_obj_t *make_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t sz, lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_size(img, sz, sz);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

/** 单行截断：LVGL 的 DOTS 只在文字高过标签时才生效，所以把高度钉成一行。 */
void label_one_line(lv_obj_t *l, const lv_font_t *font)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(font));
}

lv_obj_t *make_text(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_opa_t opa)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, user_light_text, 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

/**
 * 右栏页头 + 内容列，返回内容列（分区之间 48，页头底到内容 36）。
 *
 * 一级页（back_cb 为空，稿 6403:1722）：标题 28 + 副标题 20 @65%，都从卡片左缘起。
 * 子页（稿 6426:1914）：24 见方返回箭头 + 16 间距 + 标题；副标题与**标题文字**左对齐，
 * 不与箭头对齐。subtitle 可空（网络详情那页稿上就没有）。
 */
lv_obj_t *make_page(lv_obj_t *parent, const char *title, const char *subtitle,
                    lv_event_cb_t back_cb)
{
    lv_obj_t *head = make_box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, USER_PAD_SM, 0);

    if(back_cb) {
        /* 返回键的命中区。箭头只有 24 见方、贴在标题行左上角，光给它外扩不够：LVGL 只往
         * 命中了父对象的子对象里找（lv_indev_search_obj），外扩被 30 高的标题行裁掉，实测
         * 只剩约 40×30，箭头左边零余量。所以页头用负外边距 + 等量内距扩到右栏左上角
         * （内容位置不变；百分比宽度会减掉外边距，宽度自动补回），再在箭头后面垫一块透明
         * 热区：从右栏左缘到标题文字前、从屏顶到副标题上沿。它先建、垫在下层，点在箭头上
         * 仍由箭头接住（保留按下反馈）。挂在页头里而不是 g_detail 上，页面滚动时一起走。 */
        const int32_t pl = lv_obj_get_style_pad_left(parent, LV_PART_MAIN);
        const int32_t pt = lv_obj_get_style_pad_top(parent, LV_PART_MAIN);
        lv_obj_set_style_margin_left(head, -pl, 0);
        lv_obj_set_style_margin_top(head, -pt, 0);
        lv_obj_set_style_pad_left(head, pl, 0);
        lv_obj_set_style_pad_top(head, pt, 0);
        if(!subtitle) {
            /* 没副标题的页（网络详情）页头只到标题行底，热区下面那 16 会被页头裁掉：页头往下
             * 多伸 16，再用等量负下边距抵掉（flex 主轴计入外边距），正文位置不变 */
            lv_obj_set_style_pad_bottom(head, USER_PAD_SM, 0);
            lv_obj_set_style_margin_bottom(head, -USER_PAD_SM, 0);
        }

        lv_obj_t *hit = lv_button_create(head);
        lv_obj_remove_style_all(hit);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(hit, pl + SET_ICON_SZ + USER_PAD_SM,
                        pt + lv_font_get_line_height(user_font_dlg_title()) + USER_PAD_SM);
        lv_obj_set_pos(hit, -pl, -pt);
        lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(hit, back_cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *trow = make_box(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(trow, USER_PAD_SM, 0);
    if(back_cb) {
        lv_obj_t *back = lv_button_create(trow);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, SET_ICON_SZ, SET_ICON_SZ);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(back, user_light_text, 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(back, 26, LV_STATE_PRESSED);
        /* 外扩只在标题行范围内生效（行外的点由上面那块热区接）。点在标题文字前那 16 的
         * 缝里也算返回，箭头按下反馈照样出现。 */
        lv_obj_set_ext_click_area(back, USER_PAD_SM);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
        make_icon(back, &settings_icon_chev_left, SET_ICON_SZ, user_light_text);
    }
    lv_obj_t *t = make_text(trow, title, user_font_dlg_title(), LV_OPA_COVER);   /* 稿 28 */
    lv_obj_set_flex_grow(t, 1);
    label_one_line(t, user_font_dlg_title());

    if(subtitle) {
        lv_obj_t *s = make_text(head, subtitle, user_font_title(), kOpaValue);   /* 稿 20 */
        if(back_cb) {
            lv_obj_set_style_pad_left(s, SET_ICON_SZ + USER_PAD_SM, 0);
        }
    }

    lv_obj_t *body = make_box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, SET_SECTION_GAP, 0);
    lv_obj_set_style_margin_top(body, SET_HEAD_GAP, 0);
    return body;
}

/** 分区标题（稿「语言」「其他网络」「内置存储」那一类）：20 @65%。 */
lv_obj_t *make_caption(lv_obj_t *parent, const char *text)
{
    return make_text(parent, text, user_font_title(), kOpaValue);
}

/** 分区：标题 + 调用方往里追加的卡片（标题 → 卡 20）。caption 为空时只是个竖排容器。 */
lv_obj_t *make_section(lv_obj_t *body, const char *caption)
{
    lv_obj_t *sec = make_box(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec, SET_CAPTION_GAP, 0);
    if(caption) {
        make_caption(sec, caption);
    }
    return sec;
}

/** 列表白卡：满宽、圆角 16、零内距（内距落在每一行上，分隔线才能按稿缩进 24）。 */
lv_obj_t *make_list_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(card, user_light_card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    /* 首末行按下变灰时不越出圆角 */
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

/** 淡投影，sel 为样式选择器（左栏选中项给 LV_STATE_CHECKED）。投影画在对象外沿，
 *  父对象得留出余量，否则会被父对象的范围裁掉（LVGL 按父对象范围裁子对象）。 */
void add_soft_shadow(lv_obj_t *obj, lv_style_selector_t sel)
{
    lv_obj_set_style_shadow_width(obj, SET_SHADOW_W, sel);
    lv_obj_set_style_shadow_offset_y(obj, SET_SHADOW_Y, sel);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), sel);
    lv_obj_set_style_shadow_opa(obj, 10, sel);
}

/* 卡上打这个标，make_row_base 补的分隔线就改成通栏 #E5E7EB（熄屏时间那张卡，稿 6655:18619）。 */
constexpr lv_obj_flag_t kCardFlushDivider = LV_OBJ_FLAG_USER_1;

/** 卡内分隔线：1px 墨色 @8%，左右各缩进 24；卡上有 kCardFlushDivider 时通栏 #E5E7EB。 */
void add_divider(lv_obj_t *card)
{
    const bool flush = lv_obj_has_flag(card, kCardFlushDivider);
    lv_obj_t *wrap = make_box(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(wrap, flush ? 0 : USER_PAD, 0);
    lv_obj_t *ln = lv_obj_create(wrap);
    lv_obj_remove_style_all(ln);
    lv_obj_set_size(ln, LV_PCT(100), 1);
    if(flush) {
        lv_obj_set_style_bg_color(ln, lv_color_hex(0xE5E7EB), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(ln, user_light_text, 0);
        lv_obj_set_style_bg_opa(ln, 20, 0);
    }
    lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
}

/** 行骨架：横排、行高 h、左右内距 24。卡里已有行时先补一道分隔线。
 *  cb 非空时整行可点、按下变灰；否则不吃点击（拖动照常滚动右栏）。 */
lv_obj_t *make_row_base(lv_obj_t *card, int32_t h, lv_event_cb_t cb, void *ud)
{
    if(lv_obj_get_child_count(card) > 0) {
        add_divider(card);
    }
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_set_style_pad_hor(row, USER_PAD, 0);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_set_style_bg_color(row, user_light_more_bg, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

/**
 * 列表卡里的一行：[标题 22 / 副标题 18 @55% ... 右侧槽]，文字列在行内竖直居中。
 *
 * h 是行高（稿：带副标题 96，单行 80 / 76）。右侧槽由调用方往返回的行里追加
 * （row_add_value / row_add_chevron / make_pref_switch / make_outline_btn）。
 */
lv_obj_t *make_list_row(lv_obj_t *card, const char *title, const char *subtitle,
                        int32_t h, lv_event_cb_t cb = nullptr, void *ud = nullptr)
{
    lv_obj_t *row = make_row_base(card, h, cb, ud);

    lv_obj_t *col = make_box(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_row(col, USER_PAD_SM * 3 / 4, 0);   /* 稿 12 */

    lv_obj_t *t = make_text(col, title, user_font_dlg_btn(), LV_OPA_COVER);   /* 稿 22 */
    lv_obj_set_width(t, LV_PCT(100));
    label_one_line(t, user_font_dlg_btn());

    if(subtitle) {
        lv_obj_t *s = make_text(col, subtitle, user_font_text(), kOpaSub);   /* 稿 18 */
        lv_obj_set_width(s, LV_PCT(100));
        label_one_line(s, user_font_text());
    }
    return row;
}

/** make_list_row 建出的行里的标题 / 副标题标签（没有副标题时后者为空）。 */
lv_obj_t *list_row_title(lv_obj_t *row) { return lv_obj_get_child(lv_obj_get_child(row, 0), 0); }
lv_obj_t *list_row_sub(lv_obj_t *row)   { return lv_obj_get_child(lv_obj_get_child(row, 0), 1); }

/** 行右侧的值文字（18 @65%）。过长时截断，最宽 10 个图标盒，免得把标题挤没。 */
lv_obj_t *row_add_value(lv_obj_t *row, const char *text)
{
    lv_obj_t *v = make_text(row, text, user_font_text(), kOpaValue);
    lv_obj_set_style_max_width(v, SET_ICON_SZ * 10, 0);
    label_one_line(v, user_font_text());
    return v;
}

/** 行尾右箭头（稿 6×12、2px、#9494A2 ≈ 墨色 @50%）。稿上箭头中心离行内缘还有 4。 */
void row_add_chevron(lv_obj_t *row)
{
    lv_obj_t *c = make_icon(row, &settings_icon_chev_right, SET_ICON_SZ, user_light_text);
    lv_obj_set_style_image_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_margin_right(c, USER_PAD_SM / 4, 0);
}

/** 开关热区被点：代用户切换开关，再补发 VALUE_CHANGED——各开关的回调都从事件目标读状态，
 *  下发 / 落盘 / 提示照常走。LVGL 的开关在 CHECKED 变化时自己走圆钮动画。 */
void on_switch_zone_click(lv_event_t *e)
{
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    if(lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

/**
 * 开关（稿 72×40、圆钮 32 内缩 4；开 = 墨色实心，关 = 墨色 @8%）。Advanced Print 共用。
 * parent 须是 make_row_base 建的行、开关排在行尾；行右侧另垫一块透明热区（见函数内）。
 */
lv_obj_t *make_pref_switch(lv_obj_t *parent, bool on, lv_event_cb_t cb, void *ud)
{
    /* 热区：开关本身只有 72×40（外扩 8 后 88×56），行高 76~96，开关上下和右边到卡边都点不到。
     * 行右侧垫一块满行高、宽两个开关的透明按钮（开关左边让出 48 + 开关 72 + 行右内距 24），
     * 点到就代为切换。它先于开关建、垫在下层，点在开关上仍由开关接住；左边文字区不响应，
     * 免得点标题误关门控。行零上下内距，浮动子节点的满高就是行高；对齐时偏移行右内距，贴到行右缘。 */
    lv_obj_t *zone = lv_button_create(parent);
    lv_obj_remove_style_all(zone);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(zone, SET_SW_W * 2, LV_PCT(100));
    lv_obj_align(zone, LV_ALIGN_RIGHT_MID, lv_obj_get_style_pad_right(parent, LV_PART_MAIN), 0);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, SET_SW_W, SET_SW_H);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, user_light_text, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, 20, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, user_light_text, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, user_light_card, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    /* 圆钮按轨道高画，负内距把它往里收 4 */
    lv_obj_set_style_pad_all(sw, -(USER_PAD_SM / 4), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, USER_PAD_SM / 2);
    if(on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    if(cb) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
    }
    lv_obj_add_event_cb(zone, on_switch_zone_click, LV_EVENT_CLICKED, sw);
    return sw;
}

/**
 * 行尾描边小钮，宽随文字（左右内距 20）、高 47、圆角 10。
 * danger：稿 6443:52816 的 Format / 6448:53532 的 Restore，红 #FF4242 描边配红字；
 * 否则墨色 10% 描边配墨色字（Start / Add Hidden Network 这类）。
 */
lv_obj_t *make_outline_btn(lv_obj_t *parent, const char *text, bool danger,
                           lv_event_cb_t cb, void *ud = nullptr)
{
    const lv_color_t c = danger ? user_color_status_err : user_light_text;
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, SET_SMALL_BTN_H);
    lv_obj_set_style_radius(btn, USER_PAD_SM * 5 / 8, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, c, 0);
    lv_obj_set_style_border_opa(btn, danger ? LV_OPA_COVER : 26, 0);
    lv_obj_set_style_bg_color(btn, c, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, 26, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, SET_CAPTION_GAP, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, user_font_text(), 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_center(l);
    return btn;
}

/** 单选圆钮（稿 6438:52418）：40 见方；选中 = 墨色实心 + 白勾，未选中 = 1px 墨色 @50% 圈。 */
lv_obj_t *make_radio(lv_obj_t *parent, lv_obj_t **out_check)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, SET_RADIO_SZ, SET_RADIO_SZ);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(r, user_light_text, 0);
    lv_obj_set_style_border_color(r, user_light_text, 0);
    lv_obj_set_style_border_opa(r, LV_OPA_50, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ck = make_icon(r, &settings_icon_check, SET_ICON_SZ, lv_color_white());
    lv_obj_center(ck);
    *out_check = ck;
    return r;
}

void radio_paint(lv_obj_t *r, lv_obj_t *ck, bool on)
{
    lv_obj_set_style_bg_opa(r, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, on ? 0 : 1, 0);
    if(on) {
        lv_obj_remove_flag(ck, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ck, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 勾选列表选中行的行尾标记（稿 6655:18619）：24 墨色实心圆 + 白色小勾；未选中的行不画。 */
lv_obj_t *make_check_dot(lv_obj_t *parent)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, SET_ICON_SZ, SET_ICON_SZ);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, user_light_text, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(make_icon(dot, &settings_icon_check_sm, SET_ICON_SZ, lv_color_white()));
    return dot;
}

/** 稿 6448:54075：页头下方整块留白，「Coming soon...」在右栏里居中（20 @50%）。
 *  浮在 g_detail 上按整屏高居中，所以与页头多高无关。 */
void make_coming_soon(lv_obj_t *detail)
{
    lv_obj_t *l = make_text(detail, "Coming soon...", user_font_title(), LV_OPA_50);
    lv_obj_add_flag(l, LV_OBJ_FLAG_FLOATING);
    /* align 以内容区为参照，而内容区上下内距不等（56 / 24），补回一半落到整屏中线 */
    lv_obj_align(l, LV_ALIGN_CENTER, 0,
                 (lv_obj_get_style_pad_bottom(detail, LV_PART_MAIN) -
                  lv_obj_get_style_pad_top(detail, LV_PART_MAIN)) / 2);
}

/* ============================================================================
 *  弹窗（稿 6423:51743 密码 / 6437:51952 隐藏网络 / 6438:52418 录像存储 /
 *  6438:52611 无存储设备 / 6444:53244 格式化确认 / 6448:53673 导出日志）
 *
 *  外壳都是 ui_modal_dialog 的浅色档。稿上有两种宽度：
 *    560 宽、内距 32：密码框、隐藏网络、选择类（页脚钮 236×72）；
 *    496 宽、内距 24：确认类、提示类（页脚钮 212×72 / 448×72）。
 *  正文行距统一 20（副标题 → 标签 → 输入框 → 下一个标签都是这个距离）。
 * ========================================================================= */

/** 开一个新稿外壳：标题 28 + 可选副标题（20 @65%，居中）。
 *  parent：带输入框的建在 lv_screen_active()——键盘也在 screen 上，建在 lv_layer_top()
 *  会把键盘压在下面；其余建在 lv_layer_top()。 */
ui_modal_dialog_t *open_set_dialog(lv_obj_t *parent, const char *title, const char *subtitle,
                                   bool wide, bool with_footer)
{
    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user(title, wide ? USER_WIFI_DLG_W : USER_DLG_W, with_footer);
    cfg.light_theme = true;
    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }
    if(wide) {
        lv_obj_set_style_pad_all(dlg->card, USER_PAD_LG, 0);
    }
    lv_obj_set_style_pad_row(dlg->body, SET_CAPTION_GAP, 0);
    if(subtitle) {
        lv_obj_t *s = make_text(dlg->body, subtitle, user_font_title(), kOpaValue);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s, LV_PCT(100));
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        /* 稿上副标题比外壳默认的正文起点低 4 */
        lv_obj_set_style_margin_top(s, USER_PAD_SM / 4, 0);
    }
    return dlg;
}

enum class DlgBtn : uint8_t { Neutral, Primary, Danger };

/* 新稿页脚按钮：等分页脚、高 72、圆角 12。Neutral 是 `#333145 @10%` 底配深墨字（Cancel），
 * Primary 是深墨实心配白字（Connect / Export / Got It），Danger 是红 #FF4242 实心配白字
 * （格式化确认）。不走 ui_user_make_confirm_footer_btn：那枚的几何与配色是旧稿的。 */
lv_obj_t *make_dlg_btn(lv_obj_t *footer, const char *text, DlgBtn kind, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(footer);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(btn, kind == DlgBtn::Danger ? user_color_status_err
                                                          : user_light_nav_on, 0);
    lv_obj_set_style_bg_opa(btn, kind == DlgBtn::Neutral ? LV_OPA_10 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, kind == DlgBtn::Neutral ? LV_OPA_20 : LV_OPA_80,
                            LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, kind == DlgBtn::Neutral ? user_light_text
                                                             : lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, user_font_dlg_btn(), 0);
    lv_obj_center(lbl);
    return btn;
}

/* ---- 输入类弹窗（密码 / Wi-Fi 连接 / 隐藏网络，复用同一句柄，互斥打开） ---- */
ui_modal_dialog_t *g_dlg     = nullptr;
lv_obj_t          *g_pwd_ta  = nullptr;
lv_obj_t          *g_pwd_err = nullptr;
lv_obj_t          *g_ssid_ta = nullptr;   /**< 隐藏网络弹窗的 SSID 输入框 */

/* ---- 输入类弹窗给键盘让位 ----
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
        g_ssid_ta = nullptr;
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

/* 输入框统一样式。稿 6423:51743 是满宽、高 64、圆角 14、底 `#333145 @5%`、无描边，
 * placeholder 与输入的字都是深墨（前者压到 40%）。password 为假时明文（SSID）。 */
lv_obj_t *make_dialog_textarea(lv_obj_t *parent, const char *placeholder,
                               lv_keyboard_mode_t kb_mode, bool password = true)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
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

/* 输入类弹窗里左对齐的字段说明（稿 6423:51743「输入网络密码」、6437:51952
 * 「网络名称 (SSID)」那一类）：20 @65%。 */
lv_obj_t *make_pwd_dlg_hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = make_text(parent, text, user_font_title(), kOpaValue);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    return lbl;
}

/* ---- 开发者密码弹窗 → 通过则解锁 ---- */
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

void open_dev_password_dialog(void)
{
    if(g_dlg) {
        return;
    }
    /* 与 Wi-Fi 密码弹窗同一套外壳（稿只给了后者，这里对齐它）：白卡、560 宽、
     * 页脚两枚等宽钮。它走数字键盘，弹出的是右侧竖板，让位方向由 dlg_dodge_keyboard 判。 */
    g_dlg = open_set_dialog(lv_screen_active(), "Enter Developer Mode", nullptr, true, true);
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

    make_dlg_btn(g_dlg->footer, "Cancel", DlgBtn::Neutral, on_dlg_cancel);
    make_dlg_btn(g_dlg->footer, "Enter", DlgBtn::Primary, on_dev_confirm);
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

/* 稿 6423:51743：560×334 白卡，SSID 居中作标题，下面一行左对齐灰字，满宽输入框，
 * 页脚 Cancel / Connect 两枚等宽钮。 */
void open_wifi_connect_dialog(const char *ssid)
{
    if(g_dlg) {
        return;
    }
    g_connect_ssid = ssid;
    g_dlg = open_set_dialog(lv_screen_active(), ssid ? ssid : "Wi-Fi", nullptr, true, true);
    if(!g_dlg) {
        return;
    }
    ui_keyboard_set_visibility_cb(dlg_dodge_keyboard, nullptr);

    make_pwd_dlg_hint(g_dlg->body, "Enter network password");

    g_pwd_ta = make_dialog_textarea(g_dlg->body, "Password", LV_KEYBOARD_MODE_TEXT_LOWER);

    make_dlg_btn(g_dlg->footer, "Cancel", DlgBtn::Neutral, on_dlg_cancel);
    make_dlg_btn(g_dlg->footer, "Connect", DlgBtn::Primary, on_wifi_connect_confirm);
}

/* ---- 添加隐藏网络（稿 6437:51952）：只做界面 ----
 * 后端 WifiManager 连接时没设 scan_ssid 1，连不上不广播的网络，所以 Connect 只提示
 * Coming soon，填的内容不下发。 */
void on_hidden_connect(lv_event_t *)
{
    dlg_close();
    show_coming_soon();
}

void open_hidden_network_dialog(void)
{
    if(g_dlg) {
        return;
    }
    g_dlg = open_set_dialog(lv_screen_active(), "Add Hidden Network",
                            "Enter the details of a network that is not broadcast", true, true);
    if(!g_dlg) {
        return;
    }
    ui_keyboard_set_visibility_cb(dlg_dodge_keyboard, nullptr);

    make_pwd_dlg_hint(g_dlg->body, "Network Name (SSID)");
    g_ssid_ta = make_dialog_textarea(g_dlg->body, "SSID", LV_KEYBOARD_MODE_TEXT_LOWER, false);
    make_pwd_dlg_hint(g_dlg->body, "Network Password");
    g_pwd_ta = make_dialog_textarea(g_dlg->body, "Password", LV_KEYBOARD_MODE_TEXT_LOWER);

    make_dlg_btn(g_dlg->footer, "Cancel", DlgBtn::Neutral, on_dlg_cancel);
    make_dlg_btn(g_dlg->footer, "Connect", DlgBtn::Primary, on_hidden_connect);
}

/* ---- 确认类 / 提示类（同一句柄，互斥打开） ---- */
ui_modal_dialog_t *g_cfm_dlg = nullptr;

void cfm_close(void)
{
    if(g_cfm_dlg) {
        ui_modal_dialog_close(g_cfm_dlg);
        g_cfm_dlg = nullptr;
    }
}

void on_cfm_cancel(lv_event_t *)
{
    cfm_close();
}

/** 确认类（稿 6444:53244）：496 宽，Cancel + 主按钮；danger 时主按钮为红色实心。 */
void open_confirm_dialog(const char *title, const char *subtitle, const char *ok_text,
                         bool danger, lv_event_cb_t ok_cb)
{
    if(g_cfm_dlg) {
        return;
    }
    g_cfm_dlg = open_set_dialog(lv_layer_top(), title, subtitle, false, true);
    if(!g_cfm_dlg) {
        return;
    }
    make_dlg_btn(g_cfm_dlg->footer, "Cancel", DlgBtn::Neutral, on_cfm_cancel);
    make_dlg_btn(g_cfm_dlg->footer, ok_text, danger ? DlgBtn::Danger : DlgBtn::Primary, ok_cb);
}

/** 提示类（稿 6438:52611）：496 宽，单个满宽主按钮，点了就关。 */
void open_info_dialog(const char *title, const char *subtitle, const char *btn_text)
{
    if(g_cfm_dlg) {
        return;
    }
    g_cfm_dlg = open_set_dialog(lv_layer_top(), title, subtitle, false, true);
    if(!g_cfm_dlg) {
        return;
    }
    make_dlg_btn(g_cfm_dlg->footer, btn_text, DlgBtn::Primary, on_cfm_cancel);
}

/** 录像存储 / 导出日志在一个外部存储都没有时弹这个。 */
void open_no_storage_dialog(void)
{
    open_info_dialog("No Storage Device Detected",
                     "Insert a Micro SD card or USB drive and try again", "Got It");
}

/* ---- 选择类（稿 6438:52418 录像存储 / 6448:53673 导出日志）----
 * 560 宽，标题 + 副标题下每个选项一块 496×98 圆角底（选中 @8%、未选中 @4%）：左侧名称 20
 * + 可用容量 18 @55%，右侧 40 见方圆钮。录像存储点选即生效并关窗（稿上没有按钮，点遮罩
 * 也关）；导出日志点选只挪勾，等页脚 Export。 */
constexpr int kSelMax = 2;   /**< 外部存储只有 USB / SD 两类 */

struct SelectDlg {
    ui_modal_dialog_t *dlg = nullptr;
    lv_obj_t *opts[kSelMax] = {};
    lv_obj_t *radios[kSelMax] = {};
    lv_obj_t *checks[kSelMax] = {};
    int  values[kSelMax] = {};
    int  count = 0;
    int  sel = -1;
    void (*on_pick)(int value) = nullptr;   /**< 非空：点选即提交并关窗 */
};
SelectDlg g_sel;

void sel_close(void)
{
    if(g_sel.dlg) {
        ui_modal_dialog_close(g_sel.dlg);
    }
    g_sel = SelectDlg{};
}

void sel_paint(void)
{
    for(int i = 0; i < g_sel.count; i++) {
        const bool on = (i == g_sel.sel);
        lv_obj_set_style_bg_opa(g_sel.opts[i], on ? 20 : 10, 0);
        radio_paint(g_sel.radios[i], g_sel.checks[i], on);
    }
}

void on_sel_opt(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if(i < 0 || i >= g_sel.count) {
        return;
    }
    g_sel.sel = i;
    if(g_sel.on_pick) {
        void (*pick)(int) = g_sel.on_pick;
        const int value = g_sel.values[i];
        sel_close();
        pick(value);
        return;
    }
    sel_paint();
}

/** 点在遮罩上关窗。卡片自己可点，点卡片不会冒泡到遮罩这里。 */
void on_sel_backdrop(lv_event_t *e)
{
    if(g_sel.dlg && lv_event_get_target_obj(e) == g_sel.dlg->root) {
        sel_close();
    }
}

void on_sel_cancel(lv_event_t *)
{
    sel_close();
}

/** 开选择弹窗。names / details / values 各 count 项；sel 为初始选中下标（-1 = 无）。
 *  on_pick 为空时建页脚，由调用方往 g_sel.dlg 的 body / footer 里补内容。 */
void open_select_dialog(const char *title, const char *subtitle, int count,
                        const char *const *names, const char *const *details,
                        const int *values, int sel, void (*on_pick)(int))
{
    if(g_sel.dlg || count <= 0) {
        return;
    }
    g_sel.dlg = open_set_dialog(lv_layer_top(), title, subtitle, true, on_pick == nullptr);
    if(!g_sel.dlg) {
        g_sel = SelectDlg{};
        return;
    }
    g_sel.on_pick = on_pick;
    g_sel.count = count > kSelMax ? kSelMax : count;
    g_sel.sel = sel;
    lv_obj_add_event_cb(g_sel.dlg->root, on_sel_backdrop, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list = make_box(g_sel.dlg->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, USER_PAD_SM, 0);   /* 稿 选项间 16 */
    for(int i = 0; i < g_sel.count; i++) {
        lv_obj_t *opt = lv_button_create(list);
        lv_obj_remove_style_all(opt);
        lv_obj_set_size(opt, LV_PCT(100), SET_OPT_H);
        lv_obj_set_style_radius(opt, USER_HIST_CARD_RADIUS, 0);
        lv_obj_set_style_bg_color(opt, user_light_text, 0);
        lv_obj_set_style_bg_opa(opt, 31, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(opt, SET_CAPTION_GAP, 0);   /* 稿 20 */
        lv_obj_set_flex_flow(opt, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(opt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(opt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(opt, on_sel_opt, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *col = make_box(opt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_style_pad_row(col, USER_PAD_SM * 5 / 8, 0);   /* 稿 10 */
        make_text(col, names[i], user_font_title(), LV_OPA_COVER);
        make_text(col, details[i], user_font_text(), kOpaSub);

        g_sel.radios[i] = make_radio(opt, &g_sel.checks[i]);
        g_sel.opts[i] = opt;
        g_sel.values[i] = values[i];
    }
    sel_paint();
}

/* ============================================================================
 *  外部存储在位（存储页列表、录像存储 / 导出日志两个选择弹窗共用）
 *
 *  U 盘：GLAVO_USB_Hotplug_Ntf（后端已实现；建页时先读代理里缓存的最近一次）。容量查询
 *  后端未实现，回 present=0，所以 U 盘的可用容量显示 `--`。
 *  SD 卡：GLAVO_SdCard_Hotplug_Ntf 或容量回执 present=1，两条后端都还没做，目前不会出现。
 * ========================================================================= */
enum ExtDev { EXT_USB = 0, EXT_SD, EXT_COUNT };

bool     g_usb_present      = false;
bool     g_sd_hotplug       = false;    /**< SD 热插拔通知报的挂载态 */
bool     g_sd_usage_present = false;    /**< SD 容量回执报的在位 */
uint64_t g_ext_free[EXT_COUNT]       = {};
bool     g_ext_free_valid[EXT_COUNT] = {};

bool ext_present(int d)
{
    return d == EXT_USB ? g_usb_present : (g_sd_hotplug || g_sd_usage_present);
}

const char *ext_name(int d)      { return d == EXT_USB ? "USB Drive" : "Micro SD Card"; }
const char *ext_kind_text(int d) { return d == EXT_USB ? "USB storage device" : "Micro SD card"; }

ui_record_storage_t ext_to_pref(int d)
{
    return d == EXT_USB ? UI_RECORD_STORAGE_USB : UI_RECORD_STORAGE_SD;
}

/** 字节数 → GB（decimal 时保留一位，稿「42.6 GB」；否则取整，稿「共 64 GB」），不足 1 GB 用 MB。 */
void bytes_to_text(uint64_t bytes, char *buf, size_t n, bool decimal)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if(gb >= 1.0) { std::snprintf(buf, n, decimal ? "%.1f GB" : "%.0f GB", gb); }
    else          { std::snprintf(buf, n, "%.0f MB", gb * 1024.0); }
}

/** 外部存储文字缓冲的长度：容量文字（最长 24）再拼上「 available」。 */
constexpr size_t kExtTextLen = 48;

/** 「119.3 GB available」；容量未知时 `--`。 */
void ext_free_text(int d, char *buf, size_t n)
{
    if(!g_ext_free_valid[d]) {
        std::snprintf(buf, n, "--");
        return;
    }
    char v[24];
    bytes_to_text(g_ext_free[d], v, sizeof(v), true);
    std::snprintf(buf, n, "%s available", v);
}

/** 列出在位的外部存储（USB 在前），返回个数。 */
int ext_list(int *out)
{
    int n = 0;
    for(int d = 0; d < EXT_COUNT; d++) {
        if(ext_present(d)) {
            out[n++] = d;
        }
    }
    return n;
}

/** 给两个选择弹窗凑选项：名称 + 可用容量。details 由调用方提供存放文字的缓冲。 */
int ext_options(int *devs, const char **names, char (*details)[kExtTextLen], const char **dptr)
{
    const int n = ext_list(devs);
    for(int i = 0; i < n; i++) {
        names[i] = ext_name(devs[i]);
        ext_free_text(devs[i], details[i], sizeof(details[i]));
        dptr[i] = details[i];
    }
    return n;
}

/* ============================================================================
 *  Device Settings（稿 6403:1722）及其子页
 * ========================================================================= */

void on_dev_back_main(lv_event_t *) { dev_show(DEV_MAIN); }
void on_dev_back_wifi(lv_event_t *) { enter_wifi_page(); }
void on_dev_account(lv_event_t *)   { dev_show(DEV_ACCOUNT); }
void on_dev_wifi(lv_event_t *)      { enter_wifi_page(); }
void on_dev_display(lv_event_t *)   { dev_show(DEV_DISPLAY); }

/** 主页 Wi-Fi 行随状态推送就地刷新（整页重建会打断用户操作）。 */
void dev_main_refresh_wifi_row(void)
{
    if(g_dev_wifi_sub) {
        lv_label_set_text(g_dev_wifi_sub, wifi_status_text());
    }
    if(g_dev_wifi_val) {
        lv_label_set_text(g_dev_wifi_val,
                          (g_wifi_on && wifi_connected()) ? wifi_current_ssid() : "");
    }
}

/** 主页：一张卡三行。账号行右侧稿上的「已登录」不画——账号功能还没有，写了就是假话。 */
void build_device_main(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, kNavDefs[NAV_DEVICE].label,
                               kNavDefs[NAV_DEVICE].subtitle, nullptr);
    lv_obj_t *card = make_list_card(body);

    lv_obj_t *row = make_list_row(card, "Account", "View the current account and device binding",
                                  SET_ROW_SUB_H, on_dev_account);
    row_add_chevron(row);

    row = make_list_row(card, "Wi-Fi", wifi_status_text(), SET_ROW_SUB_H, on_dev_wifi);
    g_dev_wifi_sub = list_row_sub(row);
    g_dev_wifi_val = row_add_value(row, "");
    row_add_chevron(row);
    dev_main_refresh_wifi_row();

    row = make_list_row(card, "Display & Language", "Language, screen brightness and sleep time",
                        SET_ROW_SUB_H, on_dev_display);
    row_add_chevron(row);
}

/** Account（稿 6448:54075）：只做界面，页头 + Coming soon。 */
void build_account_page(lv_obj_t *parent)
{
    make_page(parent, "Account", "View the current account and device binding", on_dev_back_main);
    make_coming_soon(parent);
}

/* ---- Wi-Fi 子页（稿 6426:1914）---- */
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

void on_connected_wifi_click(lv_event_t *) { dev_show(DEV_WIFI_DETAIL); }
void on_add_hidden_click(lv_event_t *)     { open_hidden_network_dialog(); }

void on_saved_networks_click(lv_event_t *)
{
    dev_show(DEV_SAVED);
    /* 进页面时请求后端已保存列表（回来经 register_wifi_saved 刷新） */
    ui::UiMsgProxy::instance().send_wifi_saved_query();
}

/** 按 g_wifi_on 刷新开关与开关卡以下那段的显隐（后端推送会改这个值）。
 *  只改状态不发事件，回读不会再下发一次开关。 */
void wifi_sw_refresh(void)
{
    if(g_wifi_sw) {
        if(g_wifi_on) lv_obj_add_state(g_wifi_sw, LV_STATE_CHECKED);
        else          lv_obj_clear_state(g_wifi_sw, LV_STATE_CHECKED);
    }
    if(g_wifi_box) {
        if(g_wifi_on) lv_obj_remove_flag(g_wifi_box, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(g_wifi_box, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 应用 WiFi 开/关：下发、提示、开则扫描关则清扫描缓存。这里不重建页面——它跑在开关
 *  自己的 VALUE_CHANGED 里，重建会把正在派发事件的开关删掉；已连网络那一行随后由状态
 *  推送（连接态变化）触发重建。 */
void wifi_apply_enabled(bool on)
{
    g_wifi_on = on;                                    /* 记住开关意图，跨重建保持 */
    ui::UiMsgProxy::instance().send_wifi_enable(on);
    ui_toast_show(on ? "Wi-Fi On" : "Wi-Fi Off", UI_TOAST_SUCCESS, 1200);
    if(on) {
        ui::UiMsgProxy::instance().send_wifi_scan();   /* 开启后请求扫描 */
    } else {
        g_scan_valid = false;                          /* 关闭后清空扫描缓存 */
    }
    wifi_sw_refresh();
}

void on_wifi_switch(lv_event_t *e)
{
    wifi_apply_enabled(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

/** 网络行（行高 76）：Wi-Fi 字形 + SSID 20 [+ Connected] + 右箭头。
 *  已连接：点整行进详情（断开 / 忘记 / IP）。未连接：已保存的免密直连，否则弹密码框。 */
void make_wifi_row(lv_obj_t *card, const char *ssid, bool connected)
{
    lv_obj_t *row = make_row_base(card, SET_ROW_LIST_H,
                                  connected ? on_connected_wifi_click : on_wifi_item_click,
                                  const_cast<char *>(ssid));
    lv_obj_set_style_pad_column(row, USER_PAD_SM * 3 / 4, 0);   /* 稿 字形 ↔ SSID 12 */
    make_icon(row, &settings_icon_wifi, SET_ICON_SZ, user_light_text);
    lv_obj_t *lbl = make_text(row, ssid, user_font_title(), LV_OPA_COVER);
    lv_obj_set_flex_grow(lbl, 1);
    label_one_line(lbl, user_font_title());
    if(connected) {
        row_add_value(row, "Connected");
    }
    row_add_chevron(row);
}

/** 列表里的提示行（Searching / No networks found）：不可点、字压到 50%。 */
void make_hint_row(lv_obj_t *card, const char *text)
{
    lv_obj_t *row = make_list_row(card, text, nullptr, SET_ROW_LIST_H);
    lv_obj_set_style_text_opa(list_row_title(row), LV_OPA_50, 0);
}

/**
 * 第一张卡：Wi-Fi 开关行 + 已连网络行；下面「Other Networks」列表、已保存网络入口、
 * 右对齐的「Add Hidden Network」。Wi-Fi 关时开关卡以下整段隐藏。整页纵向滚动——
 * 扫到的网络条数不定，超出一屏就得能划。
 */
void build_wifi_page(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, "Wi-Fi", "Manage Wi-Fi and network connections",
                               on_dev_back_main);

    lv_obj_t *card = make_list_card(body);
    lv_obj_t *row = make_list_row(card, "Wi-Fi", nullptr, SET_ROW_H);
    g_wifi_sw = make_pref_switch(row, g_wifi_on, on_wifi_switch, nullptr);

    const char *cur_ssid = wifi_current_ssid();
    const bool  cur_connected = g_wifi_on && wifi_connected();
    if(cur_connected) {
        make_wifi_row(card, cur_ssid, true);
    }

    g_wifi_box = make_box(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_wifi_box, USER_PAD, 0);   /* 稿 列表卡 → 按钮 24 */

    lv_obj_t *sec = make_section(g_wifi_box, "Other Networks");
    lv_obj_t *list = make_list_card(sec);
    if(g_scan_valid) {
        /* 后端扫描结果：跳过已连接项，避免与第一张卡重复 */
        uint32_t shown = 0;
        for(uint32_t i = 0; i < g_scan_count; i++) {
            const char *ssid = g_scan_wifi[i].ssid;
            if(ssid[0] == '\0') {
                continue;
            }
            if(cur_connected && std::strcmp(ssid, cur_ssid) == 0) {
                continue;
            }
            make_wifi_row(list, ssid, false);
            shown++;
        }
        if(shown == 0) {
            make_hint_row(list, "No networks found");
        }
    } else {
        /* 首个扫描结果还没到：给等待提示，别拿假 SSID 充数 */
        make_hint_row(list, "Searching for networks...");
    }

    /* 已保存网络稿上没有，按需求保留，放在列表之下、添加隐藏网络之上 */
    lv_obj_t *saved = make_list_card(g_wifi_box);
    row = make_list_row(saved, "Saved Networks", nullptr, SET_ROW_LIST_H, on_saved_networks_click);
    row_add_chevron(row);

    lv_obj_t *btn_row = make_box(g_wifi_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 稿 175×56 是按中文量的；英文字长，宽度随文字 */
    lv_obj_t *add = make_outline_btn(btn_row, "Add Hidden Network", false, on_add_hidden_click);
    lv_obj_set_height(add, USER_PAD * 7 / 3);                 /* 稿 56 */
    lv_obj_set_style_radius(add, USER_HIST_CARD_RADIUS, 0);   /* 稿 16 */

    wifi_sw_refresh();
}

/* ---- 已连网络详情 / 已保存网络 ---- */
void on_forget_network(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_wifi_forget(wifi_current_ssid()); /* 预留端口 */
    ui_toast_show("Network forgotten", UI_TOAST_SUCCESS, 1500);
    enter_wifi_page();
}

void on_disconnect_network(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_wifi_disconnect(wifi_current_ssid());
    ui_toast_show("Disconnecting...", UI_TOAST_INFORM, 1500);
    enter_wifi_page();
}

void on_forget_saved(lv_event_t *e)
{
    const char *ssid = static_cast<const char *>(lv_event_get_user_data(e));
    ui::UiMsgProxy::instance().send_wifi_forget(ssid);
    ui::UiMsgProxy::instance().send_wifi_saved_query();  /* 请求后端回推最新已保存列表 */
    ui_toast_show("Network forgotten", UI_TOAST_SUCCESS, 1500);
    dev_show(DEV_SAVED);   /* 先按当前缓存刷新，后端回推后再重建 */
}

/** 已连网络详情（稿 6462:17768）：标题即 SSID、无副标题；断开 / 忘记 / IP 三行。 */
void build_wifi_detail(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, wifi_current_ssid(), nullptr, on_dev_back_wifi);
    lv_obj_t *card = make_list_card(body);

    lv_obj_t *row = make_list_row(card, "Disconnect", nullptr, SET_ROW_LIST_H, on_disconnect_network);
    row_add_chevron(row);
    row = make_list_row(card, "Forget Network", nullptr, SET_ROW_LIST_H, on_forget_network);
    row_add_chevron(row);
    row = make_list_row(card, "IP Address", nullptr, SET_ROW_LIST_H);
    g_ip_val_lbl = row_add_value(row, wifi_ip_text());
}

/** 已保存网络（稿外保留）：每行 Wi-Fi 字形 + SSID + 红描边 Forget。 */
void build_saved_networks(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, "Saved Networks",
                               "Networks this device can join automatically", on_dev_back_wifi);
    lv_obj_t *card = make_list_card(body);

    /* 后端一旦回过（g_saved_valid），就以真实数据为准——即使为空也显示空，
     * 不再回退占位（否则忘记后仍显示假数据、看似"没反应"）。 */
    const bool use_live = g_saved_valid;
    const uint32_t n = use_live
        ? g_saved_count
        : static_cast<uint32_t>(sizeof(kSavedWifi) / sizeof(kSavedWifi[0]));

    for(uint32_t i = 0; i < n; i++) {
        const char *ssid = use_live ? g_saved_wifi[i].ssid : kSavedWifi[i];
        lv_obj_t *row = make_row_base(card, SET_ROW_LIST_H, nullptr, nullptr);
        lv_obj_set_style_pad_column(row, USER_PAD_SM * 3 / 4, 0);
        make_icon(row, &settings_icon_wifi, SET_ICON_SZ, user_light_text);
        lv_obj_t *lbl = make_text(row, ssid, user_font_title(), LV_OPA_COVER);
        lv_obj_set_flex_grow(lbl, 1);
        label_one_line(lbl, user_font_title());
        make_outline_btn(row, "Forget", true, on_forget_saved, const_cast<char *>(ssid));
    }
    if(n == 0) {
        make_hint_row(card, "No saved networks");
    }
}

/* ---- Display & Language（稿 6403:1867）---- */

/* 语言分段只做界面：280×72、底 #333145 @6%、内距 2、段间 4；选中段 136×68 深墨实块配
 * 白字，未选中 @55%。固件只有英文，所以恒选 English；点「中文」提示 Coming soon，
 * 选中态不动。「中」「文」两字在中文字体子集里（user_font_cjk）。 */
void on_lang_zh(lv_event_t *) { show_coming_soon(); }

void make_lang_seg_btn(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       bool selected, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    /* 稿 14 = 外框 16 减去 2 的内距，两个圆角才同心 */
    lv_obj_set_style_radius(btn, USER_HIST_CARD_RADIUS - 2, 0);
    lv_obj_set_style_bg_color(btn, user_light_text, 0);
    lv_obj_set_style_bg_opa(btn, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if(cb) {
        lv_obj_set_style_bg_opa(btn, 15, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, selected ? lv_color_white() : user_light_text, 0);
    lv_obj_set_style_text_opa(l, selected ? static_cast<lv_opa_t>(LV_OPA_COVER) : kOpaSub, 0);
    lv_obj_center(l);
}

void make_lang_segment(lv_obj_t *parent)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    /* 宽 280 = 行高 80 × 3.5；高度那项旧式只有 64，1280 档单独走 USER_SETTINGS_SEG_H。
     * **它不跟其余卡片一样满 660 宽**——稿上这一处就是这么窄的。 */
    lv_obj_set_size(seg, USER_NAV_ITEM_H * 7 / 2,
                    USER_SETTINGS_SEG_H > 0 ? USER_SETTINGS_SEG_H : USER_NAV_ITEM_H * 4 / 5);
    lv_obj_set_style_radius(seg, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(seg, user_light_text, 0);
    lv_obj_set_style_bg_opa(seg, 15, 0);            /* 稿 6% */
    lv_obj_set_style_pad_all(seg, 2, 0);
    lv_obj_set_style_pad_column(seg, 4, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);

    make_lang_seg_btn(seg, "中文", user_font_cjk(), false, on_lang_zh);
    make_lang_seg_btn(seg, "English", user_font_title(), true, nullptr);
}

void on_backlight_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    if(!sl) {
        return;
    }
    (void)ui::backlight_set_percent(static_cast<int>(lv_slider_get_value(sl)));
}

/** 亮度滑条两侧的太阳盒（稿 48 见方、圆角 12、底 @4%，里面 24 的图标）。 */
void make_sun_box(lv_obj_t *parent, const lv_image_dsc_t *icon)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, USER_PAD * 2, USER_PAD * 2);
    lv_obj_set_style_radius(box, USER_LIGHT_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(box, user_light_text, 0);
    lv_obj_set_style_bg_opa(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(make_icon(box, icon, SET_ICON_SZ, user_light_text));
}

/* ---- 熄屏时间（入口行在 Display & Language，稿 6403:1867；选择子页稿 6655:18619）----
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
int g_sleep_idx = 5;   /**< 默认 5 min，与稿上的值一致 */

void on_sleep_row_click(lv_event_t *) { dev_show(DEV_SLEEP); }
void on_sleep_back(lv_event_t *)      { dev_show(DEV_DISPLAY); }

/** 点选：勾移到这一行、停在本页（稿是勾选列表），回 Display & Language 走页头返回箭头。 */
void on_sleep_pick(lv_event_t *e)
{
    const int idx = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if(idx >= 0 && idx < kSleepOptionCount) {
        g_sleep_idx = idx;
    }
    dev_show(DEV_SLEEP);
}

/** 三段：语言分段 / 亮度卡（96 高：小太阳盒 + 492×8 滑条 + 大太阳盒）/ 熄屏时间行。 */
void build_display_page(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, "Display & Language",
                               "Language, screen brightness and sleep time", on_dev_back_main);

    lv_obj_t *sec = make_section(body, "Language");
    make_lang_segment(sec);

    sec = make_section(body, "Screen Brightness");
    lv_obj_t *card = make_list_card(sec);
    lv_obj_set_height(card, SET_ROW_SUB_H);
    lv_obj_set_style_pad_hor(card, USER_PAD, 0);
    lv_obj_set_style_pad_column(card, USER_PAD_SM * 3 / 4, 0);   /* 稿 太阳盒 ↔ 滑条 12 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_sun_box(card, &settings_icon_sun_small);

    int pct = ui::backlight_get_percent();
    if(pct < 5)   { pct = (pct < 0) ? 80 : 5; }
    if(pct > 100) { pct = 100; }

    /* 轨道 492×8 @10%、填充段深墨；圆钮 28 白底带一圈淡投影（稿 blur 5 / 25%）。
     *
     * **圆钮不出轨道**：LVGL 的滑条在两端时圆钮中心落在滑条端点、半个圆钮伸出去，按稿
     * 12 的间距会压到太阳盒。所以滑条本体左右各缩进半个圆钮（zone 的左右内距），自己不画
     * 底；轨道底色单独一条（bar），靠 transform_width 往两边各画出半个圆钮，视觉上仍铺满。
     * 圆钮中心只在 [r, W−r] 里走，两端时圆钮边缘正好贴齐轨道端头。填充段从滑条左端
     * （轨道 x=r）才开始，左边那 r 宽补一段深色端头（cap），多伸进填充段下面一个轨道高、
     * 盖住两段圆角相接处的缺口；最右那 r 宽只在最大值时露出来，正好被圆钮盖住。
     * 不能直接给滑条加左右内距：LVGL 的填充段会跟着内距一起缩进，左端露出一截浅色。
     * zone 与太阳盒同高 48：圆钮与投影都画在它里面，点在圆钮上下也找得到滑条
     * （找控件时只往命中了父对象的子对象里找）。LVGL 的圆钮按滑条高画，四边各补 10 才到 28。 */
    const int32_t track_h = USER_PAD_SM / 2;
    const int32_t knob_sz = USER_PAD * 7 / 6;
    const int32_t knob_r  = knob_sz / 2;

    lv_obj_t *zone = lv_obj_create(card);
    lv_obj_remove_style_all(zone);
    lv_obj_set_flex_grow(zone, 1);
    lv_obj_set_height(zone, USER_PAD * 2);
    lv_obj_set_style_pad_hor(zone, knob_r, 0);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bar = lv_obj_create(zone);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), track_h);
    lv_obj_set_style_transform_width(bar, knob_r, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, user_light_text, 0);
    lv_obj_set_style_bg_opa(bar, 26, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *cap = lv_obj_create(zone);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, knob_r + track_h, track_h);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, user_light_text, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, -knob_r, 0);

    lv_obj_t *slider = lv_slider_create(zone);
    lv_obj_set_size(slider, LV_PCT(100), track_h);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 0, 0);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);   /* 主题若给内距，填充段会跟着缩进 */
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, user_light_text, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, user_light_card, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, (knob_sz - track_h) / 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, USER_PAD_SM * 3 / 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, LV_OPA_20, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, USER_PAD);
    lv_obj_add_event_cb(slider, on_backlight_slider, LV_EVENT_VALUE_CHANGED, nullptr);

    make_sun_box(card, &settings_icon_sun_large);

    sec = make_section(body, "Sleep Timeout");
    card = make_list_card(sec);
    lv_obj_t *row = make_list_row(card, "Sleep Timeout", nullptr, SET_ROW_LIST_H, on_sleep_row_click);
    lv_obj_set_style_text_font(list_row_title(row), user_font_title(), 0);   /* 稿这一行是 20 */
    row_add_value(row, kSleepOptions[g_sleep_idx]);
    row_add_chevron(row);
}

/**
 * 熄屏时间选择（稿 6655:18619）：一张卡 7 行，行高 68、字 20；只有选中行行尾画 24 圆勾，
 * 其余行不画（不是选择弹窗那种每行一枚圆钮）。这张卡带淡投影、分隔线通栏 #E5E7EB，
 * 与其它帧的列表卡不同，按稿只改这一张。
 */
void build_sleep_page(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, "Sleep Timeout",
                               "Turn off the screen after a period of inactivity", on_sleep_back);
    /* 内容列与卡同宽同高，卡的投影会被它整圈裁掉：四周垫出投影的余量，再用等量负外边距
     * 抵回（百分比宽度会减掉外边距，宽度自动补回；flex 主轴计入外边距），卡的位置不变。 */
    const int32_t sh = SET_SHADOW_W;
    lv_obj_set_style_pad_all(body, sh, 0);
    lv_obj_set_style_margin_left(body, -sh, 0);
    lv_obj_set_style_margin_right(body, -sh, 0);
    lv_obj_set_style_margin_top(body, SET_HEAD_GAP - sh, 0);
    lv_obj_set_style_margin_bottom(body, -sh, 0);

    lv_obj_t *card = make_list_card(body);
    lv_obj_add_flag(card, kCardFlushDivider);
    add_soft_shadow(card, 0);
    for(int i = 0; i < kSleepOptionCount; i++) {
        lv_obj_t *row = make_list_row(card, kSleepOptions[i], nullptr, SET_ROW_OPT_H, on_sleep_pick,
                                      reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *t = list_row_title(row);
        lv_obj_set_style_text_font(t, user_font_title(), 0);   /* 稿 20 */
        label_one_line(t, user_font_title());
        if(i == g_sleep_idx) {
            make_check_dot(row);
        }
    }
}

/** 切 Device Settings 的子页（清空右栏后重建）。只建页面，不发请求——推送触发的重建
 *  也走这里，发请求的是 enter_wifi_page 这类入口。离开 Wi-Fi 子页即停周期重扫。 */
void dev_show(DevSub sub)
{
    if(!g_detail) {
        return;
    }
    g_dev_sub = sub;
    if(sub != DEV_WIFI) {
        wifi_rescan_stop();
    }
    detail_reset();
    switch(sub) {
    case DEV_MAIN:        build_device_main(g_detail);    break;
    case DEV_ACCOUNT:     build_account_page(g_detail);   break;
    case DEV_WIFI:        build_wifi_page(g_detail);      break;
    case DEV_WIFI_DETAIL: build_wifi_detail(g_detail);    break;
    case DEV_SAVED:       build_saved_networks(g_detail); break;
    case DEV_DISPLAY:     build_display_page(g_detail);   break;
    case DEV_SLEEP:       build_sleep_page(g_detail);     break;
    }
}

/** 进 Wi-Fi 子页：请求当前状态 + 触发一次扫描（结果经 register_wifi_scan 回填），
 *  之后由 wifi_rescan_timer 周期重扫，停在本页也能跟上附近网络变化。 */
void enter_wifi_page(void)
{
    dev_show(DEV_WIFI);
    ui::UiMsgProxy::instance().send_wifi_info();
    ui::UiMsgProxy::instance().send_wifi_scan();
    wifi_rescan_start();
}

/** 推送触发的 Wi-Fi 子页重建：保住滚动位置，免得列表一刷新就被顶回顶部。 */
void wifi_page_rebuild(void)
{
    const int32_t y = lv_obj_get_scroll_y(g_detail);
    dev_show(DEV_WIFI);
    lv_obj_update_layout(g_detail);
    lv_obj_scroll_to_y(g_detail, y, LV_ANIM_OFF);
}

/* ============================================================================
 *  Preferences（稿 6457:17430）：三个开关 + 录像存储，值都落盘（ui_prefs）
 * ========================================================================= */
enum class PrefKey : uint8_t { AutoClean, AutoTimelapse, AutoRecord, UxProgram };

void on_pref_switch(lv_event_t *e)
{
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    switch(static_cast<PrefKey>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)))) {
    case PrefKey::AutoClean:     ui_prefs_set_auto_clean(on);     break;
    case PrefKey::AutoTimelapse: ui_prefs_set_auto_timelapse(on); break;
    case PrefKey::AutoRecord:    ui_prefs_set_auto_record(on);    break;
    case PrefKey::UxProgram:     ui_prefs_set_ux_program(on);     break;
    }
}

void make_pref_row(lv_obj_t *card, const char *title, const char *sub, bool on, PrefKey key)
{
    lv_obj_t *row = make_list_row(card, title, sub, SET_ROW_SUB_H);
    make_pref_switch(row, on, on_pref_switch,
                     reinterpret_cast<void *>(static_cast<uintptr_t>(key)));
}

/** 录像存储行的值：选中的设备在位就显示它；一个外部设备都没有显示 No Storage Device；
 *  有设备但没选过（或选的那个拔了）显示 Not Selected。 */
const char *record_storage_text(void)
{
    if(!ext_present(EXT_USB) && !ext_present(EXT_SD)) {
        return "No Storage Device";
    }
    switch(ui_prefs_record_storage()) {
    case UI_RECORD_STORAGE_USB:
        if(ext_present(EXT_USB)) return ext_name(EXT_USB);
        break;
    case UI_RECORD_STORAGE_SD:
        if(ext_present(EXT_SD)) return ext_name(EXT_SD);
        break;
    default:
        break;
    }
    return "Not Selected";
}

void refresh_record_storage_row(void)
{
    if(g_rec_storage_val) {
        lv_label_set_text(g_rec_storage_val, record_storage_text());
    }
}

void on_record_storage_pick(int dev)
{
    ui_prefs_set_record_storage(ext_to_pref(dev));
    refresh_record_storage_row();
}

void on_record_storage_click(lv_event_t *)
{
    int devs[EXT_COUNT];
    const char *names[EXT_COUNT];
    char details[EXT_COUNT][kExtTextLen];
    const char *dptr[EXT_COUNT];
    const int n = ext_options(devs, names, details, dptr);
    if(n == 0) {
        open_no_storage_dialog();
        return;
    }
    int sel = -1;
    for(int i = 0; i < n; i++) {
        if(ext_to_pref(devs[i]) == ui_prefs_record_storage()) {
            sel = i;
        }
    }
    open_select_dialog("Recording Storage", "Select a storage device for recordings",
                       n, names, dptr, devs, sel, on_record_storage_pick);
}

void build_prefs_detail(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, kNavDefs[NAV_PREFS].label,
                               kNavDefs[NAV_PREFS].subtitle, nullptr);
    lv_obj_t *card = make_list_card(body);
    make_pref_row(card, "Auto Clean After Print", "Clean powder automatically after a job",
                  ui_prefs_auto_clean(), PrefKey::AutoClean);
    make_pref_row(card, "Auto Timelapse", "Generate a timelapse for each print",
                  ui_prefs_auto_timelapse(), PrefKey::AutoTimelapse);
    make_pref_row(card, "Auto Recording", "Record video automatically when printing starts",
                  ui_prefs_auto_record(), PrefKey::AutoRecord);
    lv_obj_t *row = make_list_row(card, "Recording Storage",
                                  "Choose the external storage for recordings",
                                  SET_ROW_SUB_H, on_record_storage_click);
    g_rec_storage_val = row_add_value(row, record_storage_text());
    row_add_chevron(row);

    /* SD 在位目前只能从容量回执得知：进页问一次 */
    ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::SdCard);
}

/* ============================================================================
 *  Storage（稿 6443:52816 / 6444:53065）
 *
 *  **只做前端**：容量走现成的 UI_Storage_Usage_Req（media 侧只实现了主机那一卷，
 *  SD 卡回 present=0）；格式化那三条消息是新留的契约，后端未实现，发出去没人应答。
 *  后端要补什么见 doc/STORAGE_BACKEND_API.md。
 * ========================================================================= */

lv_obj_t *g_int_used_lbl  = nullptr;          /**< 内置卡左上「42.6 GB used」 */
lv_obj_t *g_int_total_lbl = nullptr;          /**< 右上「Total 64 GB」 */
lv_obj_t *g_int_bar       = nullptr;          /**< 用量条的填充段 */
lv_obj_t *g_int_free_lbl  = nullptr;          /**< 左下「21.4 GB available」 */
lv_obj_t *g_ext_val_lbl[EXT_COUNT] = {};      /**< 外部存储行右侧的可用容量 */
middleware::StorageVolume g_fmt_target = middleware::StorageVolume::Internal;

/** 内置卡就地刷新；present=0 时回到 `--` 并把条清零。 */
void apply_internal_usage(const middleware::StorageUsagePayload &p)
{
    if(!g_int_used_lbl || !g_int_total_lbl || !g_int_bar || !g_int_free_lbl) {
        return;
    }
    if(!p.present || p.total_bytes == 0) {
        lv_label_set_text(g_int_used_lbl, "-- used");
        lv_label_set_text(g_int_total_lbl, "Total --");
        lv_label_set_text(g_int_free_lbl, "-- available");
        lv_obj_set_width(g_int_bar, 0);
        return;
    }
    char v[24];
    char buf[48];
    bytes_to_text(p.used_bytes, v, sizeof(v), true);
    std::snprintf(buf, sizeof(buf), "%s used", v);
    lv_label_set_text(g_int_used_lbl, buf);
    bytes_to_text(p.total_bytes, v, sizeof(v), false);
    std::snprintf(buf, sizeof(buf), "Total %s", v);
    lv_label_set_text(g_int_total_lbl, buf);
    bytes_to_text(p.total_bytes - p.used_bytes, v, sizeof(v), true);
    std::snprintf(buf, sizeof(buf), "%s available", v);
    lv_label_set_text(g_int_free_lbl, buf);
    const int32_t pct = static_cast<int32_t>(p.used_bytes * 100u / p.total_bytes);
    lv_obj_set_width(g_int_bar, lv_pct(pct > 100 ? 100 : pct));
}

/** 收到容量回执：内置卷刷内置卡；外部卷记下可用容量，SD 的在位以回执为准之一。 */
void storage_apply_usage(const middleware::StorageUsagePayload &p)
{
    const auto vol = static_cast<middleware::StorageVolume>(p.volume);
    if(vol == middleware::StorageVolume::Internal) {
        apply_internal_usage(p);
        return;
    }
    const int d = (vol == middleware::StorageVolume::SdCard) ? EXT_SD : EXT_USB;
    g_ext_free_valid[d] = (p.present && p.total_bytes > 0);
    if(g_ext_free_valid[d]) {
        g_ext_free[d] = p.total_bytes - p.used_bytes;
    }
    if(d == EXT_SD) {
        const bool was = ext_present(EXT_SD);
        g_sd_usage_present = (p.present != 0);
        if(ext_present(EXT_SD) != was) {
            on_ext_presence_changed();
            return;
        }
    }
    if(g_ext_val_lbl[d]) {
        char buf[kExtTextLen];
        ext_free_text(d, buf, sizeof(buf));
        lv_label_set_text(g_ext_val_lbl[d], buf);
    }
}

void on_fmt_confirm(lv_event_t *)
{
    cfm_close();
    ui::UiMsgProxy::instance().send_storage_format(g_fmt_target);
    /* 后端未实现，这条发出去不会有回执；toast 明说，免得用户以为在跑 */
    ui_toast_show("Format requested (backend pending)", UI_TOAST_INFORM, 2500);
}

/** 格式化二次确认（稿 6444:53244）：496 宽，Cancel + 红色 Format。 */
void open_format_confirm(middleware::StorageVolume vol, const char *what)
{
    g_fmt_target = vol;
    char title[64];
    std::snprintf(title, sizeof(title), "Format %s?", what);
    open_confirm_dialog(title, "All data will be erased and cannot be recovered",
                        "Format", true, on_fmt_confirm);
}

void on_fmt_internal(lv_event_t *)
{
    open_format_confirm(middleware::StorageVolume::Internal, "Internal Storage");
}

void on_fmt_ext(lv_event_t *e)
{
    const int d = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if(!ext_present(d)) {
        return;   /* 点之前刚被拔掉，页面马上会随插拔通知重建 */
    }
    open_format_confirm(d == EXT_USB ? middleware::StorageVolume::UsbDisk
                                     : middleware::StorageVolume::SdCard, ext_name(d));
}

/** 内置存储卡（稿 150 高）：[已用 22 ... 总量 18 @65%] / 满宽 12 高用量条 / 剩余 18 @65%。 */
void make_internal_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_list_card(parent);
    lv_obj_set_style_pad_hor(card, USER_PAD, 0);
    lv_obj_set_style_pad_top(card, USER_PAD_LG, 0);              /* 稿 32 */
    lv_obj_set_style_pad_bottom(card, USER_PAD_SM * 5 / 4, 0);   /* 稿 ≈21 */

    lv_obj_t *top = make_box(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    g_int_used_lbl  = make_text(top, "-- used", user_font_dlg_btn(), LV_OPA_COVER);
    g_int_total_lbl = make_text(top, "Total --", user_font_text(), kOpaValue);

    lv_obj_t *track = lv_obj_create(card);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, LV_PCT(100), USER_PAD_SM * 3 / 4);   /* 稿 12 */
    lv_obj_set_style_margin_top(track, USER_PAD, 0);            /* 稿 24 */
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, user_light_text, 0);
    lv_obj_set_style_bg_opa(track, 26, 0);                      /* 稿 10% */
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    g_int_bar = lv_obj_create(track);
    lv_obj_remove_style_all(g_int_bar);
    lv_obj_set_size(g_int_bar, 0, LV_PCT(100));
    lv_obj_set_style_radius(g_int_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_int_bar, user_light_text, 0);
    lv_obj_set_style_bg_opa(g_int_bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_int_bar, LV_OBJ_FLAG_CLICKABLE);

    g_int_free_lbl = make_text(card, "-- available", user_font_text(), kOpaValue);
    lv_obj_set_style_margin_top(g_int_free_lbl, USER_PAD_SM, 0);   /* 稿 16 */
}

/** 存储页：内置存储卡 / 已连接的外部存储（没有就整段不画：稿 6444:53065 无外部设备、
 *  6443:52816 插了 U 盘）/ 格式化（内置一行 + 每个已连接的外部设备一行）。 */
void build_storage_detail(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, kNavDefs[NAV_STORAGE].label,
                               kNavDefs[NAV_STORAGE].subtitle, nullptr);

    lv_obj_t *sec = make_section(body, "Internal Storage");
    make_internal_card(sec);

    int devs[EXT_COUNT];
    const int n = ext_list(devs);
    if(n > 0) {
        sec = make_section(body, "Connected External Storage");
        lv_obj_t *card = make_list_card(sec);
        for(int i = 0; i < n; i++) {
            char title[32];
            std::snprintf(title, sizeof(title), "External Storage %d", i + 1);
            /* 稿上这一张单行卡是 86 高，比其余带副标题的行矮 10 */
            lv_obj_t *row = make_list_row(card, title, ext_kind_text(devs[i]),
                                          SET_ROW_SUB_H - USER_PAD_SM * 5 / 8);
            char v[kExtTextLen];
            ext_free_text(devs[i], v, sizeof(v));
            g_ext_val_lbl[devs[i]] = row_add_value(row, v);
        }
    }

    sec = make_section(body, "Format");
    lv_obj_t *card = make_list_card(sec);
    lv_obj_t *row = make_list_row(card, "Format Internal Storage", "Erase all data on the device",
                                  SET_ROW_SUB_H);
    make_outline_btn(row, "Format", true, on_fmt_internal);
    for(int i = 0; i < n; i++) {
        char title[48];
        std::snprintf(title, sizeof(title), "Format %s", ext_name(devs[i]));
        row = make_list_row(card, title,
                            devs[i] == EXT_USB ? "Erase all data on the USB drive"
                                               : "Erase all data on the SD card",
                            SET_ROW_SUB_H);
        make_outline_btn(row, "Format", true, on_fmt_ext,
                         reinterpret_cast<void *>(static_cast<intptr_t>(devs[i])));
    }

    /* 进页拉一次各卷容量（SD 卡那支后端未实现，会回 present=0；U 盘同样） */
    ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::Internal);
    ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::SdCard);
    if(g_usb_present) {
        ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::UsbDisk);
    }
}

/** 外部存储插拔：存储页整页重建（列表与格式化行都随在位变化，保住滚动位置），
 *  偏好页只刷录像存储那一行。 */
void on_ext_presence_changed(void)
{
    if(!g_detail) {
        return;
    }
    if(g_selected == NAV_STORAGE) {
        const int32_t y = lv_obj_get_scroll_y(g_detail);
        show_detail(NAV_STORAGE);
        lv_obj_update_layout(g_detail);
        lv_obj_scroll_to_y(g_detail, y, LV_ANIM_OFF);
    } else if(g_selected == NAV_PREFS) {
        refresh_record_storage_row();
    }
}

/* ============================================================================
 *  Nitrogen Generator：只做界面，页头 + Coming soon
 *  Privacy & Security（稿 6448:53385）：两条文档进 Coming soon 子页，体验计划开关落盘
 * ========================================================================= */

void build_nitrogen_detail(lv_obj_t *parent)
{
    make_page(parent, kNavDefs[NAV_NITROGEN].label, kNavDefs[NAV_NITROGEN].subtitle, nullptr);
    make_coming_soon(parent);
}

void on_priv_back(lv_event_t *) { show_detail(NAV_PRIVACY); }

/** 条款 / 隐私政策子页：页头 + Coming soon（正文还没有）。 */
void priv_show_doc(const char *title, const char *subtitle)
{
    detail_reset();
    make_page(g_detail, title, subtitle, on_priv_back);
    make_coming_soon(g_detail);
}

void on_priv_terms(lv_event_t *)
{
    priv_show_doc("Terms of Use", "View the current terms of service");
}

void on_priv_policy(lv_event_t *)
{
    priv_show_doc("Privacy Policy", "Learn how data is collected and used");
}

void build_privacy_detail(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, kNavDefs[NAV_PRIVACY].label,
                               kNavDefs[NAV_PRIVACY].subtitle, nullptr);
    lv_obj_t *card = make_list_card(body);
    lv_obj_t *row = make_list_row(card, "Terms of Use", "View the current terms of service",
                                  SET_ROW_SUB_H, on_priv_terms);
    row_add_chevron(row);
    row = make_list_row(card, "Privacy Policy", "Learn how data is collected and used",
                        SET_ROW_SUB_H, on_priv_policy);
    row_add_chevron(row);
    make_pref_row(card, "Join User Experience Program",
                  "Share usage data to help us improve the product",
                  ui_prefs_ux_program(), PrefKey::UxProgram);
}

/* ============================================================================
 *  Advanced Print（稿外保留）：打印前门控 / Wi-Fi 传输 / 校准
 * ========================================================================= */

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
        [](bool on) { set_gate_switch(g_gate_hold_z, on); },
        [](bool on) { set_gate_switch(g_gate_powder, on); },
        [](bool on) { set_gate_switch(g_gate_pm25, on); });
    ui::DebugMsgProxy::instance().register_plate_work_z(apply_plate_work_z_text);
}

void request_print_gate_state(void)
{
    auto& proxy = ui::DebugMsgProxy::instance();
    proxy.request_skip_outer_door_state();
    proxy.request_inner_door_occlusion_state();
    proxy.request_plate_height_adj_state();
    proxy.request_skip_o2_gate_state();
    proxy.request_powder_low_check_state();
    proxy.request_door_pm_check_state();
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
    case PrintGate::PlateHeight:  proxy.send_plate_height_adj_set(on); break;
    case PrintGate::Oxygen:       proxy.send_skip_o2_gate_set(!on); break;
    case PrintGate::PowderLow:    proxy.send_powder_low_check_set(on); break;
    case PrintGate::DoorPm:       proxy.send_door_pm_check_set(on); break;
    case PrintGate::HoldPrepZ:    proxy.send_hold_prep_z_set(on); break;
    case PrintGate::TransferBox:  control_set_transfer_box_flow(on); break;
    case PrintGate::AutoPrint:
        if(!upc::xfr::set_auto_print_after_transfer(on)) {
            set_gate_switch(g_gate_auto_print, !on);
            ui_toast_show("Failed to save setting", UI_TOAST_ERROR, 2000);
            break;
        }
        ui_toast_show(on ? "Slicer will auto-start print"
                         : "Transfer only, no auto-start",
                      UI_TOAST_SUCCESS, 1800);
        break;
    }
}

void on_settings_height_calib_click(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_height_calib_start(
        -5.0f, 12.0f, 1.0f, "/userdata/visual/height_lut.txt");
    if(g_height_calib_lbl) {
        lv_label_set_text(g_height_calib_lbl, "Starting...");
    }
    ui_toast_show("Height calibration started", UI_TOAST_SUCCESS, 1500);
}

/** 门控行（单行 76）：标题 + 新稿开关，返回开关。 */
lv_obj_t *make_gate_row(lv_obj_t *card, const char *label, PrintGate gate, bool checked = true)
{
    lv_obj_t *row = make_list_row(card, label, nullptr, SET_ROW_LIST_H);
    return make_pref_switch(row, checked, on_print_gate_changed,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(gate)));
}

void build_print_detail(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, kNavDefs[NAV_PRINT].label,
                               kNavDefs[NAV_PRINT].subtitle, nullptr);

    lv_obj_t *sec = make_section(body, "Print Checks");
    lv_obj_t *card = make_list_card(sec);
    g_gate_outer = make_gate_row(card, "Outer Door Check", PrintGate::OuterDoor);
    g_gate_pm25 = make_gate_row(card, "PM2.5 Check", PrintGate::DoorPm);
    g_gate_inner = make_gate_row(card, "Inner Door Vision", PrintGate::InnerVision, false);
    g_gate_height = make_gate_row(card, "Vision Check", PrintGate::PlateHeight);
    g_gate_o2 = make_gate_row(card, "Oxygen Check", PrintGate::Oxygen);
    g_gate_powder = make_gate_row(card, "Powder Low Check", PrintGate::PowderLow, false);
    g_gate_hold_z = make_gate_row(card, "Keep Z Position", PrintGate::HoldPrepZ, false);
    g_gate_transfer = make_gate_row(card, "Transfer Box", PrintGate::TransferBox,
                                    control_transfer_box_flow_enabled());

    sec = make_section(body, "Wi-Fi Transfer");
    card = make_list_card(sec);
    g_gate_auto_print = make_gate_row(card, "Auto Print After Transfer", PrintGate::AutoPrint,
                                      upc::xfr::auto_print_after_transfer_enabled());

    sec = make_section(body, "Calibration");
    card = make_list_card(sec);

    /* Calibration 内：测高收到约 1mm 之后的目标高度。填 3 则再向上 2mm。 */
    lv_obj_t *row = make_list_row(card, "Work Height (mm)", nullptr, SET_ROW_H);
    g_plate_work_z_ta = lv_textarea_create(row);
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

    /* 副标题位放测高进度（Idle / h=… / Done） */
    row = make_list_row(card, "Height Calibration", "Idle", SET_ROW_SUB_H);
    g_height_calib_lbl = list_row_sub(row);
    make_outline_btn(row, "Start", false, on_settings_height_calib_click);

    bind_print_gate_observers();
    request_print_gate_state();
}

/* ============================================================================
 *  About（稿 6448:53532 / 6448:53673）
 * ========================================================================= */

/** 连点 Firmware Version 那一行三次，呼出开发者密码。已解锁则忽略。 */
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

/** About「Check for Update」：发检查请求。有无新版本的弹窗/提示在 ui_app。 */
void on_check_update_click(lv_event_t * /*e*/)
{
    ui_toast_show("Checking for updates...", UI_TOAST_INFORM, 1500);
    ui::UiMsgProxy::instance().send_ota_check();
}

/* 导出日志只做界面：选好存储点 Export 只提示 Coming soon。 */
void on_export_confirm(lv_event_t *)
{
    sel_close();
    show_coming_soon();
}

void on_export_logs_click(lv_event_t *)
{
    int devs[EXT_COUNT];
    const char *names[EXT_COUNT];
    char details[EXT_COUNT][kExtTextLen];
    const char *dptr[EXT_COUNT];
    const int n = ext_options(devs, names, details, dptr);
    if(n == 0) {
        open_no_storage_dialog();
        return;
    }
    open_select_dialog("Export Logs", "Select a connected external storage device",
                       n, names, dptr, devs, 0, nullptr);
    if(!g_sel.dlg) {
        return;
    }
    /* 选项下方左对齐的去向说明（稿 18 @52%），页脚 Cancel / Export */
    lv_obj_t *note = make_text(g_sel.dlg->body,
                               "Logs will be exported to /SnowX-Logs in the root directory",
                               user_font_text(), 133);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
    make_dlg_btn(g_sel.dlg->footer, "Cancel", DlgBtn::Neutral, on_sel_cancel);
    make_dlg_btn(g_sel.dlg->footer, "Export", DlgBtn::Primary, on_export_confirm);
}

/* 恢复出厂只做界面：稿上没有它的确认弹窗，点 Restore 直接提示 Coming soon。 */
void on_factory_reset_click(lv_event_t *) { show_coming_soon(); }

/**
 * 卡一：设备型号 / 序列号 / 固件版本 / 检查更新 / MCU 版本 / 振镜版本（单行 80）；解锁后多两行 OTA。
 * 卡二：导出日志 / 恢复出厂（红字 + 红描边 Restore）。
 * 稿上固件版本下的「连续点击 5 次进入调试模式」按需求不显示，行尾箭头也不画——
 * 这一行不跳转，入口保持不显眼。
 */
void build_about_detail(lv_obj_t *parent)
{
    lv_obj_t *body = make_page(parent, kNavDefs[NAV_ABOUT].label,
                               kNavDefs[NAV_ABOUT].subtitle, nullptr);
    lv_obj_t *card = make_list_card(body);

    lv_obj_t *row = make_list_row(card, "Device Model", nullptr, SET_ROW_H);
    row_add_value(row, about_device_model_text());
    row = make_list_row(card, "Serial Number", nullptr, SET_ROW_H);
    row_add_value(row, about_serial_text());
    /* 整行可点、不给按压反馈：连点三次呼出开发者密码 */
    row = make_list_row(card, "Firmware Version", nullptr, SET_ROW_H, on_sw_version_click);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);
    row_add_value(row, UPC_SW_VERSION);
    /* 主动检查更新：按钮在行右侧。结果弹窗在 ui_app，不绑本页生命周期。 */
    row = make_list_row(card, "Check for Update", nullptr, SET_ROW_H);
    make_outline_btn(row, "Check", false, on_check_update_click);
    /* MCU 版本由串口异步返回：先填缓存（首次为 "--"），应答/超时后由 mcu_ver_* 刷新 */
    row = make_list_row(card, "MCU Version", nullptr, SET_ROW_H);
    g_mcu_ver_lbl = row_add_value(row, g_mcu_ver_text);
    /* 振镜板版本来自 TCP 0x0F 首包缓存：先填缓存，进页 Req 后由 galvo_ver_* 刷新 */
    row = make_list_row(card, "Galvo Version", nullptr, SET_ROW_H);
    g_galvo_ver_lbl = row_add_value(row, g_galvo_ver_text);

    /* 两枚 OTA 入口日常隐藏，与左栏 Debug 一起在连点版本号并输对密码后才出现。 */
    if(g_dev_unlocked) {
        row = make_list_row(card, "Galvo OTA", nullptr, SET_ROW_H);
        make_outline_btn(row, "Start", false, on_glavo_ota_entry_click);
        row = make_list_row(card, "MCU OTA", nullptr, SET_ROW_H);
        make_outline_btn(row, "Start", false, on_mcu_ota_entry_click);
    }

    card = make_list_card(body);
    row = make_list_row(card, "Export Logs", "Export runtime logs for troubleshooting",
                        SET_ROW_SUB_H, on_export_logs_click);
    row_add_chevron(row);
    row = make_list_row(card, "Factory Reset", "Clear local settings and account data",
                        SET_ROW_SUB_H);
    lv_obj_set_style_text_color(list_row_title(row), user_color_status_err, 0);
    lv_obj_set_style_text_color(list_row_sub(row), user_color_status_err, 0);
    make_outline_btn(row, "Restore", true, on_factory_reset_click);

    /* 导出日志要知道 SD 在不在：进页问一次 */
    ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::SdCard);
}

/* ============================================================================
 *  右栏切换 + 左栏
 * ========================================================================= */

/** 清空右栏，并把挂在旧页面上的指针全部置空：异步回调（Wi-Fi 推送、版本应答、容量回执、
 *  门控回读）都靠这些指针判断目标还在不在，漏一个就会写已释放的对象。 */
void detail_reset(void)
{
    g_dev_wifi_sub = nullptr;
    g_dev_wifi_val = nullptr;
    g_wifi_sw = nullptr;
    g_wifi_box = nullptr;
    g_ip_val_lbl = nullptr;
    g_rec_storage_val = nullptr;
    g_int_used_lbl = g_int_total_lbl = g_int_bar = g_int_free_lbl = nullptr;
    for(lv_obj_t *&l : g_ext_val_lbl) {
        l = nullptr;
    }
    /* About 面板即将销毁：先断开异步刷新用的标签指针并停掉版本查询定时器，
     * 否则离开 About 后到达的应答/超时会写已释放的 label。 */
    g_mcu_ver_lbl = nullptr;
    g_galvo_ver_lbl = nullptr;
    mcu_ver_stop_timer();
    g_gate_outer = g_gate_inner = g_gate_height = nullptr;
    g_gate_o2 = g_gate_powder = g_gate_pm25 = g_gate_hold_z = nullptr;
    g_gate_transfer = nullptr;
    g_gate_auto_print = nullptr;
    g_height_calib_lbl = nullptr;
    g_plate_work_z_ta = nullptr;
    clear_print_gate_observers();
    lv_obj_clean(g_detail);
    lv_obj_scroll_to_y(g_detail, 0, LV_ANIM_OFF);
}

void show_detail(NavId id)
{
    if(!g_detail) {
        return;
    }
    g_dev_sub = DEV_MAIN;   /* 切换左侧导航时复位子页跟踪 */
    wifi_rescan_stop();
    detail_reset();

    switch(id) {
    case NAV_DEVICE:
        build_device_main(g_detail);
        ui::UiMsgProxy::instance().send_wifi_info();   /* Wi-Fi 行要最新状态 */
        break;
    case NAV_NITROGEN: build_nitrogen_detail(g_detail); break;
    case NAV_PREFS:    build_prefs_detail(g_detail);    break;
    case NAV_STORAGE:  build_storage_detail(g_detail);  break;
    case NAV_PRIVACY:  build_privacy_detail(g_detail);  break;
    case NAV_PRINT:    build_print_detail(g_detail);    break;
    case NAV_ABOUT:
        build_about_detail(g_detail);
        /* 进入 About：向 MCU 查询一次固件版本；向 Glavo 读振镜板 TCP 首包缓存 */
        mcu_ver_query_on_enter();
        galvo_ver_query_on_enter();
        break;
    default: break;
    }
}

void update_nav_highlight(void)
{
    for(int i = 0; i < NAV_COUNT; i++) {
        if(!g_nav_btns[i]) {
            continue;
        }
        if(i == g_selected) {
            lv_obj_add_state(g_nav_btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_nav_btns[i], LV_STATE_CHECKED);
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
    /* 稿 72 = 20 + 32 图标 + 20。不改 USER_NAV_ITEM_H：选文件页的分段宽靠它算。 */
    lv_obj_set_height(btn, USER_NAV_ITEM_H * 9 / 10);
    /* 未选中无底；选中是白卡（圆角 16 + 1px 墨色 6% 描边 + 淡投影）。投影 6457:17430 没画，
     * 6403:1722 / 6426:1914 / 6655:18619 都有，取有。1280 档导航列四周有内距，投影放得下；
     * 其余档位导航列零内距，投影两侧与上沿会被裁掉，只剩底边一道。 */
    lv_obj_set_style_radius(btn, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, user_light_card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(btn, 1, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, user_light_text, LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(btn, 15, LV_STATE_CHECKED);
    add_soft_shadow(btn, LV_STATE_CHECKED);
    /* 按下压一档墨色；选中项按下仍保持白卡（单写 PRESSED 会盖过 CHECKED 的白底） */
    lv_obj_set_style_bg_color(btn, user_light_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 10, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, user_light_card, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, USER_PAD, 0);
    lv_obj_set_style_pad_column(btn, USER_PAD, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 新稿未选中项不再压淡：文字与图标都是全墨色，只靠白卡区分选中 */
    lv_obj_set_style_text_color(btn, user_light_text, 0);
    lv_obj_add_event_cb(btn, on_nav_click, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));

    /* 稿图标盒 32 见方。旧式取行高的 ~5/9（A8 图形本身只占位图约 75%，不宜直接用
     * 文字行高，否则视觉过小）；1280 档直接用稿值，其余档位仍走旧式。 */
    const int32_t icon_sz = (USER_SETTINGS_NAV_W > 0) ? 32 : USER_NAV_ITEM_H * 5 / 9;
    make_icon(btn, d.icon, icon_sz, user_light_text);

    /* 稿 22；英文「Nitrogen Generator」在 22 号下约 206，超出可用宽 196 会被截断，取 20 */
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, d.label);
    lv_obj_set_style_text_font(lbl, user_font_title(), 0);
    lv_obj_set_flex_grow(lbl, 1);
    label_one_line(lbl, user_font_title());

    /* 新稿各项都不带箭头。Debug 是例外：它不进右栏、是跳出去开发者页，保留一枚箭头
     * 提示「有去处」（旧稿给的是外链箭头 ↗，图标库没有斜向的，用同向的 ›）。 */
    if(id == NAV_DEBUG) {
        lv_obj_t *arrow = make_icon(btn, &settings_icon_chev_right, SET_ICON_SZ, user_light_text);
        lv_obj_set_style_image_opa(arrow, LV_OPA_50, 0);
    }

    g_nav_btns[id] = btn;
    return btn;
}

/**
 * 左栏：一条满高的渐变底带 + 里面 300 宽的导航列。
 *
 * 稿 6457:17430 给底带的是**竖向渐变** `#FAFAFA → #F4F4F4(50%) → #FAFAFA`，364×800
 * 铺满整屏高（整屏 x 128..492，右缘正是右栏的起点），导航列在里面内缩 32 / 48。
 * LVGL 的 bg_grad 只有两个色标（LV_GRADIENT_MAX_STOPS 2），中间那档做不出来，所以拆成
 * 上下两半各一段渐变：上半 FAFAFA→F4F4F4、下半 F4F4F4→FAFAFA，接缝在 y=400、与稿的
 * 50% 色标重合。
 *
 * 导航项高 72、项距 10：稿 6457:17430 前两档间距 12、后三档 8，各帧位置一致，取误差
 * 最小的 10。8 项全显（解锁 Debug 后）高 48 + 8×72 + 7×10 = 694，放得下。
 */
lv_obj_t *build_subnav(lv_obj_t *parent)
{
    const bool spec = (USER_SETTINGS_NAV_W > 0);   /* 有稿的档位（1280） */
    const int32_t pad_hor = spec ? USER_PAD_LG : USER_PAD;   /* 稿 32 */
    const int32_t pad_top = USER_PAD * 2;                    /* 稿 48 */

    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_remove_style_all(band);
    lv_obj_set_width(band, LV_SIZE_CONTENT);
    lv_obj_set_height(band, LV_PCT(100));
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    if(spec) {
        /* 底带自身不能带内距：两块渐变是浮动子节点，百分比尺寸与位置都按父节点的内容区算，
         * 内距放在底带上，渐变就只铺到导航列那 300×752。内距由导航列自己带。 */
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
    } else {
        lv_obj_set_style_pad_hor(band, pad_hor, 0);
        lv_obj_set_style_pad_top(band, pad_top, 0);
    }

    lv_obj_t *nav = lv_obj_create(band);
    lv_obj_remove_style_all(nav);
    /* 稿 300 定宽，连两侧内距共 364（浮动的渐变不计入底带的内容宽，底带宽就等于它）；
     * 该字段为 0 的档位沿用旧式百分比，内距在底带上。 */
    if(spec) {
        lv_obj_set_width(nav, USER_SETTINGS_NAV_W + 2 * pad_hor);
        lv_obj_set_style_pad_hor(nav, pad_hor, 0);
        lv_obj_set_style_pad_top(nav, pad_top, 0);
    } else {
        lv_obj_set_width(nav, LV_PCT(34));
    }
    lv_obj_set_height(nav, LV_PCT(100));
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nav, USER_PAD_SM * 5 / 8, 0);   /* 稿 10 */
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
    /* 进页（含从其它页切回）：Wi-Fi 子页要重新拉一遍状态并重启周期重扫，否则缓存里
     * 可能是上次离开时的旧状态；Device Settings 主页的 Wi-Fi 行同理。 */
    if(g_selected == NAV_DEVICE) {
        ui::UiMsgProxy::instance().send_wifi_info();
        if(g_dev_sub == DEV_WIFI) {
            ui::UiMsgProxy::instance().send_wifi_scan();
            wifi_rescan_start();
        }
    } else if(g_selected == NAV_PRINT && g_gate_outer) {
        bind_print_gate_observers();
        request_print_gate_state();
        set_gate_switch(g_gate_transfer, control_transfer_box_flow_enabled());
        set_gate_switch(g_gate_auto_print, upc::xfr::auto_print_after_transfer_enabled());
    } else if(g_selected == NAV_STORAGE) {
        /* 离开期间可能写过文件 / 格式化过：重拉容量 */
        ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::Internal);
        ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::SdCard);
    }
}

void page_user_settings_dismiss_dialog(void)
{
    wifi_rescan_stop();   /* 离开设置页：停掉周期重扫 */
    dlg_close();
    sel_close();
    cfm_close();
    ota_close_dialog(OtaTarget::Glavo);
    ota_close_dialog(OtaTarget::McuSerial);
    close_connecting_dialog();
    mcu_ver_stop_timer();   /* 离开设置页：不再重发版本查询 */
}

lv_obj_t *page_user_settings_create(lv_obj_t *parent)
{
    g_selected = NAV_DEVICE;
    g_dev_sub  = DEV_MAIN;
    g_detail   = nullptr;
    g_dev_wifi_sub = nullptr;
    g_dev_wifi_val = nullptr;
    g_wifi_sw = nullptr;
    g_wifi_box = nullptr;
    g_rec_storage_val = nullptr;
    g_mcu_ver_lbl = nullptr;
    g_gate_outer = g_gate_inner = g_gate_height = nullptr;
    g_gate_o2 = g_gate_powder = g_gate_pm25 = g_gate_hold_z = nullptr;
    g_gate_transfer = nullptr;
    g_gate_auto_print = nullptr;
    g_height_calib_lbl = nullptr;
    g_plate_work_z_ta = nullptr;
    clear_print_gate_observers();
    for(int i = 0; i < NAV_COUNT; i++) {
        g_nav_btns[i] = nullptr;
    }
    /* 本页建得晚（首次进设置页才建），Glavo 启动时推的那条 U 盘状态早就过去了，
     * 先读代理缓存的最近一次，之后靠下面的监听跟进。 */
    g_usb_present = ui::UiMsgProxy::instance().usb_mounted();
    g_sd_hotplug  = ui::UiMsgProxy::instance().sdcard_mounted();

    /* 缓存后端网络状态：IP 就地改标签；主页 Wi-Fi 行就地改字；连接态 / 开关变了才重建
     * Wi-Fi 子页——整页重建会打断用户操作，所以不能每条推送都建。 */
    ui::UiMsgProxy::instance().add_wifi_state_listener(
        [](const middleware::WifiStatePayload &s) {
            const bool was_valid = g_wifi_state_valid;
            const bool conn_changed =
                !was_valid ||
                (g_wifi_state.connected != s.connected) ||
                (std::strcmp(g_wifi_state.ssid, s.ssid) != 0);
            const bool en_changed = !was_valid || (g_wifi_on != (s.enabled != 0));

            g_wifi_state = s;
            g_wifi_state_valid = true;
            g_wifi_on = (s.enabled != 0);

            if(g_ip_val_lbl) {
                lv_label_set_text(g_ip_val_lbl, wifi_ip_text());
            }
            if(g_selected != NAV_DEVICE) {
                return;
            }
            switch(g_dev_sub) {
            case DEV_MAIN:
                dev_main_refresh_wifi_row();
                break;
            case DEV_WIFI:
                if(conn_changed || en_changed) {
                    wifi_page_rebuild();   /* 已连网络那一行随连接态出没 */
                } else {
                    wifi_sw_refresh();
                }
                break;
            case DEV_WIFI_DETAIL:
                if(!s.connected) {
                    enter_wifi_page();     /* 已断开，详情页无内容可看 */
                }
                break;
            default:
                break;
            }
        });

    /* 缓存扫描结果；若正停在 Wi-Fi 子页且条目有变化则用真实列表重建。 */
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
            if(list_changed && g_selected == NAV_DEVICE && g_dev_sub == DEV_WIFI) {
                wifi_page_rebuild();   /* 用真实扫描列表重建 */
            }
        });

    /* 缓存后端已保存网络列表；若正停在 Saved Networks 页则刷新。 */
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
            if(g_selected == NAV_DEVICE && g_dev_sub == DEV_SAVED) {
                dev_show(DEV_SAVED);   /* 用真实列表重建（不再重发请求，避免回环） */
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

    /* 容量回执：内置卡就地刷新；外部卷记容量与 SD 在位（不在存储页也要记，弹窗要用）。 */
    ui::UiMsgProxy::instance().register_storage_usage(
        [](const middleware::StorageUsagePayload &p) {
            storage_apply_usage(p);
        });

    /* U 盘热插拔：外部存储列表、格式化行、两个选择弹窗都看它。这条后端**已实现**。
     * 与文件页共用 add_usb_hotplug_listener，勿用单槽 register，否则后注册的会顶掉这边。 */
    ui::UiMsgProxy::instance().add_usb_hotplug_listener(
        [](const middleware::UsbHotplugPayload &p) {
            const bool now = (p.present != 0 && p.mounted != 0);
            if(now == g_usb_present) {
                return;
            }
            g_usb_present = now;
            g_ext_free_valid[EXT_USB] = false;
            if(now) {
                ui::UiMsgProxy::instance().send_storage_usage(middleware::StorageVolume::UsbDisk);
            }
            on_ext_presence_changed();
        });

    /* SD 卡热插拔：后端**未实现**，目前不会触发；接好线，后端一上来就能用。单槽，只有本页挂。 */
    ui::UiMsgProxy::instance().register_sdcard_hotplug(
        [](const middleware::UsbHotplugPayload &p) {
            const bool was = ext_present(EXT_SD);
            g_sd_hotplug = (p.present != 0 && p.mounted != 0);
            if(ext_present(EXT_SD) != was) {
                g_ext_free_valid[EXT_SD] = false;
                on_ext_presence_changed();
            }
        });

    /* 格式化进度 / 终态：后端**未实现**，这两个回调目前不会被触发；先接好线，
     * 后端一上来就能用。见 doc/STORAGE_BACKEND_API.md。 */
    ui::UiMsgProxy::instance().register_storage_format_progress(
        [](const middleware::StorageFormatProgressPayload &p) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Formatting... %u%%",
                          static_cast<unsigned>(p.percent));
            ui_toast_show(buf, UI_TOAST_INFORM, 1000);
        });
    ui::UiMsgProxy::instance().register_storage_format_result(
        [](const middleware::StorageFormatRspPayload &p) {
            if(p.result == 0) {
                ui_toast_show("Format complete", UI_TOAST_SUCCESS, 2000);
                if(g_selected == NAV_STORAGE) {
                    ui::UiMsgProxy::instance().send_storage_usage(
                        static_cast<middleware::StorageVolume>(p.volume));
                }
            } else {
                ui_toast_show("Format failed", UI_TOAST_ERROR, 2500);
            }
        });

    /* 连接结果：关闭"连接中"模态，成功轻提示；失败弹"连接失败"（如密码错/超时）。 */
    ui::UiMsgProxy::instance().register_wifi_connect_result(
        [](bool ok, const char * /*ssid*/) {
            close_connecting_dialog();
            if(ok) {
                ui_toast_show("Connected", UI_TOAST_SUCCESS, 1500);
            } else {
                ui_toast_show("Connect failed", UI_TOAST_ERROR, 2500);
            }
        });

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 主体：左子导航 + 右详情（本页不显示外壳的设备信息条，见 page_manager） */
    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* 稿上两栏在内容区 1152 里正好闭合，**内距全落在各自栏里**、body 本身零内距：
     *   左栏渐变带 364（= 32 + 300 + 32，见 build_subnav）
     *   右栏 788（= 64 + 660 + 64），起点 x=492 正是渐变带的右缘 */
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    build_subnav(body);

    g_detail = lv_obj_create(body);
    lv_obj_remove_style_all(g_detail);
    lv_obj_set_height(g_detail, LV_PCT(100));
    lv_obj_set_flex_grow(g_detail, 1);
    lv_obj_set_flex_flow(g_detail, LV_FLEX_FLOW_COLUMN);
    /* 右栏内距 56/64/24：顶内距把标题行盒顶放在 y=56（见 SET_HEAD_TOP）；页头与分区的
     * 间距由 make_page 返回的内容列自己管，这一层不设行距。内容超出一屏的页（Wi-Fi、
     * 存储、关于、Advanced Print）纵向滚动，装得下的页拖不动。 */
    const bool spec = (USER_SETTINGS_NAV_W > 0);
    lv_obj_set_style_pad_top(g_detail, spec ? SET_HEAD_TOP : USER_PAD, 0);
    lv_obj_set_style_pad_hor(g_detail, spec ? USER_PAD_LG * 2 : USER_PAD, 0);
    lv_obj_set_style_pad_bottom(g_detail, USER_PAD, 0);
    lv_obj_set_style_pad_row(g_detail, 0, 0);
    lv_obj_set_style_bg_opa(g_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_detail, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_detail, LV_SCROLLBAR_MODE_OFF);

    update_nav_highlight();
    show_detail(NAV_DEVICE);

    return page;
}
