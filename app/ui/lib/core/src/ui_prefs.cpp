#include "ui_prefs.hpp"

#include "debug_msg_proxy.hpp"
#include "lvgl_wrap.hpp"
#include "msg_port.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifndef UPC_UI_PREFS_DIR
#define UPC_UI_PREFS_DIR   "/userdata/ui"
#endif
#ifndef UPC_UI_PREFS_PATH
#define UPC_UI_PREFS_PATH  UPC_UI_PREFS_DIR "/prefs.conf"
#endif

namespace {

struct Prefs {
    bool auto_clean     = true;
    bool auto_timelapse = true;
    bool auto_record    = true;
    ui_record_storage_t record_storage = UI_RECORD_STORAGE_NONE;
    bool ux_program     = true;
};

Prefs       g_prefs;
bool        g_inited    = false;
bool        g_glavo_up  = false;   /**< 上一轮轮询时 Glavo 是否可达 */
lv_timer_t *g_sync_timer = nullptr;

constexpr uint32_t kGlavoPollMs = 1000;

const char *storage_text(ui_record_storage_t s)
{
    switch(s) {
    case UI_RECORD_STORAGE_USB: return "usb";
    case UI_RECORD_STORAGE_SD:  return "sd";
    default:                    return "none";
    }
}

ui_record_storage_t storage_from_text(const char *v)
{
    if(std::strcmp(v, "usb") == 0) return UI_RECORD_STORAGE_USB;
    if(std::strcmp(v, "sd") == 0)  return UI_RECORD_STORAGE_SD;
    return UI_RECORD_STORAGE_NONE;
}

void apply_kv(const char *key, const char *val)
{
    const bool on = (val[0] == '1');
    if(std::strcmp(key, "auto_clean") == 0)          g_prefs.auto_clean = on;
    else if(std::strcmp(key, "auto_timelapse") == 0) g_prefs.auto_timelapse = on;
    else if(std::strcmp(key, "auto_record") == 0)    g_prefs.auto_record = on;
    else if(std::strcmp(key, "record_storage") == 0) g_prefs.record_storage = storage_from_text(val);
    else if(std::strcmp(key, "ux_program") == 0)     g_prefs.ux_program = on;
    /* 认不得的键忽略：老版本读新文件时不至于出错 */
}

void load(void)
{
    FILE *f = std::fopen(UPC_UI_PREFS_PATH, "r");
    if(!f) {
        return;   /* 首次运行没有文件：全按默认值 */
    }
    char line[128];
    while(std::fgets(line, sizeof(line), f)) {
        line[std::strcspn(line, "\r\n")] = '\0';
        char *eq = std::strchr(line, '=');
        if(!eq || line[0] == '#') {
            continue;
        }
        *eq = '\0';
        apply_kv(line, eq + 1);
    }
    std::fclose(f);
}

/** 整份重写：先写临时文件再 rename，断电时不会留下半截文件。写失败只是这次没落盘，
 *  内存里的值照常生效，下次改值时再写。 */
void save(void)
{
    std::error_code ec;
    std::filesystem::create_directories(UPC_UI_PREFS_DIR, ec);
    const std::string tmp = std::string(UPC_UI_PREFS_PATH) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if(!f) {
        return;
    }
    std::fprintf(f, "auto_clean=%d\n", g_prefs.auto_clean ? 1 : 0);
    std::fprintf(f, "auto_timelapse=%d\n", g_prefs.auto_timelapse ? 1 : 0);
    std::fprintf(f, "auto_record=%d\n", g_prefs.auto_record ? 1 : 0);
    std::fprintf(f, "record_storage=%s\n", storage_text(g_prefs.record_storage));
    std::fprintf(f, "ux_program=%d\n", g_prefs.ux_program ? 1 : 0);
    const bool ok = (std::fflush(f) == 0);
    std::fclose(f);
    if(ok) {
        std::rename(tmp.c_str(), UPC_UI_PREFS_PATH);
    }
}

bool glavo_reachable(void)
{
    return middleware::MsgPort::instance().peer_connected(middleware::ModuleId::APP_GLAVO);
}

/** Glavo 的 Print H264 Rec 每次启动都回到上电默认，工程里也没有「对端连上」事件，只能轮询：
 *  由断到连就把落盘值补发一次，Glavo 中途重启同样覆盖。 */
void on_glavo_poll(lv_timer_t * /*t*/)
{
    const bool up = glavo_reachable();
    if(up && !g_glavo_up) {
        ui::DebugMsgProxy::instance().send_print_h264_record_set(g_prefs.auto_record);
    }
    g_glavo_up = up;
}

}  // namespace

extern "C" {

void ui_prefs_init(void)
{
    if(g_inited) {
        return;
    }
    g_inited = true;
    load();
    g_sync_timer = lv_timer_create(on_glavo_poll, kGlavoPollMs, nullptr);
    on_glavo_poll(g_sync_timer);
}

bool ui_prefs_auto_clean(void) { return g_prefs.auto_clean; }

void ui_prefs_set_auto_clean(bool on)
{
    g_prefs.auto_clean = on;
    save();
}

bool ui_prefs_auto_timelapse(void) { return g_prefs.auto_timelapse; }

void ui_prefs_set_auto_timelapse(bool on)
{
    g_prefs.auto_timelapse = on;
    save();
}

bool ui_prefs_auto_record(void) { return g_prefs.auto_record; }

void ui_prefs_set_auto_record(bool on)
{
    g_prefs.auto_record = on;
    save();
    /* Glavo 不可达时这一条会丢，等轮询发现它连上再补发 */
    ui::DebugMsgProxy::instance().send_print_h264_record_set(on);
}

ui_record_storage_t ui_prefs_record_storage(void) { return g_prefs.record_storage; }

void ui_prefs_set_record_storage(ui_record_storage_t storage)
{
    g_prefs.record_storage = storage;
    save();
}

bool ui_prefs_ux_program(void) { return g_prefs.ux_program; }

void ui_prefs_set_ux_program(bool on)
{
    g_prefs.ux_program = on;
    save();
}

}  // extern "C"
