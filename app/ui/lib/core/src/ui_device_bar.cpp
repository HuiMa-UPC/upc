#include "ui_device_bar.hpp"

#include "ui_device_state.hpp"
#include "ui_theme.hpp"

namespace {

lv_obj_t *g_bar        = nullptr;
lv_obj_t *g_lbl_wifi   = nullptr;
lv_obj_t *g_lbl_name   = nullptr;
lv_obj_t *g_dot_status = nullptr;
lv_obj_t *g_lbl_status = nullptr;

void on_state_changed(const ui_device_snapshot_t *snap)
{
    if(!snap || !g_bar) {
        return;
    }
    if(g_lbl_wifi) {
        /* 连上=正常白，断开=灰暗，作为 WiFi 连接状态指示 */
        lv_obj_set_style_text_color(g_lbl_wifi,
            snap->wifi_connected ? user_color_text : user_color_text_dim, 0);
    }
    if(g_lbl_name) {
        lv_label_set_text(g_lbl_name, snap->device_name);
    }
    if(g_lbl_status) {
        lv_label_set_text(g_lbl_status, ui_device_status_text(snap->status));
        lv_obj_set_style_text_color(g_lbl_status,
                                    ui_device_status_color(snap->status), 0);
    }
    if(g_dot_status) {
        lv_obj_set_style_bg_color(g_dot_status,
                                  ui_device_status_color(snap->status), 0);
    }
}

} /* namespace */

lv_obj_t *ui_device_bar_create(lv_obj_t *parent)
{
    if(g_bar) {
        return g_bar;
    }

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    /* FLOATING + pos(0,0)：shell_ 是横向 flex，本条不能占格位，否则会把
     * content_panel_ 挤走（同 menu_panel_ 的做法）。 */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_height(bar, USER_TOPBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 本条铺满整屏（要压在首页的整屏摄像头层之上），所以左边距自己让出侧栏；
     * 页面内的控件则是靠 content_panel_ 的 pad_left 让位，两套算出的 x 一致。 */
    lv_obj_set_style_pad_left(bar, USER_SIDEBAR_W + USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_right(bar, USER_TOPBAR_PAD_HOR, 0);
    lv_obj_set_style_pad_column(bar, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 去掉 lv_obj_create 默认带的 CLICKABLE：本条浮在页面之上且整屏宽，若可点
     * 会吃掉底下控件的触摸——首页那两颗顶栏按钮正好在它下面。LVGL 命中测试会
     * 跳过非可点对象继续往下找，所以这里只需保证条自身不可点；四个子控件都是
     * label / 纯色块，本身不带 CLICKABLE。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    g_lbl_wifi = lv_label_create(bar);
    lv_label_set_text(g_lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(g_lbl_wifi, user_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_wifi, user_font_text(), 0);

    g_lbl_name = lv_label_create(bar);
    lv_obj_set_style_text_color(g_lbl_name, user_color_text, 0);
    lv_obj_set_style_text_font(g_lbl_name, user_font_text(), 0);

    g_dot_status = lv_obj_create(bar);
    lv_obj_remove_style_all(g_dot_status);
    lv_obj_set_size(g_dot_status, USER_STATUS_DOT_SZ, USER_STATUS_DOT_SZ);
    lv_obj_set_style_radius(g_dot_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_dot_status, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_left(g_dot_status, USER_PAD_SM, 0);
    lv_obj_remove_flag(g_dot_status, LV_OBJ_FLAG_CLICKABLE);

    g_lbl_status = lv_label_create(bar);
    lv_obj_set_style_text_font(g_lbl_status, user_font_text(), 0);

    g_bar = bar;
    ui_device_state_add_observer(on_state_changed);
    on_state_changed(ui_device_state_get());   /* 首帧：先按当前快照填一次 */
    return g_bar;
}

void ui_device_bar_set_visible(bool visible)
{
    if(!g_bar) {
        return;
    }
    if(visible) {
        lv_obj_remove_flag(g_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_bar, LV_OBJ_FLAG_HIDDEN);
    }
}
