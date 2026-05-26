#include "ui_shell.hpp"
#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "page_home.hpp"
#include "page_control.hpp"
#include "page_files.hpp"
#include "page_settings.hpp"
#include "page_serial.hpp"
#include "page_tcpclient.hpp"

#include <string.h>

typedef lv_obj_t *(*page_factory_t)(lv_obj_t *parent);

typedef struct {
    const char *icon;
    const char *label;
    page_factory_t factory;
} nav_item_def_t;

/** 全局 UI 刷新周期（仅刷新当前可见页） */
static constexpr uint32_t kShellRefreshMs = 200;

static const nav_item_def_t k_nav_defs[UI_PAGE_COUNT] = {
    { LV_SYMBOL_HOME,     "Home",     page_home_create },
    { LV_SYMBOL_PLAY,     "Control",  page_control_create },
    { LV_SYMBOL_FILE,     "Files",    page_files_create },
    { LV_SYMBOL_SETTINGS, "Settings", page_settings_create },
    { LV_SYMBOL_USB,      "Serial",   page_serial_create },
    { LV_SYMBOL_WIFI,     "WIFI",      page_tcp_create },
};

static lv_obj_t *g_root;
static lv_obj_t *g_sidebar;
static lv_obj_t *g_content;
static lv_obj_t *g_nav_btns[UI_PAGE_COUNT];
static lv_obj_t *g_pages[UI_PAGE_COUNT];
static ui_page_id_t g_active_page = UI_PAGE_HOME;
static bool g_page_transition_running = false;
static lv_obj_t *g_anim_out_page = nullptr;
static lv_obj_t *g_anim_in_page = nullptr;

static void shell_refresh_page(ui_page_id_t id, bool force);
static void style_nav_btn(lv_obj_t *btn, bool active);

static void page_anim_set_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0);
}

static void page_anim_set_x(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x(static_cast<lv_obj_t *>(obj), v, 0);
}

static void page_reset_visual(lv_obj_t *page)
{
    if(!page) {
        return;
    }
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(page, 0, 0);
}

static void page_transition_complete(lv_anim_t *)
{
    if(g_anim_out_page) {
        lv_obj_add_flag(g_anim_out_page, LV_OBJ_FLAG_HIDDEN);
        page_reset_visual(g_anim_out_page);
        g_anim_out_page = nullptr;
    }
    if(g_anim_in_page) {
        page_reset_visual(g_anim_in_page);
        g_anim_in_page = nullptr;
    }
    g_page_transition_running = false;
    shell_refresh_page(g_active_page, true);
}

static void page_show_instant(ui_page_id_t id)
{
    g_active_page = id;

    for(uint32_t i = 0; i < UI_PAGE_COUNT; i++) {
        if(g_pages[i]) {
            if(i == (uint32_t)id) {
                page_reset_visual(g_pages[i]);
                lv_obj_clear_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
                page_reset_visual(g_pages[i]);
            }
        }
        if(g_nav_btns[i]) {
            style_nav_btn(g_nav_btns[i], i == (uint32_t)id);
        }
    }

    page_files_set_visible(id == UI_PAGE_FILES ? 1 : 0);
    shell_refresh_page(id, true);
}

static void page_transition(ui_page_id_t from, ui_page_id_t to)
{
    lv_obj_t *out_page = g_pages[from];
    lv_obj_t *in_page = g_pages[to];
    if(!out_page || !in_page) {
        page_show_instant(to);
        return;
    }

    g_page_transition_running = true;
    g_anim_out_page = out_page;
    g_anim_in_page = in_page;

    page_files_set_visible(to == UI_PAGE_FILES ? 1 : 0);

    lv_obj_clear_flag(in_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(in_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_x(in_page, UI_PAGE_ANIM_SLIDE, 0);
    lv_obj_move_foreground(in_page);

    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, out_page);
    lv_anim_set_exec_cb(&a_out, page_anim_set_opa);
    lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a_out, UI_PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in);
    lv_anim_start(&a_out);

    lv_anim_t a_in_opa;
    lv_anim_init(&a_in_opa);
    lv_anim_set_var(&a_in_opa, in_page);
    lv_anim_set_exec_cb(&a_in_opa, page_anim_set_opa);
    lv_anim_set_values(&a_in_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_in_opa, UI_PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a_in_opa, lv_anim_path_ease_out);
    lv_anim_start(&a_in_opa);

    lv_anim_t a_in_x;
    lv_anim_init(&a_in_x);
    lv_anim_set_var(&a_in_x, in_page);
    lv_anim_set_exec_cb(&a_in_x, page_anim_set_x);
    lv_anim_set_values(&a_in_x, UI_PAGE_ANIM_SLIDE, 0);
    lv_anim_set_duration(&a_in_x, UI_PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a_in_x, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a_in_x, page_transition_complete);
    lv_anim_start(&a_in_x);
}

static void shell_refresh_page(ui_page_id_t id, bool force)
{
    switch(id) {
    case UI_PAGE_HOME:
        page_home_refresh(force);
        break;
    case UI_PAGE_CONTROL:
        page_control_refresh();
        break;
    case UI_PAGE_FILES:
        page_files_refresh();
        break;
    case UI_PAGE_SETTINGS:
        page_settings_refresh(force);
        break;
    default:
        break;
    }
}

static void shell_refresh_cb(lv_timer_t *)
{
    if(g_page_transition_running) {
        return;
    }
    shell_refresh_page(g_active_page, false);
}

static void style_nav_btn(lv_obj_t *btn, bool active)
{
    lv_obj_remove_style(btn, ui_style_nav_item(), 0);
    lv_obj_remove_style(btn, ui_style_nav_item_active(), 0);
    lv_obj_add_style(btn, active ? ui_style_nav_item_active() : ui_style_nav_item(), 0);
}

static void show_page(ui_page_id_t id, bool instant)
{
    if(id >= UI_PAGE_COUNT) {
        return;
    }
    if(g_page_transition_running) {
        return;
    }
    if(id == g_active_page && !instant) {
        return;
    }

    ui_keyboard_hide();

    for(uint32_t i = 0; i < UI_PAGE_COUNT; i++) {
        if(g_nav_btns[i]) {
            style_nav_btn(g_nav_btns[i], i == (uint32_t)id);
        }
    }

    if(instant) {
        page_show_instant(id);
        return;
    }

    const ui_page_id_t from = g_active_page;
    g_active_page = id;
    page_transition(from, id);
}

static void nav_click_cb(lv_event_t *e)
{
    ui_page_id_t id = (ui_page_id_t)(uintptr_t)lv_event_get_user_data(e);
    show_page(id, false);
}

static lv_obj_t *make_nav_button(lv_obj_t *parent, const nav_item_def_t *def, ui_page_id_t id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, UI_NAV_ITEM_H);
    lv_obj_add_style(btn, ui_style_nav_item(), 0);
    lv_obj_add_event_cb(btn, nav_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, UI_PAD_SM, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, def->icon);
    lv_obj_set_style_text_font(icon, ui_font_icon(), 0);
    lv_obj_set_width(icon, UI_NAV_ICON_W);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, def->label);
    lv_obj_set_flex_grow(txt, 1);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_LEFT, 0);

    return btn;
}

void ui_shell_init(void)
{
    ui_theme_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, ui_color_bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 横屏：左导航 + 右内容（类似手机底部 Tab 翻到左侧） */
    g_root = lv_obj_create(scr);
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(g_root, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_set_style_pad_column(g_root, 0, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);

    /* 左侧滑动导航栏 */
    g_sidebar = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_sidebar);
    lv_obj_set_width(g_sidebar, UI_SIDEBAR_W);
    lv_obj_set_height(g_sidebar, LV_PCT(100));
    lv_obj_set_style_bg_color(g_sidebar, ui_color_sidebar, 0);
    lv_obj_set_style_bg_opa(g_sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(g_sidebar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(g_sidebar, 1, 0);
    lv_obj_set_style_border_color(g_sidebar, ui_color_border, 0);
    lv_obj_set_flex_flow(g_sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_sidebar, UI_PAD_SM, 0);
    lv_obj_set_style_pad_row(g_sidebar, 10, 0);
    lv_obj_add_flag(g_sidebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_sidebar, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_sidebar, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *brand = lv_label_create(g_sidebar);
    lv_label_set_text(brand, LV_SYMBOL_SETTINGS " Rongsu");
    lv_obj_set_style_text_color(brand, ui_color_accent, 0);
    lv_obj_set_style_text_font(brand, ui_font_title(), 0);
    lv_obj_set_style_pad_bottom(brand, UI_PAD_SM, 0);

    for(uint32_t i = 0; i < UI_PAGE_COUNT; i++) {
        g_nav_btns[i] = make_nav_button(g_sidebar, &k_nav_defs[i], (ui_page_id_t)i);
    }

    /* 右侧内容区 */
    g_content = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_content);
    lv_obj_set_flex_grow(g_content, 1);
    lv_obj_set_height(g_content, LV_PCT(100));
    lv_obj_set_style_bg_opa(g_content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);

    for(uint32_t i = 0; i < UI_PAGE_COUNT; i++) {
        g_pages[i] = k_nav_defs[i].factory(g_content);
        lv_obj_set_size(g_pages[i], LV_PCT(100), LV_PCT(100));
        if(i != 0) {
            lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    show_page(UI_PAGE_HOME, true);

    lv_timer_create(shell_refresh_cb, kShellRefreshMs, nullptr);

    /* 初始化全局软键盘 */
    ui_keyboard_init(scr);
}
