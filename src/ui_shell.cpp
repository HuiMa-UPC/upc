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

static const nav_item_def_t k_nav_defs[UI_PAGE_COUNT] = {
    { LV_SYMBOL_HOME,     "Home",     page_home_create },
    { LV_SYMBOL_PLAY,     "Control",  page_control_create },
    { LV_SYMBOL_FILE,     "Files",    page_files_create },
    { LV_SYMBOL_SETTINGS, "Settings", page_settings_create },
    { LV_SYMBOL_USB,      "Serial",   page_serial_create },
    { LV_SYMBOL_WIFI,     "TCP",      page_tcp_create },
};

static lv_obj_t *g_root;
static lv_obj_t *g_sidebar;
static lv_obj_t *g_content;
static lv_obj_t *g_nav_btns[UI_PAGE_COUNT];
static lv_obj_t *g_pages[UI_PAGE_COUNT];
static ui_page_id_t g_active = UI_PAGE_HOME;

static void style_nav_btn(lv_obj_t *btn, bool active)
{
    lv_obj_remove_style(btn, ui_style_nav_item(), 0);
    lv_obj_remove_style(btn, ui_style_nav_item_active(), 0);
    lv_obj_add_style(btn, active ? ui_style_nav_item_active() : ui_style_nav_item(), 0);
}

static void show_page(ui_page_id_t id)
{
    if(id >= UI_PAGE_COUNT) {
        return;
    }

    /* 页面切换时隐藏键盘，避免键盘残留在新页面上 */
    ui_keyboard_hide();

    for(uint32_t i = 0; i < UI_PAGE_COUNT; i++) {
        if(g_pages[i]) {
            if(i == (uint32_t)id) {
                lv_obj_clear_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
            else {
                lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if(g_nav_btns[i]) {
            style_nav_btn(g_nav_btns[i], i == (uint32_t)id);
        }
    }
    g_active = id;

    page_files_set_visible(id == UI_PAGE_FILES ? 1 : 0);
}

static void nav_click_cb(lv_event_t *e)
{
    ui_page_id_t id = (ui_page_id_t)(uintptr_t)lv_event_get_user_data(e);
    show_page(id);
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
    lv_obj_set_style_pad_column(btn, 18, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, def->icon);
    lv_obj_set_style_text_font(icon, ui_font_icon(), 0);
    lv_obj_set_width(icon, 64);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, def->label);
    lv_obj_set_flex_grow(txt, 1);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
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

    show_page(UI_PAGE_HOME);

    /* 初始化全局软键盘 */
    ui_keyboard_init(scr);
}

lv_obj_t *ui_shell_content_root(void)
{
    return g_content;
}

void ui_shell_show_page(ui_page_id_t page)
{
    show_page(page);
}
