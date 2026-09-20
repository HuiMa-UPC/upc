#include "ui_device_bar.hpp"

#include "ui_device_state.hpp"
#include "ui_theme.hpp"

namespace {

lv_obj_t *g_bar        = nullptr;
lv_obj_t *g_lbl_wifi   = nullptr;
lv_obj_t *g_lbl_name   = nullptr;
lv_obj_t *g_dot_status = nullptr;
lv_obj_t *g_lbl_status = nullptr;

/* 第二组（稿上的 NitroPod）。数据源未接通，先在 create() 里填占位值，
 * 之后由 ui_device_bar_set_peer() 改写，见头文件。 */
lv_obj_t *g_divider      = nullptr;
lv_obj_t *g_peer_group   = nullptr;
lv_obj_t *g_lbl_peer     = nullptr;
lv_obj_t *g_dot_peer     = nullptr;
lv_obj_t *g_lbl_peer_st  = nullptr;

/** 稿上状态点是 8@1280，而共享的 USER_STATUS_DOT_SZ 是 12（还被调试页、
 *  ui_user_widgets、侧栏红点、通知徽标用着，不能动）。按 2/3 折算三档得
 *  12 / 8 / 6，与稿的 ×1.5 / ×1 / ×0.8 逐档吻合。 */
int32_t dot_sz(void)
{
    return USER_STATUS_DOT_SZ * 2 / 3;
}

/* 只有三档：稿上第二台设备只画了 Connected 一态，多余的等级等接上真数据、
 * 知道它到底有哪些状态了再加，免得凭空造一套语义。 */
lv_color_t peer_level_color(ui_peer_level_t level)
{
    switch(level) {
    case UI_PEER_ERROR:   return user_color_status_err;
    case UI_PEER_OFFLINE: return user_light_icon_off;
    case UI_PEER_OK:
    default:              return user_light_ok;
    }
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
    lv_obj_set_style_text_font(name, user_font_body(), 0);   /* 稿 22 */

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
    lv_obj_set_style_text_font(status, user_font_body(), 0);   /* 稿 22 */

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
    if(g_lbl_wifi) {
        /* 连上=正常深色，断开=灰暗，作为 WiFi 连接状态指示。
         * **两档都得是浅色档的值**：早先连上那档给的是 user_color_text（纯白），
         * 压在浅底上整个图标会消失。 */
        lv_obj_set_style_text_color(g_lbl_wifi,
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
    /* 稿上 wifi 图标与第一组、组与分隔线之间都是 10.667，取 11。
     * 组内那两档间距（16 / 8）另给，见 make_group()。 */
    lv_obj_set_style_pad_column(bar, USER_PAD_SM * 2 / 3, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 去掉 lv_obj_create 默认带的 CLICKABLE：本条浮在页面之上且整屏宽，若可点
     * 会吃掉底下控件的触摸——首页那两颗顶栏按钮正好在它下面。LVGL 命中测试会
     * 跳过非可点对象继续往下找，所以这里只需保证条自身不可点；子控件都是
     * label / 纯色块 / 去了 CLICKABLE 的容器。 */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    g_lbl_wifi = lv_label_create(bar);
    lv_label_set_text(g_lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(g_lbl_wifi, user_light_text, 0);
    lv_obj_set_style_text_font(g_lbl_wifi, user_font_body(), 0);

    make_group(bar, &g_lbl_name, &g_dot_status, &g_lbl_status);

    /* 两组之间那道竖线（稿 1×20 #d9d9d9）。它与第二组同进同退，见 set_peer。 */
    g_divider = lv_obj_create(bar);
    lv_obj_remove_style_all(g_divider);
    lv_obj_set_size(g_divider, 1, USER_STATUS_DOT_SZ * 5 / 3);   /* 稿 20 */
    lv_obj_set_style_bg_color(g_divider, user_light_divider, 0);
    lv_obj_set_style_bg_opa(g_divider, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_divider, LV_OBJ_FLAG_CLICKABLE);

    g_peer_group = make_group(bar, &g_lbl_peer, &g_dot_peer, &g_lbl_peer_st);

    g_bar = bar;
    ui_device_state_add_observer(on_state_changed);
    on_state_changed(ui_device_state_get());   /* 首帧：先按当前快照填一次 */

    /* 第二台设备的数据源还没接通，先按稿写死。接上之后把这一行挪到数据更新处即可。 */
    ui_device_bar_set_peer("NitroPod", "Connected", UI_PEER_OK);
    return g_bar;
}

void ui_device_bar_set_peer(const char *name, const char *status,
                            ui_peer_level_t level)
{
    if(!g_peer_group) {
        return;
    }
    /* 没名字 = 那台设备不存在：整组连同分隔线一起收起来，第一组仍正常显示。 */
    const bool show = (name != nullptr) && (name[0] != '\0');
    if(!show) {
        lv_obj_add_flag(g_peer_group, LV_OBJ_FLAG_HIDDEN);
        if(g_divider) lv_obj_add_flag(g_divider, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(g_peer_group, LV_OBJ_FLAG_HIDDEN);
    if(g_divider) lv_obj_remove_flag(g_divider, LV_OBJ_FLAG_HIDDEN);

    const lv_color_t c = peer_level_color(level);
    lv_label_set_text(g_lbl_peer, name);
    lv_label_set_text(g_lbl_peer_st, status ? status : "");
    lv_obj_set_style_text_color(g_lbl_peer_st, c, 0);
    lv_obj_set_style_bg_color(g_dot_peer, c, 0);
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
