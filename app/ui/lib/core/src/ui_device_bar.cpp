#include "ui_device_bar.hpp"

#include "settings_icons.h"
#include "ui_device_state.hpp"
#include "ui_theme.hpp"

namespace {

lv_obj_t *g_bar        = nullptr;
lv_obj_t *g_img_wifi   = nullptr;
lv_obj_t *g_lbl_name   = nullptr;
lv_obj_t *g_dot_status = nullptr;
lv_obj_t *g_lbl_status = nullptr;

/** 稿上状态点是 8@1280，而共享的 USER_STATUS_DOT_SZ 是 12（还被调试页、
 *  ui_user_widgets、侧栏红点、通知徽标用着，不能动）。按 2/3 折算三档得
 *  12 / 8 / 6，与稿的 ×1.5 / ×1 / ×0.8 逐档吻合。 */
int32_t dot_sz(void)
{
    return USER_STATUS_DOT_SZ * 2 / 3;
}

/** 建一组「设备名 + 状态点 + 状态字」。稿 6201:85961：组内 gap 16，
 *  状态点与状态字之间再单独 gap 8。 */
lv_obj_t *make_group(lv_obj_t *parent, lv_obj_t **out_name,
                     lv_obj_t **out_dot, lv_obj_t **out_status)
{
    lv_obj_t *grp = lv_obj_create(parent);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, USER_PAD_SM, 0);   /* 稿 16 */
    lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(grp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(grp);
    lv_obj_set_style_text_color(name, user_light_dev_name, 0);
    lv_obj_set_style_text_font(name, user_font_dlg_btn(), 0);   /* 稿 22 */

    /* 状态点与状态字自成一个小组：稿上它俩之间是 8，与上面那 16 不同。 */
    lv_obj_t *st = lv_obj_create(grp);
    lv_obj_remove_style_all(st);
    lv_obj_set_size(st, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, USER_PAD_SM / 2, 0);   /* 稿 8 */
    lv_obj_set_style_bg_opa(st, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(st, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(st);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, dot_sz(), dot_sz());
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *status = lv_label_create(st);
    lv_obj_set_style_text_font(status, user_font_dlg_btn(), 0);   /* 稿 22 */

    if(out_name)   *out_name = name;
    if(out_dot)    *out_dot = dot;
    if(out_status) *out_status = status;
    return grp;
}

void on_state_changed(const ui_device_snapshot_t *snap)
{
    if(!snap || !g_bar) {
        return;
    }
    if(g_img_wifi) {
        /* 连上=正常深色，断开=灰暗，作为 WiFi 连接状态指示。
         * **两档都得是浅色档的值**：早先连上那档给的是 user_color_text（纯白），
         * 压在浅底上整个图标会消失。 */
        lv_obj_set_style_image_recolor(g_img_wifi,
            snap->wifi_connected ? user_light_text : user_light_icon_off, 0);
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
    /* 本条铺满整屏（要压在首页的整屏摄像头层之上），所以左边距自己让出侧栏。
     * 退让量走 USER_PAD_LG 而不是 USER_TOPBAR_PAD_HOR：稿上本条左缘是 160，
     * 与 HOME 卡、问候语同一条竖线，而 USER_TOPBAR_PAD_HOR(28) 会算出 156。
     * 那个字段仍被首页那条顶部占位带用着，不动它。 */
    lv_obj_set_style_pad_left(bar, USER_SIDEBAR_W + USER_PAD_LG, 0);
    lv_obj_set_style_pad_right(bar, USER_TOPBAR_PAD_HOR, 0);
    /* 稿上 wifi 图标位与设备组之间是 10.667，取 10。
     * 组内那两档间距（16 / 8）另给，见 make_group()。 */
    lv_obj_set_style_pad_column(bar, USER_PAD_SM * 2 / 3, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 去掉 lv_obj_create 默认带的 CLICKABLE：本条浮在页面之上且整屏宽，若可点
     * 会吃掉底下控件的触摸——首页那两颗顶栏按钮正好在它下面。LVGL 命中测试会
     * 跳过非可点对象继续往下找，所以这里只需保证条自身不可点；子控件都是
     * label / Wi-Fi 图标 / 纯色块 / 去了 CLICKABLE 的容器。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    /* Wi-Fi 字形：稿上是两道弧 + 扇形（约 20×15），与设置页 Wi-Fi 列表那枚是同一个字形
     * （路径只差平移），直接用 settings_icon_wifi；LVGL 自带的 LV_SYMBOL_WIFI 是三道弧 + 圆点，
     * 24 号下约 30×20，形状大小都不对。稿上图标位 32 宽、字形居中，A8 图 24 见方，
     * 左右各留 4 才落到稿的 x 166..185，设备名起点随之到 202（稿 203）。 */
    g_img_wifi = lv_image_create(bar);
    lv_image_set_src(g_img_wifi, &settings_icon_wifi);
    lv_obj_set_size(g_img_wifi, USER_PAD, USER_PAD);
    lv_image_set_inner_align(g_img_wifi, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_margin_hor(g_img_wifi, USER_PAD_SM / 4, 0);
    lv_obj_set_style_image_recolor_opa(g_img_wifi, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(g_img_wifi, user_light_text, 0);
    lv_obj_remove_flag(g_img_wifi, LV_OBJ_FLAG_CLICKABLE);

    make_group(bar, &g_lbl_name, &g_dot_status, &g_lbl_status);

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
