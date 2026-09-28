#include "ui_chroma_green.hpp"

#include "lvgl_wrap.hpp"

namespace {

lv_obj_t *g_scr = nullptr;
bool      g_on  = false;
ui_chroma_green_changed_cb_t g_changed_cb = nullptr;

void notify_changed(bool enabled)
{
    if(g_changed_cb) {
        g_changed_cb(enabled);
    }
}

void destroy_scr()
{
    if(g_scr) {
        lv_obj_delete(g_scr);
        g_scr = nullptr;
    }
}

void on_tap_exit(lv_event_t * /*e*/)
{
    if(!g_on) {
        return;
    }
    g_on = false;
    destroy_scr();
    /* 只有点绿幕退出才回调：底下开关被挡住，需把勾选态弹回 Off。 */
    notify_changed(false);
}

/** 黑色十字：横竖两根纯黑条居中交叉；不可点，点击落到绿幕上仍可退出。 */
void add_cross(lv_obj_t *parent, lv_align_t align, int32_t x_ofs, int32_t y_ofs)
{
    constexpr int32_t kLen   = 48;
    constexpr int32_t kThick = 6;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, kLen, kLen);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_align(box, align, x_ofs, y_ofs);

    auto make_bar = [&](int32_t w, int32_t h) {
        lv_obj_t *b = lv_obj_create(box);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(b);
    };
    make_bar(kLen, kThick);
    make_bar(kThick, kLen);
}

void create_scr()
{
    if(g_scr) {
        return;
    }
    lv_obj_t *sys = lv_layer_sys();
    if(!sys) {
        return;
    }

    /* 绿幕满屏 + 中心/四角黑色十字（对齐/畸变参考）；无字、无边、无圆角。
     * 整屏可点一次关闭——打开后底下调试钮被挡住，只能点绿幕退出。 */
    g_scr = lv_obj_create(sys);
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_size(g_scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_scr, 0, 0);
    lv_obj_set_style_radius(g_scr, 0, 0);
    lv_obj_set_style_pad_all(g_scr, 0, 0);
    lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_scr, on_tap_exit, LV_EVENT_CLICKED, nullptr);

    constexpr int32_t kMargin = 24;
    add_cross(g_scr, LV_ALIGN_CENTER, 0, 0);
    add_cross(g_scr, LV_ALIGN_TOP_LEFT, kMargin, kMargin);
    add_cross(g_scr, LV_ALIGN_TOP_RIGHT, -kMargin, kMargin);
    add_cross(g_scr, LV_ALIGN_BOTTOM_LEFT, kMargin, -kMargin);
    add_cross(g_scr, LV_ALIGN_BOTTOM_RIGHT, -kMargin, -kMargin);

    lv_obj_move_foreground(g_scr);
}

} // namespace

extern "C" {

void ui_chroma_green_set_enabled(bool enabled)
{
    if(enabled == g_on) {
        return;
    }
    if(enabled) {
        create_scr();
        if(!g_scr) {
            return;
        }
        g_on = true;
    } else {
        g_on = false;
        destroy_scr();
    }
}

bool ui_chroma_green_enabled(void)
{
    return g_on;
}

void ui_chroma_green_set_changed_cb(ui_chroma_green_changed_cb_t cb)
{
    g_changed_cb = cb;
}

} // extern "C"
