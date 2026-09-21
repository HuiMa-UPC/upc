#include "ui_sys_hud.hpp"

#include "lvgl_wrap.hpp"
#include "ui_theme.hpp"
#include "upc_sys_monitor.hpp"

#include <cstdio>
#include <cstddef>

namespace {

lv_obj_t  *g_lbl   = nullptr;
lv_timer_t *g_timer = nullptr;
bool        g_on    = false;

void format_line(char *buf, size_t n, const UpcSysMonitorSample &s)
{
    char temp[24];
    if(s.cpu_temp_c >= 0.f) {
        std::snprintf(temp, sizeof(temp), "%.1fC", static_cast<double>(s.cpu_temp_c));
    } else {
        std::snprintf(temp, sizeof(temp), "N/A");
    }

    char cpu[40];
    if(s.cpu_sys_pct >= 0.f) {
        std::snprintf(cpu, sizeof(cpu), "CPU %.0f%% ui %.0f%%",
                      static_cast<double>(s.cpu_sys_pct),
                      static_cast<double>(s.cpu_proc_pct));
    } else {
        std::snprintf(cpu, sizeof(cpu), "CPU --");
    }

    const unsigned long used_mb =
        (s.mem_total_kb > s.mem_avail_kb)
            ? (s.mem_total_kb - s.mem_avail_kb) / 1024UL
            : 0UL;
    const unsigned long total_mb = s.mem_total_kb / 1024UL;
    const unsigned long rss_mb   = s.rss_kb / 1024UL;

    std::snprintf(buf, n, "%s\n%s\nMEM %lu/%luMB rss %luMB",
                  temp, cpu, used_mb, total_mb, rss_mb);
}

void refresh(lv_timer_t * /*t*/)
{
    if(!g_lbl) {
        return;
    }
    UpcSysMonitorSample s{};
    if(!upc_sys_monitor_sample(&s)) {
        lv_label_set_text(g_lbl, "sys N/A");
        return;
    }
    char line[160];
    format_line(line, sizeof(line), s);
    lv_label_set_text(g_lbl, line);
}

void destroy_hud()
{
    if(g_timer) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    if(g_lbl) {
        lv_obj_delete(g_lbl);
        g_lbl = nullptr;
    }
}

void create_hud()
{
    if(g_lbl) {
        return;
    }
    lv_obj_t *sys = lv_layer_sys();
    if(!sys) {
        return;
    }

    /* 纯红字蒙版：无底、无边、不可点——与 LVGL 自带 sysmon 同层，压在一切业务 UI 之上。 */
    g_lbl = lv_label_create(sys);
    lv_obj_remove_flag(g_lbl, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_bg_opa(g_lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_lbl, 0, 0);
    lv_obj_set_style_pad_all(g_lbl, 0, 0);
    lv_obj_set_style_text_color(g_lbl, lv_color_hex(0xFF2020), 0);
    lv_obj_set_style_text_font(g_lbl, user_font_caption(), 0);
    lv_obj_set_style_text_align(g_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(g_lbl, "...");
    lv_obj_align(g_lbl, LV_ALIGN_TOP_RIGHT, -USER_PAD, USER_PAD);

    g_timer = lv_timer_create(refresh, 1000, nullptr);
    refresh(nullptr); /* 立刻采一轮，避免空白等 1s */
}

} // namespace

extern "C" {

void ui_sys_hud_set_enabled(bool enabled)
{
    if(enabled == g_on) {
        return;
    }
    g_on = enabled;
    if(enabled) {
        create_hud();
    } else {
        destroy_hud();
    }
}

bool ui_sys_hud_enabled(void)
{
    return g_on;
}

} // extern "C"
