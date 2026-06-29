#include "page_user_settings.hpp"
#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_keyboard.hpp"
#include "ui_feedback.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_msg_proxy.hpp"
#include "msg_types.hpp"

#include <cstring>

#define UPC_SW_VERSION "v3.5"

/* 本工程仅迁移用户 Shell，未包含开发者调试 Shell。
 * 密码校验通过后提示开发者 Shell 不可用（接口保留，后续接入时替换此处）。 */

/**
 * @file page_user_settings.cpp
 * @brief 用户设置页（含 "进入开发者模式" 密码入口）
 *
 * 当前仅实现一个核心条目：
 *   [进入开发者模式] → 弹出密码对话框 → 校验通过后 app_shell_show_dev()
 *
 * 其余用户级设置（语言、声音、亮度...）后续按设计稿补齐。
 *
 * 密码：硬编码 "888"（与用户在 Cursor 对话中确认一致）。
 * 之后需要改成 NVRAM 存储 / 可由开发者修改时，把比较逻辑放到一个
 * settings_backend.{hpp,cpp} 模块即可，UI 文件无需改动。
 */

namespace {

constexpr const char *kDevPassword = "888";

lv_obj_t *g_dlg_mask    = nullptr;
lv_obj_t *g_dlg_box     = nullptr;
lv_obj_t *g_dlg_pwd_ta  = nullptr;
lv_obj_t *g_dlg_err_lbl = nullptr;

ui_modal_dialog_t *g_ota_dlg = nullptr;
lv_obj_t *g_ota_status_lbl = nullptr;
lv_obj_t *g_ota_bar = nullptr;
bool g_ota_proxy_wired = false;

struct OtaUiUpdate {
    middleware::McuOtaStatusPayload status;
};

void ota_close_dialog(void);

const char *ota_state_text(middleware::McuOtaState state)
{
    switch(state) {
    case middleware::McuOtaState::Preparing:
        return "Preparing OTA...";
    case middleware::McuOtaState::Transferring:
        return "Transferring firmware...";
    case middleware::McuOtaState::Verifying:
        return "Verifying on MCU...";
    case middleware::McuOtaState::Success:
        return "OTA successful";
    case middleware::McuOtaState::Failed:
        return "OTA failed";
    case middleware::McuOtaState::Cancelled:
        return "OTA cancelled";
    default:
        return "OTA...";
    }
}

void ota_ui_apply_async(void *user)
{
    auto *upd = static_cast<OtaUiUpdate *>(user);
    if(!upd) {
        return;
    }
    if(g_ota_status_lbl) {
        char buf[96];
        const int pct = (upd->status.progress * 100) / 255;
        std::snprintf(buf, sizeof(buf), "%s (%d%%)",
                      ota_state_text(upd->status.state), pct);
        lv_label_set_text(g_ota_status_lbl, buf);
    }
    if(g_ota_bar) {
        lv_bar_set_value(g_ota_bar, upd->status.progress, LV_ANIM_OFF);
    }
    delete upd;
}

void wire_ota_proxy_once()
{
    if(g_ota_proxy_wired) {
        return;
    }
    g_ota_proxy_wired = true;
    ui::UiMsgProxy::instance().register_mcu_ota_status(
        [](const middleware::McuOtaStatusPayload &st) {
            auto *upd = new OtaUiUpdate{};
            upd->status = st;
            lv_async_call(ota_ui_apply_async, upd);
        });
}

/* TEST ONLY: 右上角 X 中止 SOC OTA 会话，便于联调卡死退出 */
void on_ota_close(lv_event_t *)
{
    ui::UiMsgProxy::instance().send_mcu_ota_cancel();
    ota_close_dialog();
}

void open_ota_dialog(void)
{
    if(g_ota_dlg) {
        return;
    }
    wire_ota_proxy_once();

    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("MCU OTA (Test)", USER_DLG_W, false);
    cfg.with_close_btn = true;
    g_ota_dlg = ui_modal_dialog_open(lv_screen_active(), &cfg);
    if(!g_ota_dlg) {
        return;
    }
    if(g_ota_dlg->close_btn) {
        lv_obj_add_event_cb(g_ota_dlg->close_btn, on_ota_close,
                            LV_EVENT_CLICKED, nullptr);
    }

    g_ota_status_lbl = ui_user_make_body_label(g_ota_dlg->body, "Starting...");
    g_ota_bar = ui_user_make_progress_bar(
        g_ota_dlg->body, LV_PCT(100), USER_PROGRESS_H);
    lv_bar_set_value(g_ota_bar, 0, LV_ANIM_OFF);
}

void ota_close_dialog(void)
{
    if(g_ota_dlg) {
        ui_modal_dialog_close(g_ota_dlg);
        g_ota_dlg         = nullptr;
        g_ota_status_lbl  = nullptr;
        g_ota_bar         = nullptr;
    }
}

void on_ota_entry_click(lv_event_t *)
{
    open_ota_dialog();
    ui::UiMsgProxy::instance().send_mcu_ota_start();
}

void dlg_close(void);

void on_dlg_cancel(lv_event_t *)
{
    dlg_close();
}

void on_dlg_confirm(lv_event_t *)
{
    if(!g_dlg_pwd_ta) {
        return;
    }
    const char *input = lv_textarea_get_text(g_dlg_pwd_ta);
    if(input && std::strcmp(input, kDevPassword) == 0) {
        dlg_close();
        ui_toast_show("Developer shell not available in this build",
                      user_color_accent, 2000);
        return;
    }
    if(g_dlg_err_lbl) {
        lv_label_set_text(g_dlg_err_lbl, "Wrong password");
        lv_obj_remove_flag(g_dlg_err_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_dlg_pwd_ta) {
        lv_textarea_set_text(g_dlg_pwd_ta, "");
    }
}

void on_dlg_mask_click(lv_event_t *e)
{
    /* 仅点击遮罩本身关闭（点到对话框子元素时 target != mask） */
    if(lv_event_get_target_obj(e) == g_dlg_mask) {
        dlg_close();
    }
}

void dlg_close(void)
{
    ui_keyboard_hide();
    if(g_dlg_mask) {
        lv_obj_delete(g_dlg_mask);
        g_dlg_mask    = nullptr;
        g_dlg_box     = nullptr;
        g_dlg_pwd_ta  = nullptr;
        g_dlg_err_lbl = nullptr;
    }
}

void open_dev_password_dialog(void)
{
    if(g_dlg_mask) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();

    /* 全屏半透明遮罩，吃掉背景点击 */
    g_dlg_mask = lv_obj_create(scr);
    lv_obj_remove_style_all(g_dlg_mask);
    lv_obj_set_size(g_dlg_mask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_dlg_mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_dlg_mask, LV_OPA_60, 0);
    lv_obj_clear_flag(g_dlg_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_dlg_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_dlg_mask, on_dlg_mask_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(g_dlg_mask);

    /* 居中对话框 */
    g_dlg_box = lv_obj_create(g_dlg_mask);
    lv_obj_remove_style_all(g_dlg_box);
    lv_obj_set_size(g_dlg_box, UI_CONFIRM_BOX_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_dlg_box, user_color_panel, 0);
    lv_obj_set_style_bg_opa(g_dlg_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_dlg_box, USER_RADIUS, 0);
    lv_obj_set_style_border_width(g_dlg_box, 1, 0);
    lv_obj_set_style_border_color(g_dlg_box, user_color_divider, 0);
    lv_obj_set_style_pad_all(g_dlg_box, USER_PAD, 0);
    lv_obj_set_flex_flow(g_dlg_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_dlg_box, USER_PAD_SM, 0);
    lv_obj_align(g_dlg_box, LV_ALIGN_TOP_MID, 0, USER_PAD_LG);
    lv_obj_clear_flag(g_dlg_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_dlg_box);
    lv_label_set_text(title, "Enter Developer Mode");
    lv_obj_set_style_text_color(title, user_color_text, 0);
    lv_obj_set_style_text_font(title, user_font_text(), 0);

    lv_obj_t *hint = lv_label_create(g_dlg_box);
    lv_label_set_text(hint, "Enter access password:");
    lv_obj_set_style_text_color(hint, user_color_text_dim, 0);
    lv_obj_set_style_text_font(hint, user_font_caption(), 0);

    g_dlg_pwd_ta = lv_textarea_create(g_dlg_box);
    lv_textarea_set_one_line(g_dlg_pwd_ta, true);
    lv_textarea_set_password_mode(g_dlg_pwd_ta, true);
    lv_textarea_set_placeholder_text(g_dlg_pwd_ta, "Password");
    lv_obj_set_width(g_dlg_pwd_ta, LV_PCT(100));
    lv_obj_set_height(g_dlg_pwd_ta, UI_INPUT_H);
    lv_obj_set_style_text_font(g_dlg_pwd_ta, user_font_text(), 0);
    lv_obj_set_style_bg_color(g_dlg_pwd_ta, user_color_bg, 0);
    lv_obj_set_style_border_color(g_dlg_pwd_ta, user_color_divider, 0);
    lv_obj_set_style_text_color(g_dlg_pwd_ta, user_color_text, 0);
    ui_keyboard_attach(g_dlg_pwd_ta, LV_KEYBOARD_MODE_NUMBER);

    g_dlg_err_lbl = lv_label_create(g_dlg_box);
    lv_label_set_text(g_dlg_err_lbl, "");
    lv_obj_set_style_text_color(g_dlg_err_lbl, ui_color_danger, 0);
    lv_obj_set_style_text_font(g_dlg_err_lbl, user_font_caption(), 0);
    lv_obj_add_flag(g_dlg_err_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_row = lv_obj_create(g_dlg_box);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, USER_PAD_SM, 0);

    lv_obj_t *btn_cancel = lv_button_create(btn_row);
    lv_obj_add_style(btn_cancel, user_style_btn_ghost(), 0);
    lv_obj_set_size(btn_cancel, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
    lv_obj_add_event_cb(btn_cancel, on_dlg_cancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_lbl = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    lv_obj_t *btn_ok = lv_button_create(btn_row);
    lv_obj_add_style(btn_ok, user_style_btn_primary(), 0);
    lv_obj_set_size(btn_ok, USER_BTN_PRIMARY_W, USER_BTN_PRIMARY_H);
    lv_obj_add_event_cb(btn_ok, on_dlg_confirm, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ok_lbl = lv_label_create(btn_ok);
    lv_label_set_text(ok_lbl, "Enter");
    lv_obj_center(ok_lbl);
}

void on_dev_entry_click(lv_event_t *)
{
    open_dev_password_dialog();
}

} /* namespace */

void page_user_settings_refresh(bool /*force*/)
{
    /* 占位 */
}

void page_user_settings_dismiss_dialog(void)
{
    dlg_close();
    ota_close_dialog();
}

lv_obj_t *page_user_settings_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, USER_PAD, 0);
    lv_obj_set_style_pad_row(page, USER_PAD, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, user_color_text, 0);
    lv_obj_set_style_text_font(title, user_font_value(), 0);

    lv_obj_t *card = ui_user_make_frosted_card(page, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    ui_user_make_setting_row(card, "Brightness", nullptr, nullptr);
    ui_user_make_setting_row(card, "Sound", nullptr, nullptr);
    ui_user_make_setting_row(card, "Network", nullptr, nullptr);
    ui_user_make_setting_row(card, "About", nullptr, nullptr);
    ui_user_make_setting_row(card, "MCU OTA (Test)", "Start", on_ota_entry_click);
    ui_user_make_setting_row(card, "Developer Mode", "Enter", on_dev_entry_click);

    /* 软件版本信息行：左侧标签 + 右侧版本号（纯文本，无功能） */
    lv_obj_t *ver_row = ui_user_make_setting_row(card, "Software Version", nullptr, nullptr);
    lv_obj_t *ver_val = lv_label_create(ver_row);
    lv_label_set_text(ver_val, UPC_SW_VERSION);
    lv_obj_set_style_text_color(ver_val, user_color_text_dim, 0);
    lv_obj_set_style_text_font(ver_val, user_font_text(), 0);

    return page;
}
