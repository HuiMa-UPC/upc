#include "page_user_settings.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "page_manager.hpp"
#include "settings_icons.h"
#include "ui_msg_proxy.hpp"
#include "msg_types.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

/**
 * @file page_user_settings.cpp
 * @brief 设置页（两栏：左侧子导航 + 右侧详情），对齐设计稿。
 *
 * 布局：
 *   ┌──────────────────────────────────────────────────────┐
 *   │ [WiFi] SNOWPOD A1 ● Ready                  (header)    │
 *   ├───────────────────────┬──────────────────────────────┤
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
 *   - Wi-Fi 列表为静态假数据；连接动作仅弹密码框 + toast（stub）。
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

/* 可选 Wi-Fi 静态假数据（真实扫描由后端接） */
const char *const kCurrentWifi = "SnowX_WIFI_2.4G";
const char *const kAvailWifi[] = {
    "SnowX_WIFI_2.4G",
    "Sweden_2.4G",
    "Sweden_5G",
    "Guest_2.4G",
    "Lab_Internal",
};

lv_obj_t *g_nav_btns[NAV_COUNT]  = { nullptr };
lv_obj_t *g_nav_icons[NAV_COUNT] = { nullptr };
lv_obj_t *g_detail     = nullptr;   /**< 右侧详情容器（按选中重建） */
lv_obj_t *g_wifi_list  = nullptr;   /**< 翻页按钮的滚动目标（网络详情=整个详情容器） */
lv_obj_t *g_page       = nullptr;   /**< 设置页根：翻页按钮悬浮父级（整屏居中用） */
lv_obj_t *g_scroll_btns= nullptr;   /**< Wi-Fi 翻页按钮悬浮层（仅网络详情存在） */
int       g_selected   = NAV_NETWORK;

/* ---- 弹窗（密码 / Wi-Fi 连接，复用同一句柄，互斥打开） ---- */
ui_modal_dialog_t *g_dlg     = nullptr;
lv_obj_t          *g_pwd_ta  = nullptr;
lv_obj_t          *g_pwd_err = nullptr;

ui_modal_dialog_t *g_ota_dlg = nullptr;
lv_obj_t *g_ota_status_lbl = nullptr;
lv_obj_t *g_ota_bar = nullptr;
bool g_ota_proxy_wired = false;

struct OtaUiUpdate {
    middleware::McuOtaStatusPayload status;
};

void ota_close_dialog(void);

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
    if(g_ota_status_lbl) {
        char buf[96];
        const int pct = (upd->status.progress * 100) / 255;
        std::snprintf(buf, sizeof(buf), "%s (%d%%)",
                      ota_state_text(upd->status.state), pct);
        lv_label_set_text(g_ota_status_lbl, buf);
    }
    if(g_ota_bar) {
        const int pct = (upd->status.progress * 100) / 255;
        lv_bar_set_value(g_ota_bar, pct, LV_ANIM_OFF);
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
            upd->status = st;
            lv_async_call(ota_ui_apply_async, upd);
        });
}

void on_ota_close(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_mcu_ota_cancel();
    ota_close_dialog();
}

void open_ota_dialog(void)
{
    if(g_ota_dlg) {
        return;
    }
    wire_ota_proxy_once();

    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("MCU OTA (Test)", USER_DLG_W, false);
    cfg.with_close_btn = true;
    g_ota_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_ota_dlg) {
        return;
    }
    if(g_ota_dlg->close_btn) {
        lv_obj_add_event_cb(g_ota_dlg->close_btn, on_ota_close,
                            LV_EVENT_CLICKED, nullptr);
    }

    g_ota_status_lbl = ui_user_make_body_label(g_ota_dlg->body, "Starting...");
    g_ota_bar = ui_user_make_progress_bar(
        g_ota_dlg->body, LV_PCT(100), USER_PROGRESS_H);
    lv_bar_set_value(g_ota_bar, 0, LV_ANIM_OFF);
}

void ota_close_dialog(void)
{
    if(g_ota_dlg) {
        ui_modal_dialog_close(g_ota_dlg);
        g_ota_dlg         = nullptr;
        g_ota_status_lbl  = nullptr;
        g_ota_bar         = nullptr;
    }
}

void on_ota_entry_click(lv_event_t *)
{
    open_ota_dialog();
    ui::UiMsgProxy::instance().send_mcu_ota_start();
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

    lv_obj_t *btn_ok = lv_button_create(g_dlg->footer);
    lv_obj_add_style(btn_ok, user_style_btn_primary(), 0);
    lv_obj_set_size(btn_ok, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    lv_obj_add_event_cb(btn_ok, on_dev_confirm, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ok_lbl = lv_label_create(btn_ok);
    lv_label_set_text(ok_lbl, "Enter");
    lv_obj_center(ok_lbl);
}

/* ---- Wi-Fi 连接密码弹窗（stub） ---- */
void on_wifi_connect_confirm(lv_event_t *)
{
    dlg_close();
    ui_toast_show("Connecting...", user_color_accent, 1500);
}

void open_wifi_connect_dialog(const char *ssid)
{
    if(g_dlg) {
        return;
    }
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

    lv_obj_t *btn_ok = lv_button_create(g_dlg->footer);
    lv_obj_add_style(btn_ok, user_style_btn_primary(), 0);
    lv_obj_set_size(btn_ok, USER_DLG_FOOTER_BTN_W, USER_DLG_FOOTER_BTN_H);
    lv_obj_add_event_cb(btn_ok, on_wifi_connect_confirm, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ok_lbl = lv_label_create(btn_ok);
    lv_label_set_text(ok_lbl, "Connect");
    lv_obj_center(ok_lbl);
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
    open_wifi_connect_dialog(ssid);
}

/** Wi-Fi 列表项卡片：图标 + SSID + 右箭头；点击弹连接框 */
lv_obj_t *make_wifi_item(lv_obj_t *parent, const char *ssid)
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
    lv_obj_add_event_cb(btn, on_wifi_item_click, LV_EVENT_CLICKED,
                        const_cast<char *>(ssid));

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(icon, user_font_text(), 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, ssid);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, user_font_caption(), 0);
    lv_obj_set_style_text_color(arrow, user_color_text_dim, 0);
    return btn;
}

/* ---- 详情面板 ---- */
void on_wifi_switch_changed(lv_event_t *e)
{
    lv_obj_t *seg = lv_event_get_target_obj(e);
    const uint8_t sel = ui_user_segmented_get_selected(seg);
    ui_toast_show(sel == 0 ? "Wi-Fi On" : "Wi-Fi Off", user_color_accent, 1200);
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

void build_network_detail(lv_obj_t *parent)
{
    /*
     * 整块内容（Wi-Fi 开关 / 当前 Wi-Fi / 可选 Wi-Fi）作为一个纵向滚动区一起滚动；
     * 翻页按钮另做成相对整屏垂直居中的悬浮层（见函数末尾），不随内容滚动。
     */
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_right(parent, USER_BACK_BTN_H * 5 / 4 + USER_PAD, 0); /* 给悬浮翻页按钮留位 */
    g_wifi_list = parent;   /* 翻页按钮滚动整个详情 */

    make_caption(parent, "Wi-Fi");

    const char *const sw_labels[] = { "On", "Off" };
    lv_obj_t *sw = ui_user_make_segmented(parent, sw_labels, 2, 0);
    lv_obj_add_event_cb(sw, on_wifi_switch_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    /*
     * 分段控件的段默认 flex_grow=1，需父级定宽才撑得开；此处按内容宽度摆放（未定宽），
     * grow 会把 On/Off 压成 0 宽导致开关不显示。关掉各段 grow，按文字宽度自适应。
     */
    for(uint32_t i = 0; i < lv_obj_get_child_count(sw); i++) {
        lv_obj_set_flex_grow(lv_obj_get_child(sw, i), 0);
    }

    make_caption(parent, "Current Wi-Fi");
    make_wifi_item(parent, kCurrentWifi);

    make_caption(parent, "Available Wi-Fi");
    const size_t n = sizeof(kAvailWifi) / sizeof(kAvailWifi[0]);
    for(size_t i = 0; i < n; i++) {
        make_wifi_item(parent, kAvailWifi[i]);
    }

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
}

void build_about_detail(lv_obj_t *parent)
{
    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    struct AboutRow { const char *label; const char *value; };
    const AboutRow rows[] = {
        { "Software Version", UPC_SW_VERSION },
        { "Device Model",     "SNOWPOD A1"   },
    };
    for(const AboutRow &r : rows) {
        lv_obj_t *row = ui_user_make_setting_row(card, r.label, nullptr, nullptr);
        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, r.value);
        lv_obj_set_style_text_color(val, user_color_text_dim, 0);
        lv_obj_set_style_text_font(val, user_font_text(), 0);
    }
}

void build_device_detail(lv_obj_t *parent)
{
    lv_obj_t *card = ui_user_make_frosted_card(parent, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    ui_user_make_setting_row(card, "MCU OTA (Test)", "Start", on_ota_entry_click);
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
    lv_obj_clean(g_detail);
    /* 复位详情容器：默认不滚动、无右内边距（网络详情会按需开启） */
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_right(g_detail, 0, 0);

    switch(id) {
    case NAV_NETWORK: build_network_detail(g_detail); break;
    case NAV_DEVICE:  build_device_detail(g_detail);  break;
    case NAV_ABOUT:   build_about_detail(g_detail);   break;
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
    /* Debug：保留密码门，通过后进调试页（不切换右侧详情） */
    if(id == NAV_DEBUG) {
        open_dev_password_dialog();
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

    const int32_t icon_sz = lv_font_get_line_height(user_font_text());
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

lv_obj_t *build_header(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_TOPBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_column(bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wifi = lv_label_create(bar);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi, user_color_text, 0);
    lv_obj_set_style_text_font(wifi, user_font_text(), 0);

    lv_obj_t *name = lv_label_create(bar);
    lv_label_set_text(name, "SNOWPOD A1");
    lv_obj_set_style_text_color(name, user_color_text, 0);
    lv_obj_set_style_text_font(name, user_font_text(), 0);

    lv_obj_t *dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, USER_STATUS_DOT_SZ, USER_STATUS_DOT_SZ);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, user_color_status_ok, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(dot, USER_PAD_SM, 0);

    lv_obj_t *status = lv_label_create(bar);
    lv_label_set_text(status, "Ready");
    lv_obj_set_style_text_color(status, user_color_status_ok, 0);
    lv_obj_set_style_text_font(status, user_font_text(), 0);

    return bar;
}

} /* namespace */

void page_user_settings_refresh(bool /*force*/)
{
    /* 静态内容，无需周期刷新 */
}

void page_user_settings_dismiss_dialog(void)
{
    dlg_close();
    ota_close_dialog();
}

lv_obj_t *page_user_settings_create(lv_obj_t *parent)
{
    g_selected   = NAV_NETWORK;
    g_wifi_list  = nullptr;
    g_page       = nullptr;
    g_scroll_btns = nullptr;
    for(int i = 0; i < NAV_COUNT; i++) {
        g_nav_btns[i]  = nullptr;
        g_nav_icons[i] = nullptr;
    }

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_page = page;   /* 翻页按钮悬浮层挂这里，整屏居中 */

    build_header(page);

    /* 主体：左子导航 + 右详情 */
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
