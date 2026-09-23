#include "ui_keyboard.hpp"
#include "ui_theme.hpp"

#include <cstdio>
#include <cstring>

/*
 * 键盘组件：两套输入面板共用同一套 attach/hide 接口。
 *   - 文本模式（LV_KEYBOARD_MODE_TEXT_*）：沿用 LVGL 内置 lv_keyboard，底部弹出。
 *   - 数字模式（LV_KEYBOARD_MODE_NUMBER）：自绘右侧数字键盘面板（大号读数 + 圆形键
 *     首排 C 清零 / − 负号 / ⌫ 删除，随后 7 8 9 / 4 5 6 / 1 2 3 / . 0 + 确认）。
 *     圆形按键 + 大号读数无法用 buttonmatrix 实现，故单独自绘并直接读写 textarea。
 *     负号键切换前导 '-'，全局对所有数字输入（含 debug 页各字段）生效。
 *
 *     配色按新稿 6363:44775/44778 翻成浅色（白面板 + 深墨键 + 深墨字），**几何一律不动**：
 *     稿上只画了 7-9 / 4-6 / 1-3 / . 0 确认四排，没画 C / − / ⌫ 那一排，但调试页有字段
 *     依赖负号与删除，删掉是功能回退，故保留原布局。遮罩仍是黑 50%（稿自己标的
 *     「50%黑色蒙版」），与弹窗那档的深墨 28% 不同。
 */

static lv_obj_t *s_kb          = nullptr;  /**< 文本键盘（lv_keyboard） */
static lv_obj_t *s_screen      = nullptr;  /**< 顶层父，创建数字键盘用 */
static lv_obj_t *s_numpad      = nullptr;  /**< 数字键盘根（含遮罩） */
static lv_obj_t *s_numpad_disp = nullptr;  /**< 数字键盘大号读数 label */
static lv_obj_t *s_current_ta  = nullptr;
/** 数字键盘：首键插入前先清空（覆盖原值） */
static bool      s_replace_on_first_key = false;
static bool      s_numpad_open          = false;

/** 数字读数最大长度（含小数点/负号），防止溢出 */
static constexpr size_t kNumpadMaxLen = 10;

/** 键盘显隐观察者（单槽），见 ui_keyboard_set_visibility_cb() */
static ui_keyboard_visibility_cb_t s_vis_cb = nullptr;
static void                       *s_vis_ud = nullptr;

/** 通知观察者键盘已弹出，并带上它遮住的屏幕矩形。
 *  必须先 update_layout：刚 remove HIDDEN 的对象坐标还是上一帧的。 */
static void notify_shown(lv_obj_t *kb_root)
{
    if(!s_vis_cb || !kb_root) {
        return;
    }
    lv_obj_update_layout(kb_root);
    lv_area_t a;
    lv_obj_get_coords(kb_root, &a);
    s_vis_cb(true, &a, s_vis_ud);
}

static void notify_hidden(void)
{
    if(s_vis_cb) {
        s_vis_cb(false, nullptr, s_vis_ud);
    }
}

static bool ta_input_allowed(lv_obj_t *ta)
{
    if(!ta) {
        return false;
    }
    if(lv_obj_has_state(ta, LV_STATE_DISABLED)) {
        return false;
    }
    if(!lv_obj_has_flag(ta, LV_OBJ_FLAG_CLICKABLE)) {
        return false;
    }
    return true;
}

/** 单行输入框：关闭聚焦自动滚动，避免文字在框内上下跳动 */
static void style_single_line_ta(lv_obj_t *ta)
{
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scroll_dir(ta, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_line_space(ta, 0, 0);

    const lv_font_t *font = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
    if(!font) {
        font = ui_font_body();
        lv_obj_set_style_text_font(ta, font, 0);
    }

    /* 优先用样式高度：attach 时常在首次 layout 之前（如 Other 隐藏 tab），
     * lv_obj_get_height() 读 coords 会得到 0/默认值，pad 被钳成 2 导致数字贴顶偏高。 */
    int32_t h = lv_obj_get_style_height(ta, LV_PART_MAIN);
    if(h == LV_SIZE_CONTENT || LV_COORD_IS_PCT(h) || h <= 0) {
        lv_obj_update_layout(ta);
        h = lv_obj_get_height(ta);
    }
    int32_t lh = lv_font_get_line_height(font);
    int32_t border = lv_obj_get_style_border_width(ta, LV_PART_MAIN);
    if(h > 0 && lh > 0) {
        int32_t pad_v = (h - 2 * border - lh) / 2;
        if(pad_v < 2) {
            pad_v = 2;
        }
        lv_obj_set_style_pad_top(ta, pad_v, 0);
        lv_obj_set_style_pad_bottom(ta, pad_v, 0);
    }
}

static void detach_keyboard_from_ta(lv_obj_t *ta)
{
    const bool was_visible =
        (s_kb && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) || s_numpad_open;

    if(s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, nullptr);
    }
    if(s_numpad) {
        lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    }
    s_numpad_open = false;
    if(ta) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        lv_indev_reset(nullptr, ta);
    }
    s_current_ta = nullptr;
    s_replace_on_first_key = false;

    if(was_visible) {
        notify_hidden();
    }
}

static void hide_keyboard_core(lv_obj_t *ta)
{
    detach_keyboard_from_ta(ta);
}

/* ===========================================================================
 *  数字键盘（自绘）
 * =========================================================================== */

/** 大号读数与当前 textarea 同步；空值显示 "0" */
static void numpad_sync_display(void)
{
    if(!s_numpad_disp) {
        return;
    }
    const char *cur = s_current_ta ? lv_textarea_get_text(s_current_ta) : "";
    lv_label_set_text(s_numpad_disp, (cur && cur[0]) ? cur : "0");
}

/** 数字/小数点键：user_data 为待插入字符串（"7"/"."…） */
static void numpad_on_char(lv_event_t *e)
{
    if(!s_current_ta) {
        return;
    }
    const char *s = static_cast<const char *>(lv_event_get_user_data(e));
    if(!s || !s[0]) {
        return;
    }

    if(s_replace_on_first_key) {
        lv_textarea_set_text(s_current_ta, "");
        lv_textarea_set_cursor_pos(s_current_ta, 0);
        s_replace_on_first_key = false;
    }

    const char *cur = lv_textarea_get_text(s_current_ta);
    /* 小数点去重 */
    if(s[0] == '.' && cur && std::strchr(cur, '.')) {
        return;
    }
    /* 长度上限 */
    if(cur && std::strlen(cur) >= kNumpadMaxLen) {
        return;
    }

    lv_textarea_add_text(s_current_ta, s);
    numpad_sync_display();
}

/** 删除键 ⌫：删除光标前一个字符 */
static void numpad_on_del(lv_event_t *)
{
    if(!s_current_ta) {
        return;
    }
    s_replace_on_first_key = false;
    lv_textarea_delete_char(s_current_ta);
    numpad_sync_display();
}


/** 清零键 C：清空当前输入框（读数回落 "0"） */
static void numpad_on_clear(lv_event_t *)
{
    if(!s_current_ta) {
        return;
    }
    s_replace_on_first_key = false;
    lv_textarea_set_text(s_current_ta, "");
    lv_textarea_set_cursor_pos(s_current_ta, 0);
    numpad_sync_display();
}

/** 负号键 −：切换当前值正负（切换前导 '-'），在现值基础上编辑而非覆盖 */
static void numpad_on_sign(lv_event_t *)
{
    if(!s_current_ta) {
        return;
    }
    s_replace_on_first_key = false;
    const char *cur = lv_textarea_get_text(s_current_ta);
    char buf[kNumpadMaxLen + 2];
    if(cur && cur[0] == '-') {
        std::snprintf(buf, sizeof(buf), "%s", cur + 1);           /* 去负号 */
    } else {
        if(cur && std::strlen(cur) >= kNumpadMaxLen) {
            return;                                               /* 加负号会超长则忽略 */
        }
        std::snprintf(buf, sizeof(buf), "-%s", cur ? cur : "");   /* 加负号 */
    }
    lv_textarea_set_text(s_current_ta, buf);
    lv_textarea_set_cursor_pos(s_current_ta, LV_TEXTAREA_CURSOR_LAST);
    numpad_sync_display();
}

/** 点击遮罩：当前值已实时写入 textarea，直接收起即可（不提交） */
static void numpad_on_ok(lv_event_t *)
{
    hide_keyboard_core(s_current_ta);
}

/** 确认键（对勾）：收起并向 textarea 发 LV_EVENT_READY，供弹窗“对勾即提交” */
static void numpad_on_ok_confirm(lv_event_t *)
{
    lv_obj_t *ta = s_current_ta;
    hide_keyboard_core(ta);
    if(ta) {
        lv_obj_send_event(ta, LV_EVENT_READY, nullptr);
    }
}

/** 圆形按键（数字 / 功能键 / 确认复用）；accent=true 为确认键（深墨实心 + 白字）。
 *
 * 稿 6363:44778 的配色：普通键 `#333145 @4%` 实底加一圈同色 4% 描边，字形 `#333145 @70%`；
 * 确认键是深墨实心配白字（稿上带一层渐变与外发光，LVGL 做不了背景模糊，取平涂）。 */
static lv_obj_t *numpad_make_key(lv_obj_t *parent, int32_t sz, const char *txt,
                                 lv_event_cb_t cb, void *ud, bool accent,
                                 const lv_font_t *font)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if(accent) {
        lv_obj_set_style_bg_color(btn, user_light_nav_on, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_color(btn, user_light_nav_on, 0);
        lv_obj_set_style_bg_opa(btn, 10, 0);          /* 稿 #333145 @4% */
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, user_light_nav_on, 0);
        lv_obj_set_style_border_opa(btn, 10, 0);
    }
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, accent ? lv_color_white() : user_light_text, 0);
    lv_obj_set_style_text_opa(l, accent ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    return btn;
}

/** 一行（横向 flex，居中，键间距 gap） */
static lv_obj_t *numpad_make_row(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    return row;
}

static lv_obj_t *numpad_make_spacer(lv_obj_t *parent)
{
    lv_obj_t *sp = lv_obj_create(parent);
    lv_obj_remove_style_all(sp);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    return sp;
}

static void build_numpad(lv_obj_t *parent)
{
    lv_display_t *disp = lv_display_get_default();
    const int32_t scr_w = lv_display_get_horizontal_resolution(disp);
    const int32_t scr_h = lv_display_get_vertical_resolution(disp);

    /* 面板与按键尺寸按屏宽比例推导（适配 1024/1280/1920 三套 profile） */
    int32_t panel_w = scr_w * 39 / 100;
    if(panel_w < 360) {
        panel_w = 360;
    }
    const int32_t side = panel_w * 12 / 100;
    const int32_t gap  = panel_w * 6 / 100;   /* 由 8% 收窄为 6%：为新增的 C/−/⌫ 一排腾出纵向空间 */

    /*
     * 键径同时受面板宽（每行 3 键）与屏高（读数行 + 5 排按键 + 6 段行距）约束，取较小者，
     * 保证在 1024×600 矮屏也放得下加高后的 5 排键盘（面板列内子项：读数/5排/底部弹簧）。
     */
    const int32_t readout_h = lv_font_get_line_height(user_font_value());
    const int32_t btn_w = (panel_w - 2 * side - 2 * gap) / 3;
    const int32_t btn_h = (scr_h - 2 * side - readout_h - 6 * gap) / 5;
    int32_t btn = btn_w < btn_h ? btn_w : btn_h;
    if(btn < 44) {
        btn = 44;
    }

    /* 遮罩根：占满全屏、半透明黑，点击空白处=确认收起 */
    s_numpad = lv_obj_create(parent);
    lv_obj_remove_style_all(s_numpad);
    lv_obj_set_size(s_numpad, scr_w, scr_h);
    lv_obj_align(s_numpad, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_numpad, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_numpad, LV_OPA_50, 0);
    lv_obj_remove_flag(s_numpad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_numpad, numpad_on_ok, LV_EVENT_CLICKED, nullptr);

    /* 右侧面板：纵向 flex，吸收自身点击（不穿透到遮罩）。稿 6363:44778 是纯白底。 */
    lv_obj_t *panel = lv_obj_create(s_numpad);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, panel_w, scr_h);
    lv_obj_align(panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, user_light_card, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, user_light_nav_on, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_10, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_pad_all(panel, side, 0);
    lv_obj_set_style_pad_row(panel, gap, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 顶部读数行：仅大号读数（删除键下移到首排功能键） */
    lv_obj_t *top = numpad_make_row(panel, gap);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    s_numpad_disp = lv_label_create(top);
    lv_obj_set_flex_grow(s_numpad_disp, 1);
    lv_label_set_long_mode(s_numpad_disp, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_numpad_disp, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_numpad_disp, user_light_text, 0);
    lv_obj_set_style_text_font(s_numpad_disp, user_font_value(), 0);
    lv_label_set_text(s_numpad_disp, "0");

    /* 首排功能键：C 清零 / − 负号 / ⌫ 删除 */
    static const char *kClear = "C";
    static const char *kMinus = "-";
    lv_obj_t *row_fn = numpad_make_row(panel, gap);
    numpad_make_key(row_fn, btn, kClear, numpad_on_clear, nullptr, false,
                    user_font_value());
    numpad_make_key(row_fn, btn, kMinus, numpad_on_sign, nullptr, false,
                    user_font_value());
    numpad_make_key(row_fn, btn, LV_SYMBOL_BACKSPACE, numpad_on_del, nullptr, false,
                    user_font_text());


    /* 数字键区：7 8 9 / 4 5 6 / 1 2 3 / . 0 [确认] */
    static const char *kRow1[3] = { "7", "8", "9" };
    static const char *kRow2[3] = { "4", "5", "6" };
    static const char *kRow3[3] = { "1", "2", "3" };
    const char *const *kRows[3] = { kRow1, kRow2, kRow3 };

    for(int r = 0; r < 3; ++r) {
        lv_obj_t *row = numpad_make_row(panel, gap);
        for(int c = 0; c < 3; ++c) {
            numpad_make_key(row, btn, kRows[r][c], numpad_on_char,
                            const_cast<char *>(kRows[r][c]), false,
                            user_font_value());
        }
    }

    static const char *kDot = ".";
    static const char *kZero = "0";
    lv_obj_t *row4 = numpad_make_row(panel, gap);
    numpad_make_key(row4, btn, kDot, numpad_on_char, const_cast<char *>(kDot),
                    false, user_font_value());
    numpad_make_key(row4, btn, kZero, numpad_on_char, const_cast<char *>(kZero),
                    false, user_font_value());
    numpad_make_key(row4, btn, LV_SYMBOL_OK, numpad_on_ok_confirm, nullptr, true,
                    user_font_text());

    numpad_make_spacer(panel);
}

static void show_numpad_for_ta(lv_obj_t *ta)
{
    if(!s_numpad || !ta || !ta_input_allowed(ta)) {
        return;
    }

    if(s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }

    s_current_ta = ta;
    s_replace_on_first_key = true;
    numpad_sync_display();
    lv_obj_remove_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_numpad);
    s_numpad_open = true;
    /* 遮罩铺满全屏、面板只占右侧，让位要按面板算，所以取 panel 而不是 s_numpad */
    notify_shown(lv_obj_get_child(s_numpad, 0));

    /*
     * 等当前触摸抬起后再处理下一次点击，避免抬起落在刚弹出的面板上导致输入框失焦。
     * 与 LVGL demos/widgets ta_event_cb 一致。
     */
    lv_indev_t *indev = lv_indev_active();
    if(indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_indev_wait_release(indev);
    }
}

/* ===========================================================================
 *  文本键盘（lv_keyboard）
 * =========================================================================== */

static void show_keyboard_for_ta(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    if(!s_kb || !ta || !ta_input_allowed(ta)) {
        return;
    }

    if(s_numpad) {
        lv_obj_add_flag(s_numpad, LV_OBJ_FLAG_HIDDEN);
        s_numpad_open = false;
    }

    s_current_ta = ta;
    lv_keyboard_set_mode(s_kb, mode);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
    notify_shown(s_kb);

    lv_indev_t *indev = lv_indev_active();
    if(indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_indev_wait_release(indev);
    }
}

/* textarea 被销毁时解除绑定，避免键盘仍指向已释放对象 */
static void ta_delete_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if(s_current_ta == lv_event_get_target_obj(e)) {
        detach_keyboard_from_ta(nullptr);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_READY) {
        /* 文本键盘对勾：收起并把 READY 转发给 textarea，供弹窗“对勾即提交” */
        lv_obj_t *ta = s_current_ta;
        hide_keyboard_core(ta);
        if(ta) {
            lv_obj_send_event(ta, LV_EVENT_READY, nullptr);
        }
    } else if(code == LV_EVENT_CANCEL) {
        hide_keyboard_core(s_current_ta);
    }
}

/*
 * 用 CLICKED（而非 FOCUSED）触发弹键盘：FOCUSED 在"按下"瞬间就发出，无法区分
 * 点按与滑动翻页；CLICKED 仅在"按下→抬起且期间未发生滚动"时发出，滑动手势不会
 * 触发，避免翻页时误弹键盘。
 */
static void ta_click_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const lv_keyboard_mode_t mode =
        static_cast<lv_keyboard_mode_t>((uintptr_t)lv_event_get_user_data(e));

    if(!ta_input_allowed(ta)) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        if(s_current_ta == ta) {
            hide_keyboard_core(ta);
        }
        return;
    }

    if(mode == LV_KEYBOARD_MODE_NUMBER) {
        show_numpad_for_ta(ta);
    } else {
        show_keyboard_for_ta(ta, mode);
    }
}

/*
 * 键盘打开期间若被其它逻辑误触发 DEFOCUSED，立即恢复焦点，避免键盘闪退。
 * 真正关闭仅通过 OK/Cancel、ui_keyboard_hide 或禁用输入框。
 */
static void ta_defocus_guard_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_DEFOCUSED) {
        return;
    }

    lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const bool kb_open =
        (s_kb && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) || s_numpad_open;
    if(s_current_ta == ta && kb_open) {
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
    }
}

void ui_keyboard_init(lv_obj_t *parent)
{
    s_screen = parent;

    s_kb = lv_keyboard_create(parent);
    lv_obj_set_size(s_kb, LV_PCT(100), LV_PCT(42));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_font(s_kb, ui_font_body(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_kb, ui_font_body(), LV_PART_ITEMS);

    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, nullptr);

    build_numpad(parent);
}

void ui_keyboard_attach(lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    style_single_line_ta(ta);
    void *ud = reinterpret_cast<void *>(static_cast<uintptr_t>(mode));
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(ta, ta_defocus_guard_cb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(ta, ta_delete_cb, LV_EVENT_DELETE, nullptr);
}

void ui_keyboard_hide(void)
{
    hide_keyboard_core(s_current_ta);
}

void ui_keyboard_set_visibility_cb(ui_keyboard_visibility_cb_t cb, void *user_data)
{
    s_vis_cb = cb;
    s_vis_ud = user_data;
}
