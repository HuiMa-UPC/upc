#include "page_tcpclient.hpp"
#include "ui_theme.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>

static int g_tcp_fd = -1;
static lv_obj_t *g_kb = nullptr;
static lv_obj_t *g_recv_ta = nullptr;
static lv_obj_t *g_send_ta = nullptr;
static lv_obj_t *g_ip_ta = nullptr;
static lv_obj_t *g_port_ta = nullptr;
static lv_obj_t *g_status_lbl = nullptr;
static lv_obj_t *g_btn_connect = nullptr;
static lv_timer_t *g_read_timer = nullptr;

static void tcp_read_cb(lv_timer_t *)
{
    if(g_tcp_fd < 0) return;

    char buf[256];
    ssize_t n = recv(g_tcp_fd, buf, sizeof(buf) - 1, 0);
    if(n > 0) {
        buf[n] = '\0';
        lv_textarea_add_text(g_recv_ta, buf);

        const char *txt = lv_textarea_get_text(g_recv_ta);
        size_t len = strlen(txt);
        if(len > 8192) {
            lv_textarea_set_text(g_recv_ta, txt + (len - 4096));
        }
    }
    else if(n == 0) {
        close(g_tcp_fd);
        g_tcp_fd = -1;
        lv_timer_pause(g_read_timer);
        lv_label_set_text(g_status_lbl, "Remote closed");
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_accent_dim, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Connect");
    }
}

static void tcp_connect_cb(lv_event_t *)
{
    if(g_tcp_fd >= 0) {
        lv_timer_pause(g_read_timer);
        close(g_tcp_fd);
        g_tcp_fd = -1;
        lv_label_set_text(g_status_lbl, "Disconnected");
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_accent_dim, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Connect");
    }
    else {
        const char *ip = lv_textarea_get_text(g_ip_ta);
        const char *port_str = lv_textarea_get_text(g_port_ta);
        if(!ip || !ip[0] || !port_str || !port_str[0]) {
            lv_label_set_text(g_status_lbl, "Empty IP/Port");
            return;
        }

        int port = atoi(port_str);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            lv_label_set_text(g_status_lbl, "Socket fail");
            return;
        }

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if(inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
            close(fd);
            lv_label_set_text(g_status_lbl, "Invalid IP");
            return;
        }

        if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            lv_label_set_text(g_status_lbl, "Connect failed");
            return;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        g_tcp_fd = fd;
        lv_timer_resume(g_read_timer);
        lv_label_set_text(g_status_lbl, "Connected");
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_danger, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Disconnect");
    }
}

static void tcp_send_cb(lv_event_t *)
{
    if(g_tcp_fd < 0) return;

    const char *text = lv_textarea_get_text(g_send_ta);
    if(text && text[0]) {
        send(g_tcp_fd, text, strlen(text), 0);
        send(g_tcp_fd, "\r\n", 2, 0);
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

lv_obj_t *page_tcp_create(lv_obj_t *parent)
{
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
    lv_obj_set_width(g_ip_ta, 220);
    lv_obj_add_event_cb(g_ip_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *port_lbl = lv_label_create(cfg_row);
    lv_label_set_text(port_lbl, "Port:");
    lv_obj_set_style_text_color(port_lbl, ui_color_text, 0);

    g_port_ta = lv_textarea_create(cfg_row);
    lv_textarea_set_one_line(g_port_ta, true);
    lv_textarea_set_text(g_port_ta, "8080");
    lv_obj_set_width(g_port_ta, 120);
    lv_obj_add_event_cb(g_port_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    g_btn_connect = lv_button_create(cfg_row);
    lv_obj_set_size(g_btn_connect, 120, 48);
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
    lv_obj_set_size(btn_send, 100, 48);
    lv_obj_add_style(btn_send, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn_send, tcp_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_btn_lbl = lv_label_create(btn_send);
    lv_label_set_text(send_btn_lbl, "Send");
    lv_obj_center(send_btn_lbl);

    /* ---------- 触摸键盘 ---------- */
    g_kb = lv_keyboard_create(page);
    lv_obj_set_width(g_kb, LV_PCT(100));
    lv_obj_set_height(g_kb, 240);
    lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(g_kb, g_send_ta);

    /* ---------- 定时轮询 ---------- */
    g_read_timer = lv_timer_create(tcp_read_cb, 50, NULL);
    lv_timer_pause(g_read_timer);

    return page;
}
