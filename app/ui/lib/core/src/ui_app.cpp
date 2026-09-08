#include "ui_app.hpp"

#include "page_manager.hpp"
#include "ui_msg_proxy.hpp"
#include "camera_msg_proxy.hpp"
#include "debug_msg_proxy.hpp"
#include "camera_preview.hpp"
#include "raw_thumb_decoder.hpp"
#include "ui_simple_page.hpp"

#include "page_user_home.hpp"
#include "page_user_history.hpp"
#include "page_user_control.hpp"
#include "page_user_settings.hpp"
#include "page_user_message.hpp"
#include "page_user_debug.hpp"
#include "page_user_timelapse.hpp"
#include "page_user_photo.hpp"

#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "ui_device_state.hpp"
#include "ui_message_store.hpp"
#include "ui_alarm_state.hpp"
#include "ui_clean_state.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "control.hpp"
#include "nav_icons.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace ui {

namespace {

void settings_page_exit(void)
{
    page_user_settings_dismiss_dialog();
    ui_keyboard_hide();
}

/** 同步侧栏消息图标上的红点：有未读，或还有未解除/未复位的 MCU 报警
 *  （当前报警表非空）就亮——进一次消息页只消未读，报警要处理完才灭。 */
void sync_message_badge(void)
{
    PageManager::instance().set_menu_badge(
        "user_message",
        ui_message_unread_count() > 0 || ui_alarm_current_count() > 0);
}

lv_obj_t* g_ota_screen = nullptr;
ui_modal_dialog_t* g_ota_prompt_dlg = nullptr;
ui_modal_dialog_t* g_ota_progress_dlg = nullptr;
lv_obj_t* g_ota_progress_bar = nullptr;
lv_obj_t* g_ota_progress_label = nullptr;
std::string g_ota_task_id;
bool g_ota_required = false;

void close_ota_prompt()
{
    if(g_ota_prompt_dlg) {
        ui_modal_dialog_close(g_ota_prompt_dlg);
        g_ota_prompt_dlg = nullptr;
    }
}

void close_ota_progress()
{
    if(g_ota_progress_dlg) {
        ui_modal_dialog_close(g_ota_progress_dlg);
        g_ota_progress_dlg = nullptr;
        g_ota_progress_bar = nullptr;
        g_ota_progress_label = nullptr;
    }
}

void ota_decision_cb(lv_event_t* e)
{
    const bool approve = reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0;
    if(!approve && g_ota_required) {
        return;
    }
    if(!g_ota_task_id.empty()) {
        UiMsgProxy::instance().send_ota_user_decision(g_ota_task_id.c_str(), approve);
    }
    close_ota_prompt();
}

void open_ota_prompt(const middleware::OtaNotifyPayload& p)
{
    if(!g_ota_screen) {
        return;
    }
    close_ota_prompt();
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("OTA Update Available", USER_DLG_W, true);
    g_ota_prompt_dlg = ui_modal_dialog_open(g_ota_screen, &cfg);
    if(!g_ota_prompt_dlg) {
        return;
    }
    char body[192];
    std::snprintf(body, sizeof(body),
                  "Version: %s\nTask: %s\nMode: %s",
                  p.release_version[0] ? p.release_version : "unknown",
                  p.task_id,
                  p.required ? "required" : "optional");
    lv_obj_t* lbl = ui_user_make_body_label(g_ota_prompt_dlg->body, body);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    if(!g_ota_required) {
        ui_user_make_confirm_footer_btn(g_ota_prompt_dlg->footer, "Cancel", false,
                                        user_color_text_dim, ota_decision_cb,
                                        reinterpret_cast<void*>(0));
    }
    ui_user_make_confirm_footer_btn(g_ota_prompt_dlg->footer, "Confirm", true,
                                    user_color_accent, ota_decision_cb,
                                    reinterpret_cast<void*>(1));
}

void ensure_ota_progress_dialog()
{
    if(g_ota_progress_dlg || !g_ota_screen) {
        return;
    }
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("OTA Download Progress", USER_DLG_W, false);
    g_ota_progress_dlg = ui_modal_dialog_open(g_ota_screen, &cfg);
    if(!g_ota_progress_dlg) {
        return;
    }
    g_ota_progress_label = ui_user_make_body_label(g_ota_progress_dlg->body, "Starting...");
    lv_obj_set_width(g_ota_progress_label, LV_PCT(100));
    lv_obj_set_style_text_align(g_ota_progress_label, LV_TEXT_ALIGN_CENTER, 0);
    g_ota_progress_bar = ui_user_make_progress_bar(g_ota_progress_dlg->body, LV_PCT(100), USER_PROGRESS_H);
    lv_bar_set_range(g_ota_progress_bar, 0, 100);
    lv_bar_set_value(g_ota_progress_bar, 0, LV_ANIM_OFF);
}

void on_ota_notify(const middleware::OtaNotifyPayload& p)
{
    g_ota_task_id = p.task_id;
    g_ota_required = (p.required != 0);
    open_ota_prompt(p);
}

void on_ota_progress(const middleware::OtaProgressPayload& p)
{
    if(p.task_id[0] != '\0') {
        g_ota_task_id = p.task_id;
    }
    ensure_ota_progress_dialog();
    if(!g_ota_progress_dlg || !g_ota_progress_bar || !g_ota_progress_label) {
        return;
    }
    const middleware::OtaProgressState state =
        static_cast<middleware::OtaProgressState>(p.state);
    const char* state_text = "Downloading";
    switch(state) {
    case middleware::OtaProgressState::Verifying: state_text = "Verifying"; break;
    case middleware::OtaProgressState::Applying: state_text = "Applying"; break;
    case middleware::OtaProgressState::Completed: state_text = "Completed"; break;
    case middleware::OtaProgressState::Failed: state_text = "Failed"; break;
    case middleware::OtaProgressState::RolledBack: state_text = "Rolled Back"; break;
    case middleware::OtaProgressState::Downloading:
    default: break;
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%s [%s] %u%%",
                  state_text,
                  p.component[0] ? p.component : "unknown",
                  static_cast<unsigned>(p.progress_pct));
    lv_label_set_text(g_ota_progress_label, text);
    lv_bar_set_value(g_ota_progress_bar, static_cast<int32_t>(p.progress_pct), LV_ANIM_OFF);

    if(state == middleware::OtaProgressState::Completed) {
        close_ota_prompt();
        ui_toast_show("OTA files ready", user_color_status_ok, 1800);
        close_ota_progress();
    } else if(state == middleware::OtaProgressState::Failed) {
        close_ota_prompt();
        char err[128];
        std::snprintf(err, sizeof(err), "OTA failed: %s",
                      p.error_code[0] ? p.error_code : "unknown");
        ui_toast_show(err, user_color_status_err, 2200);
        close_ota_progress();
    }
}

} // namespace

void app_init(lv_obj_t* screen) {
    ui_theme_init();
    raw_thumb_decoder_init();

    lv_obj_set_style_bg_color(screen, user_color_bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    UiMsgProxy::instance().init();
    UiMsgProxy::instance().register_ota_notify(on_ota_notify);
    UiMsgProxy::instance().register_ota_progress(on_ota_progress);
    CameraMsgProxy::instance().init();
    DebugMsgProxy::instance().init();
    CameraPreviewPlayer::instance().init();

    /* 设备名/联网/运行状态/环境快照的唯一来源，须在 UiMsgProxy::init() 之后、
     * 建常驻设备信息条（PageManager::init）与首页之前订阅上。 */
    ui_device_state_init();

    /* 消息中心也要赶在 PageManager::init() 之前——左下通知浮层就是在那里建的，
     * 建的时候要能读到当前消息集合。 */
    ui_message_store_init();

    /* MCU 报警：生命周期缓存 + 历史加载，喂消息中心（须在 store 之后） */
    ui_alarm_state_init();

    /* 清粉回执：接管两个单槽 Proxy 回调，再按发起方扇出给首页结果页与控制页。
     * 两个页面都在 on_create 里注册观察者，所以这里只要赶在建页之前。 */
    ui_clean_state_init();

    PageManager::instance().init(screen);

    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_home",
                                     page_user_home_create,
                                     page_user_home_refresh),
        &nav_icon_home);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_history",
                                     page_user_history_create,
                                     page_user_history_refresh,
                                     page_user_history_exit),
        &nav_icon_history);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_control",
                                     page_user_control_create,
                                     page_user_control_refresh,
                                     page_user_control_exit),
        &nav_icon_control);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_settings",
                                     page_user_settings_create,
                                     page_user_settings_refresh,
                                     settings_page_exit),
        &nav_icon_settings);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_message",
                                     page_user_message_create,
                                     page_user_message_refresh,
                                     page_user_message_exit),
        &nav_icon_message);

    /* 选择文件页：隐藏页（无侧边栏图标），归属首页菜单按钮，由「My Files」进入 */
    PageManager::instance().register_hidden_page(
        std::make_unique<SimplePage>("user_files",
                                     page_user_files_create,
                                     page_user_files_refresh,
                                     page_user_files_exit),
        "user_home");

    /* 调试页：隐藏页（无侧边栏图标），归属设置页菜单按钮；
     * 由 Settings → Debug（密码 "888"）进入 */
    PageManager::instance().register_hidden_page(
        std::make_unique<SimplePage>("user_debug",
                                     page_user_debug_create,
                                     page_user_debug_refresh,
                                     page_user_debug_exit),
        "user_settings");

    /* 延时摄影播放器：隐藏页（无侧边栏图标），归属历史页菜单按钮；
     * 由历史页点击缩略图进入 */
    PageManager::instance().register_hidden_page(
        std::make_unique<SimplePage>("user_timelapse",
                                     page_user_timelapse_create,
                                     page_user_timelapse_refresh,
                                     page_user_timelapse_exit),
        "user_history");

    /* 相册全屏浏览器：隐藏页（无侧边栏图标），归属历史页菜单按钮；
     * 由历史页 Album 点击缩略图进入 */
    PageManager::instance().register_hidden_page(
        std::make_unique<SimplePage>("user_photo",
                                     page_user_photo_create,
                                     page_user_photo_refresh,
                                     page_user_photo_exit),
        "user_history");

    /* 红点要等消息页注册完才挂得上（set_menu_badge 认页名）。
     * 报警观察者也要挂：报警解除/复位不走消息 store 的未读计数，
     * 只靠 ui_message 观察者的话红点不会跟着灭。 */
    ui_message_add_observer(sync_message_badge);
    ui_alarm_add_observer(sync_message_badge);
    sync_message_badge();

    /* 首页图标点击 → 首页区当前视图（仪表盘 / 选择文件页），实现“回到上次视图” */
    PageManager::instance().set_menu_target_resolver(
        "user_home",
        []() { return std::string(control_home_section_target()); });

    ui_keyboard_init(screen);
    control_init(screen);
    g_ota_screen = screen;

    PageManager::instance().switch_to("user_home");
}

} // namespace ui
