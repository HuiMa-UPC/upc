#include "ui_xfr_dialog.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_theme.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---- 稿上出现、主题里没有的几个常量 ----------------------------------- */

/* 稿上的强调蓝 #4258ff 与 user_color_accent 完全相同，直接复用主题色；
 * 下面两个只在本文件用到，就地定义。 */
static const lv_color_t kGradEndColor  = LV_COLOR_MAKE(0x58, 0x6b, 0xff); /**< 进度条渐变末端 */
static const lv_color_t kFileNameColor = LV_COLOR_MAKE(0x64, 0x76, 0xff); /**< 文件名蓝 */

/** 稿上多处的 rgba(255,255,255,0.04)：副按钮底色、进度条槽 */
static const lv_opa_t kFaintFillOpa = 10; /* 0.04 × 255 ≈ 10 */

/** 进度条圆角：稿上 2px，这个量级跟分辨率无关（同 USER_DLG_BACKDROP_OPA 的处理） */
static const int32_t kBarRadius = 2;

/** 文件名最多显示多少个字符，超出中间省略 */
static const size_t kNameMaxChars = 24;

/* ---- 两稿共用的外壳与副按钮 -------------------------------------------- */

/**
 * ui_modal_dialog 的默认样式与这两稿有两处出入，就地覆盖。
 * 不动 USER_DLG_* 宏，否则会连带改掉打印流程那一批弹窗。
 *   - 卡内行距：宏是 USER_DLG_PAD(22/28)，稿上 16 → 用 USER_PAD_SM(11/14)
 *   - 遮罩不透明度：宏是 LV_OPA_70，稿上 0.5 → LV_OPA_50
 */
static ui_modal_dialog_t *open_shell(lv_obj_t *parent, const char *title)
{
    ui_modal_dialog_cfg_t cfg = {};
    cfg.title_text       = title;
    cfg.card_width       = USER_DLG_W;
    cfg.with_footer      = false;
    cfg.accent_underline = false;
    cfg.with_close_btn   = false;

    ui_modal_dialog_t *dlg = ui_modal_dialog_open(parent, &cfg);
    if(!dlg) {
        return nullptr;
    }
    lv_obj_set_style_bg_opa(dlg->root, LV_OPA_50, 0);
    lv_obj_set_style_pad_row(dlg->card, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(dlg->body, USER_PAD_SM, 0);
    return dlg;
}

/**
 * 稿上的「弹窗副按钮」：满宽、极淡的白底、白字居中。
 * ui_user_make_footer_ghost_btn 是固定宽 + 描边样式，对不上，这里另写一个。
 */
static lv_obj_t *make_secondary_btn(lv_obj_t *parent, const char *text,
                                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, USER_DLG_FOOTER_BTN_H);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, kFaintFillOpa, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, user_color_text, 0);
    lv_obj_set_style_text_font(lbl, user_font_text(), 0);
    lv_obj_center(lbl);
    return btn;
}

/* ============================================================================
 *  PIN 配对码弹窗（Figma 3859:9545）
 * ============================================================================ */

struct ui_xfr_pin_dialog_t {
    ui_modal_dialog_t  *dlg;
    ui_xfr_dialog_cb_t  on_cancel;
    void               *user_data;
};

/* 数字格宽与格间距按稿上的比例从卡片宽推出（稿：卡片 496 / 格 64 / 间距 16），
 * 这样三档分辨率下数字区与卡片的比例始终跟稿一致。 */
static int32_t pin_cell_w(void)   { return USER_DLG_W * 64 / 496; }
static int32_t pin_cell_gap(void) { return USER_DLG_W * 16 / 496; }

/** 稿上是 4 位数字；PIN 不足 4 位或混入非数字时显示 '-'，避免出现乱码 */
static char pin_digit_at(const char *pin, size_t i)
{
    if(!pin || i >= std::strlen(pin)) {
        return '-';
    }
    const char c = pin[i];
    return (c >= '0' && c <= '9') ? c : '-';
}

static void pin_cancel_event_cb(lv_event_t *e)
{
    auto *self = static_cast<ui_xfr_pin_dialog_t *>(lv_event_get_user_data(e));
    if(!self || !self->on_cancel) {
        return;
    }
    /* 回调内通常会把 self 关掉，先把要用的取出来再调 */
    ui_xfr_dialog_cb_t cb = self->on_cancel;
    void *u = self->user_data;
    cb(u);
}

ui_xfr_pin_dialog_t *ui_xfr_pin_dialog_open(lv_obj_t *parent, const char *pin,
                                            ui_xfr_dialog_cb_t on_cancel,
                                            void *user_data)
{
    if(!parent) {
        return nullptr;
    }
    auto *self = static_cast<ui_xfr_pin_dialog_t *>(
        std::calloc(1, sizeof(ui_xfr_pin_dialog_t)));
    if(!self) {
        return nullptr;
    }
    self->on_cancel = on_cancel;
    self->user_data = user_data;

    self->dlg = open_shell(parent, "Enter this PIN on your device");
    if(!self->dlg) {
        std::free(self);
        return nullptr;
    }

    /* ---- 四位数字 ----
     * 稿上每格 64 宽但没有底色，所以视觉上就是四个等距数字；这里也只给宽度不画底，
     * 靠固定格宽保证数字等距对齐（而不是让字形宽度决定间距）。 */
    lv_obj_t *row = lv_obj_create(self->dlg->body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, pin_cell_gap(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for(size_t i = 0; i < 4; ++i) {
        const char text[2] = { pin_digit_at(pin, i), '\0' };
        lv_obj_t *cell = lv_label_create(row);
        lv_label_set_text(cell, text);
        lv_obj_set_width(cell, pin_cell_w());
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(cell, user_color_accent, 0);
        /* 稿上 72px；固件里编进去的 Montserrat 最大就是 48，够用且零成本 */
        lv_obj_set_style_text_font(cell, &lv_font_montserrat_48, 0);
    }

    make_secondary_btn(self->dlg->body, "Cancel Pairing",
                       pin_cancel_event_cb, self);
    return self;
}

void ui_xfr_pin_dialog_close(ui_xfr_pin_dialog_t *self)
{
    if(!self) {
        return;
    }
    ui_modal_dialog_close(self->dlg);
    std::free(self);
}

/* ============================================================================
 *  文件接收进度弹窗（Figma 3859:9810）
 *
 *  注意：这一版只建 UI，没有接线——XFR 现在只在文件落盘之后发
 *  XFR_File_Received_Ntf，没有"传输中"和进度的消息，接哪儿都是假的。
 *  等 XFR 进度契约落地后，由 control.cpp 打开本弹窗并调
 *  ui_xfr_recv_dialog_set_progress() 驱动。
 * ============================================================================ */

struct ui_xfr_recv_dialog_t {
    ui_modal_dialog_t  *dlg;
    lv_obj_t           *bar;
    ui_xfr_dialog_cb_t  on_cancel;
    void               *user_data;
};

/** 稿上 8px；USER_PROGRESS_H(12/14) 是打印页那条粗条，这里取一半 */
static int32_t recv_bar_h(void) { return USER_PROGRESS_H / 2; }

/**
 * 文件名可能长到 127 字节（XfrFileReceivedPayload.file_name[128]），卡片放不下。
 * 稿上保留了扩展名，所以中间省略而不是尾部截断。
 */
static void shorten_file_name(char *out, size_t out_sz, const char *name)
{
    if(!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if(!name || !name[0]) {
        return;
    }
    const size_t len = std::strlen(name);
    if(len <= kNameMaxChars || out_sz <= kNameMaxChars) {
        std::snprintf(out, out_sz, "%s", name);
        return;
    }
    /* 尾段留够 ".stl" / ".json" 这类扩展名 */
    const size_t tail = 8;
    const size_t head = kNameMaxChars - tail - 3;
    std::snprintf(out, out_sz, "%.*s...%s",
                  static_cast<int>(head), name, name + len - tail);
}

static void recv_cancel_event_cb(lv_event_t *e)
{
    auto *self = static_cast<ui_xfr_recv_dialog_t *>(lv_event_get_user_data(e));
    if(!self || !self->on_cancel) {
        return;
    }
    ui_xfr_dialog_cb_t cb = self->on_cancel;
    void *u = self->user_data;
    cb(u);
}

ui_xfr_recv_dialog_t *ui_xfr_recv_dialog_open(lv_obj_t *parent, const char *file_name,
                                              ui_xfr_dialog_cb_t on_cancel,
                                              void *user_data)
{
    if(!parent) {
        return nullptr;
    }
    auto *self = static_cast<ui_xfr_recv_dialog_t *>(
        std::calloc(1, sizeof(ui_xfr_recv_dialog_t)));
    if(!self) {
        return nullptr;
    }
    self->on_cancel = on_cancel;
    self->user_data = user_data;

    /* 标题分两段上色（"Receiving" 白 + 文件名蓝），用不了外壳自带的单色标题，
     * 所以标题传 NULL，自己在 body 里拼一行。 */
    self->dlg = open_shell(parent, nullptr);
    if(!self->dlg) {
        std::free(self);
        return nullptr;
    }

    lv_obj_t *title_row = lv_obj_create(self->dlg->body);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prefix = lv_label_create(title_row);
    lv_label_set_text(prefix, "Receiving");
    lv_obj_set_style_text_color(prefix, user_color_text, 0);
    lv_obj_set_style_text_font(prefix, user_font_value(), 0);

    char shown[64];
    shorten_file_name(shown, sizeof(shown), file_name);
    lv_obj_t *name = lv_label_create(title_row);
    lv_label_set_text(name, shown);
    lv_obj_set_style_text_color(name, kFileNameColor, 0);
    lv_obj_set_style_text_font(name, user_font_value(), 0);

    /* ---- 进度条：槽极淡白底，填充走 #4258ff → #586bff 横向渐变 ---- */
    self->bar = lv_bar_create(self->dlg->body);
    lv_obj_remove_style_all(self->bar);
    lv_obj_set_width(self->bar, LV_PCT(100));
    lv_obj_set_height(self->bar, recv_bar_h());
    lv_obj_set_style_bg_color(self->bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(self->bar, kFaintFillOpa, LV_PART_MAIN);
    lv_obj_set_style_radius(self->bar, kBarRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(self->bar, user_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(self->bar, kGradEndColor, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(self->bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(self->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(self->bar, kBarRadius, LV_PART_INDICATOR);
    lv_bar_set_range(self->bar, 0, 100);
    lv_bar_set_value(self->bar, 0, LV_ANIM_OFF);

    make_secondary_btn(self->dlg->body, "Cancel Transfer",
                       recv_cancel_event_cb, self);
    return self;
}

void ui_xfr_recv_dialog_set_progress(ui_xfr_recv_dialog_t *self, int32_t percent)
{
    if(!self || !self->bar) {
        return;
    }
    if(percent < 0)   percent = 0;
    if(percent > 100) percent = 100;
    lv_bar_set_value(self->bar, percent, LV_ANIM_ON);
}

void ui_xfr_recv_dialog_close(ui_xfr_recv_dialog_t *self)
{
    if(!self) {
        return;
    }
    ui_modal_dialog_close(self->dlg);
    std::free(self);
}
