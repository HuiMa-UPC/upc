#include "page_tcpclient.hpp"
#include "tcpclient_backend.hpp"
#include "ui_theme.hpp"

#include <cstring>
#include <cstdlib>

/* ---------- 前端 UI 控件 ---------- */
static lv_obj_t *g_kb = nullptr;
static lv_obj_t *g_recv_ta = nullptr;
static lv_obj_t *g_send_ta = nullptr;
static lv_obj_t *g_ip_ta = nullptr;
static lv_obj_t *g_port_ta = nullptr;
static lv_obj_t *g_status_lbl = nullptr;
static lv_obj_t *g_btn_connect = nullptr;

/* ---------- 内部辅助 ---------- */
static void update_connect_btn(bool connected)
{
    if(connected) {
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_danger, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Disconnect");
    }
    else {
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_accent_dim, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Connect");
    }
}

/* ---------- 后端回调 -> 更新 UI ---------- */
static void on_tcp_state_changed(tcp_state_t state, const char *msg, void *)
{
    lv_label_set_text(g_status_lbl, msg ? msg : "");

    switch(state) {
        case TCP_STATE_CONNECTED:
            update_connect_btn(true);
            break;
        case TCP_STATE_DISCONNECTED:
        case TCP_STATE_REMOTE_CLOSED:
        case TCP_STATE_ERROR:
            update_connect_btn(false);
            break;
    }
}

static void on_tcp_data_received(const char *data, size_t /*len*/, void *)
{
    lv_textarea_add_text(g_recv_ta, data);

    const char *txt = lv_textarea_get_text(g_recv_ta);
    size_t len = strlen(txt);
    if(len > 8192) {
        lv_textarea_set_text(g_recv_ta, txt + (len - 4096));
    }
}

/* ---------- UI 事件回调 ---------- */
static void tcp_connect_cb(lv_event_t *)
{
    if(tcp_backend_is_connected()) {
        tcp_backend_disconnect();
    }
    else {
        const char *ip = lv_textarea_get_text(g_ip_ta);
        const char *port_str = lv_textarea_get_text(g_port_ta);
        if(!ip || !ip[0] || !port_str || !port_str[0]) {
            lv_label_set_text(g_status_lbl, "Empty IP/Port");
            return;
        }
        int port = atoi(port_str);
        tcp_backend_connect(ip, port);
    }
}

static void tcp_send_cb(lv_event_t *)
{
    const char *text = lv_textarea_get_text(g_send_ta);
    if(text && text[0]) {
        tcp_backend_send(text, strlen(text));
        tcp_backend_send("\r\n", 2);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_keyboard_set_textarea(g_kb, ta);
    if(ta == g_port_ta) {
        lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_NUMBER);
    }
    else {
        lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
}

/* ---------- 页面创建 ---------- */
lv_obj_t *page_tcp_create(lv_obj_t *parent)
{
    /* 初始化后端并注册回调 */
    tcp_backend_init();
    tcp_backend_set_state_cb(on_tcp_state_changed, nullptr);
    tcp_backend_set_data_cb(on_tcp_data_received, nullptr);

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);

    /* ---------- 配置栏 ---------- */
    lv_obj_t *cfg_row = lv_obj_create(page);
    lv_obj_remove_style_all(cfg_row);
    lv_obj_set_width(cfg_row, LV_PCT(100));
    lv_obj_set_height(cfg_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cfg_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cfg_row, UI_PAD_SM, 0);
    lv_obj_set_flex_align(cfg_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ip_lbl = lv_label_create(cfg_row);
    lv_label_set_text(ip_lbl, "IP:");
    lv_obj_set_style_text_color(ip_lbl, ui_color_text, 0);

    g_ip_ta = lv_textarea_create(cfg_row);
    lv_textarea_set_one_line(g_ip_ta, true);
    lv_textarea_set_text(g_ip_ta, "192.168.1.100");
    lv_obj_set_width(g_ip_ta, 130);
    lv_obj_add_event_cb(g_ip_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *port_lbl = lv_label_create(cfg_row);
    lv_label_set_text(port_lbl, "Port:");
    lv_obj_set_style_text_color(port_lbl, ui_color_text, 0);

    g_port_ta = lv_textarea_create(cfg_row);
    lv_textarea_set_one_line(g_port_ta, true);
    lv_textarea_set_text(g_port_ta, "8080");
    lv_obj_set_width(g_port_ta, 70);
    lv_obj_add_event_cb(g_port_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    g_btn_connect = lv_button_create(cfg_row);
    lv_obj_set_size(g_btn_connect, 72, 36);
    lv_obj_add_style(g_btn_connect, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_connect, tcp_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(g_btn_connect);
    lv_label_set_text(btn_lbl, "Connect");
    lv_obj_center(btn_lbl);

    g_status_lbl = lv_label_create(cfg_row);
    lv_label_set_text(g_status_lbl, "Disconnected");
    lv_obj_set_style_text_color(g_status_lbl, ui_color_text_dim, 0);
    lv_obj_set_flex_grow(g_status_lbl, 1);

    /* ---------- 接收区 ---------- */
    lv_obj_t *recv_lbl = lv_label_create(page);
    lv_label_set_text(recv_lbl, "Received:");
    lv_obj_set_style_text_color(recv_lbl, ui_color_text, 0);

    g_recv_ta = lv_textarea_create(page);
    lv_obj_set_flex_grow(g_recv_ta, 1);
    lv_obj_set_width(g_recv_ta, LV_PCT(100));
    lv_textarea_set_placeholder_text(g_recv_ta, "Received data will appear here...");

    /* ---------- 发送栏 ---------- */
    lv_obj_t *send_row = lv_obj_create(page);
    lv_obj_remove_style_all(send_row);
    lv_obj_set_width(send_row, LV_PCT(100));
    lv_obj_set_height(send_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(send_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(send_row, UI_PAD_SM, 0);
    lv_obj_set_flex_align(send_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *send_lbl = lv_label_create(send_row);
    lv_label_set_text(send_lbl, "Send:");
    lv_obj_set_style_text_color(send_lbl, ui_color_text, 0);

    g_send_ta = lv_textarea_create(send_row);
    lv_textarea_set_one_line(g_send_ta, true);
    lv_textarea_set_placeholder_text(g_send_ta, "Type here...");
    lv_obj_set_flex_grow(g_send_ta, 1);
    lv_obj_add_event_cb(g_send_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *btn_send = lv_button_create(send_row);
    lv_obj_set_size(btn_send, 64, 36);
    lv_obj_add_style(btn_send, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn_send, tcp_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_btn_lbl = lv_label_create(btn_send);
    lv_label_set_text(send_btn_lbl, "Send");
    lv_obj_center(send_btn_lbl);

    /* ---------- 触摸键盘 ---------- */
    g_kb = lv_keyboard_create(page);
    lv_obj_set_width(g_kb, LV_PCT(100));
    lv_obj_set_height(g_kb, 200);
    lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(g_kb, g_send_ta);

    return page;
}
