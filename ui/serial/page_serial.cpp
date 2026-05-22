#include "page_serial.hpp"
#include "ui_theme.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>

static int g_serial_fd = -1;
static lv_obj_t *g_kb = nullptr;
static lv_obj_t *g_recv_ta = nullptr;
static lv_obj_t *g_send_ta = nullptr;
static lv_obj_t *g_dev_ta = nullptr;
static lv_obj_t *g_baud_dd = nullptr;
static lv_obj_t *g_status_lbl = nullptr;
static lv_obj_t *g_btn_connect = nullptr;
static lv_timer_t *g_read_timer = nullptr;

static const int k_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

static speed_t baud_to_speed(int baud)
{
    switch(baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

static void serial_read_cb(lv_timer_t *)
{
    if(g_serial_fd < 0) return;

    char buf[256];
    ssize_t n = read(g_serial_fd, buf, sizeof(buf) - 1);
    if(n > 0) {
        buf[n] = '\0';
        lv_textarea_add_text(g_recv_ta, buf);

        const char *txt = lv_textarea_get_text(g_recv_ta);
        size_t len = strlen(txt);
        if(len > 8192) {
            lv_textarea_set_text(g_recv_ta, txt + (len - 4096));
        }
    }
}

static void serial_connect_cb(lv_event_t *)
{
    if(g_serial_fd >= 0) {
        lv_timer_pause(g_read_timer);
        close(g_serial_fd);
        g_serial_fd = -1;
        lv_label_set_text(g_status_lbl, "Disconnected");
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_accent_dim, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Connect");
    }
    else {
        const char *dev = lv_textarea_get_text(g_dev_ta);
        if(!dev || !dev[0]) {
            lv_label_set_text(g_status_lbl, "Empty device");
            return;
        }

        int fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
        if(fd < 0) {
            lv_label_set_text(g_status_lbl, "Open failed");
            return;
        }

        struct termios tty;
        tcgetattr(fd, &tty);

        uint16_t sel = lv_dropdown_get_selected(g_baud_dd);
        int baud = (sel < 8) ? k_bauds[sel] : 115200;
        cfsetispeed(&tty, baud_to_speed(baud));
        cfsetospeed(&tty, baud_to_speed(baud));

        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_oflag &= ~OPOST;
        tcsetattr(fd, TCSANOW, &tty);

        fcntl(fd, F_SETFL, FNDELAY);

        g_serial_fd = fd;
        lv_timer_resume(g_read_timer);
        lv_label_set_text(g_status_lbl, "Connected");
        lv_obj_set_style_bg_color(g_btn_connect, ui_color_danger, 0);
        lv_label_set_text(lv_obj_get_child(g_btn_connect, 0), "Disconnect");
    }
}

static void serial_send_cb(lv_event_t *)
{
    if(g_serial_fd < 0) return;

    const char *text = lv_textarea_get_text(g_send_ta);
    if(text && text[0]) {
        write(g_serial_fd, text, strlen(text));
        write(g_serial_fd, "\r\n", 2);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_keyboard_set_textarea(g_kb, ta);
    lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

lv_obj_t *page_serial_create(lv_obj_t *parent)
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

    lv_obj_t *dev_lbl = lv_label_create(cfg_row);
    lv_label_set_text(dev_lbl, "Device:");
    lv_obj_set_style_text_color(dev_lbl, ui_color_text, 0);

    g_dev_ta = lv_textarea_create(cfg_row);
    lv_textarea_set_one_line(g_dev_ta, true);
    lv_textarea_set_text(g_dev_ta, "/dev/ttyUSB0");
    lv_obj_set_width(g_dev_ta, 140);
    lv_obj_add_event_cb(g_dev_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *baud_lbl = lv_label_create(cfg_row);
    lv_label_set_text(baud_lbl, "Baud:");
    lv_obj_set_style_text_color(baud_lbl, ui_color_text, 0);

    g_baud_dd = lv_dropdown_create(cfg_row);
    lv_dropdown_set_options(g_baud_dd, "9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600");
    lv_dropdown_set_selected(g_baud_dd, 4);
    lv_obj_set_width(g_baud_dd, 80);

    g_btn_connect = lv_button_create(cfg_row);
    lv_obj_set_size(g_btn_connect, 72, 36);
    lv_obj_add_style(g_btn_connect, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_connect, serial_connect_cb, LV_EVENT_CLICKED, NULL);
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
    lv_obj_add_event_cb(btn_send, serial_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_btn_lbl = lv_label_create(btn_send);
    lv_label_set_text(send_btn_lbl, "Send");
    lv_obj_center(send_btn_lbl);

    /* ---------- 触摸键盘 ---------- */
    g_kb = lv_keyboard_create(page);
    lv_obj_set_width(g_kb, LV_PCT(100));
    lv_obj_set_height(g_kb, 200);
    lv_keyboard_set_mode(g_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(g_kb, g_send_ta);

    /* ---------- 定时轮询 ---------- */
    g_read_timer = lv_timer_create(serial_read_cb, 50, NULL);
    lv_timer_pause(g_read_timer);

    return page;
}
