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
#include "page_user_monitor.hpp"
#include "page_user_settings.hpp"
#include "page_user_message.hpp"
#include "page_user_debug.hpp"
#include "page_user_timelapse.hpp"
#include "page_user_photo.hpp"
#include "page_user_files.hpp"

#include "ui_theme.hpp"
#include "ui_backlight.hpp"
#include "ui_keyboard.hpp"
#include "ui_device_state.hpp"
#include "ui_message_store.hpp"
#include "ui_alarm_state.hpp"
#include "ui_notice_stack.hpp"
#include "ui_clean_state.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "control.hpp"
#include "home_metric_icons.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ui {

namespace {

void settings_page_exit(void)
{
    page_user_settings_dismiss_dialog();
    ui_keyboard_hide();
}

/** 同步两颗红点（侧栏消息图标 + 首页右上铃铛）：有未读，或还有未解除的告警级消息
 *  就亮——进一次消息页只消未读，告警要处理完才灭。
 *
 *  判据从原先的「未读 || ui_alarm_current_count() > 0」收紧成了 store 里那个谓词，
 *  收的是两个边：旧判据只看 MCU 报警、漏掉后端 GLAVO_Device_Event_Ntf 推来的告警级
 *  事件；而且它不分等级，一条 INFO 级的 CONDITION 也会让红点常亮。新谓词扫的是
 *  store（报警也在里面，解除时会被 retract 掉），两个边一起收。
 *
 *  **和铃铛面板的取数共用同一个谓词**，所以「红点亮」与「面板里有东西」恒等价，
 *  不会出现点开是空的。 */
void sync_message_badge(void)
{
    const bool on = ui_message_needs_attention();
    PageManager::instance().set_menu_badge("user_message", on);
    ui_notice_stack_set_badge(on);
}

lv_obj_t* g_ota_screen = nullptr;
ui_modal_dialog_t* g_ota_prompt_dlg = nullptr;
ui_modal_dialog_t* g_ota_progress_dlg = nullptr;
ui_modal_dialog_t* g_soc_ota_dlg = nullptr;
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

void close_soc_ota_prompt()
{
    if(g_soc_ota_dlg) {
        ui_modal_dialog_close(g_soc_ota_dlg);
        g_soc_ota_dlg = nullptr;
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
    cfg.light_theme = true;
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
                                        user_color_text_dim, true, ota_decision_cb,
                                        reinterpret_cast<void*>(0));
    }
    ui_user_make_confirm_footer_btn(g_ota_prompt_dlg->footer, "Confirm", true,
                                    user_color_accent, true, ota_decision_cb,
                                    reinterpret_cast<void*>(1));
}

void ensure_ota_progress_dialog()
{
    if(g_ota_progress_dlg || !g_ota_screen) {
        return;
    }
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("OTA Download Progress", USER_DLG_W, false);
    cfg.light_theme = true;
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
        /* SOC 全包升级：进度拉满后不关窗、不弹成功提示，等 APP_Soc_Ota_Ready_Ntf
         * 把界面切成"是否重启升级"。其它组件保持原有的一闪即过提示。 */
        if(std::strcmp(p.component, "soc") == 0) {
            return;
        }
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

/**
 * 用户对已就绪 SOC 固件的处置。
 *
 * 打印中拦截"立即重启"：重启会毁掉正在进行的打印任务，这里只提示不关窗，
 * 用户打印结束后还能再点。"稍后"不发消息，MQTT 侧的待应用状态已持久化，
 * 下次开机连上云端会重新提示。
 */
void soc_ota_apply_cb(lv_event_t* e)
{
    const auto action = static_cast<middleware::SocOtaApplyAction>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));

    if(action == middleware::SocOtaApplyAction::RebootNow && control_is_printing()) {
        ui_toast_show("Printing in progress, cannot reboot",
                      user_color_status_err, 2200);
        return;
    }
    UiMsgProxy::instance().send_soc_ota_apply(action);
    close_soc_ota_prompt();
}

void soc_ota_later_cb(lv_event_t*)
{
    close_soc_ota_prompt();
}

void on_soc_ota_ready(const middleware::SocOtaReadyPayload& p)
{
    if(!g_ota_screen) {
        return;
    }
    /* 进度框让位给确认框：下载阶段的 100% 已经展示完了。 */
    close_ota_progress();
    close_ota_prompt();
    close_soc_ota_prompt();

    ui_modal_dialog_cfg_t cfg =
        ui_modal_dialog_cfg_user("System Update Ready", USER_DLG_W, true);
    cfg.light_theme = true;
    g_soc_ota_dlg = ui_modal_dialog_open(g_ota_screen, &cfg);
    if(!g_soc_ota_dlg) {
        return;
    }

    /* 应用层升级只换 /home 下的可执行文件，重启一次就好；全包要进 recovery
     * 刷整机，耗时长得多，文案上先给用户一个预期。 */
    const bool is_app =
        static_cast<middleware::SocOtaKind>(p.kind) == middleware::SocOtaKind::App;
    char body[224];
    std::snprintf(body, sizeof(body),
                  "Version: %s\nSize: %.1f MB\n%s",
                  p.version[0] ? p.version : "unknown",
                  static_cast<double>(p.img_bytes) / (1024.0 * 1024.0),
                  is_app ? "The device will restart to switch to the new version."
                         : "The device will reboot to install the update.");
    lv_obj_t* lbl = ui_user_make_body_label(g_soc_ota_dlg->body, body);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    ui_user_make_confirm_footer_btn(
        g_soc_ota_dlg->footer, "Discard", false, user_color_text_dim, true,
        soc_ota_apply_cb,
        reinterpret_cast<void*>(
            static_cast<intptr_t>(middleware::SocOtaApplyAction::Discard)));
    ui_user_make_confirm_footer_btn(
        g_soc_ota_dlg->footer, "Later", false, user_color_text_dim, true,
        soc_ota_later_cb, nullptr);
    ui_user_make_confirm_footer_btn(
        g_soc_ota_dlg->footer, "Reboot", true, user_color_accent, true,
        soc_ota_apply_cb,
        reinterpret_cast<void*>(
            static_cast<intptr_t>(middleware::SocOtaApplyAction::RebootNow)));
}

void on_soc_ota_apply_rsp(const middleware::SocOtaApplyRspPayload& p)
{
    const auto result = static_cast<middleware::SocOtaApplyResult>(p.result);
    if(result == middleware::SocOtaApplyResult::Ok) {
        /* RebootNow 成功时设备正在重启，没必要提示；只回执删除结果。 */
        if(static_cast<middleware::SocOtaApplyAction>(p.action) ==
           middleware::SocOtaApplyAction::Discard) {
            ui_toast_show("Update package removed", user_color_status_ok, 1800);
        }
        return;
    }
    char err[128];
    std::snprintf(err, sizeof(err), "Update failed: %s",
                  p.reason[0] ? p.reason : "unknown");
    ui_toast_show(err, user_color_status_err, 2200);
}

} // namespace

void app_init(lv_obj_t* screen) {
    ui_theme_init();
    ui::backlight_apply_saved();
    raw_thumb_decoder_init();

    lv_obj_set_style_bg_color(screen, user_color_bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    UiMsgProxy::instance().init();
    UiMsgProxy::instance().register_ota_notify(on_ota_notify);
    UiMsgProxy::instance().register_ota_progress(on_ota_progress);
    UiMsgProxy::instance().register_soc_ota_ready(on_soc_ota_ready);
    UiMsgProxy::instance().register_soc_ota_apply_rsp(on_soc_ota_apply_rsp);
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
        ic_nav_home_set);
    /* 监控页在侧栏排第 2（稿 6338:27240）。改版前这一格是控制页，控制页已退役。 */
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_monitor",
                                     page_user_monitor_create,
                                     page_user_monitor_refresh,
                                     page_user_monitor_exit),
        ic_nav_monitor_set);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_history",
                                     page_user_history_create,
                                     page_user_history_refresh,
                                     page_user_history_exit),
        ic_nav_history_set);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_settings",
                                     page_user_settings_create,
                                     page_user_settings_refresh,
                                     settings_page_exit),
        ic_nav_settings_set);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_message",
                                     page_user_message_create,
                                     page_user_message_refresh,
                                     page_user_message_exit),
        /* 第二个 set 是有未读时换上的那版：右上角咬掉一块给红点让位（稿 6255:95805） */
        ic_nav_message_set, ic_nav_message_dot_set);

    /* 选择文件页：隐藏页（无侧边栏图标），由首页 / 监控页的 PRINT 进入。这里登记的归属
     * 首页只是初值：每次进页 goto_file_selection() 会按入口改写，侧栏高亮跟着入口走。 */
    PageManager::instance().register_hidden_page(
        std::make_unique<SimplePage>("user_files",
                                     page_user_files_create,
                                     page_user_files_refresh,
                                     page_user_files_exit),
        "user_home");

    /* 调试页：隐藏页（无侧边栏图标），归属设置页菜单按钮；
     * 由 Settings → About 连点 Software Version 三次并输密码 "888" 后，再点 Debug 进入 */
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
     * 两个数据源都挂：报警的生命周期变化（解除、复位）有些只落在 ui_alarm_state 里，
     * 不一定同时动到 store，只订一边会让红点慢一拍。注册是幂等的，重复触发无害。 */
    ui_message_add_observer(sync_message_badge);
    ui_alarm_add_observer(sync_message_badge);
    sync_message_badge();

    /* 首页 / 监控页图标点击 → 该分区当前视图（分区本页 / 挂在该分区下的选择文件页），
     * 实现“回到上次视图”。选文件页同一时间只挂在其中一个分区下，见 control_section_target()。 */
    PageManager::instance().set_menu_target_resolver(
        "user_home",
        []() { return std::string(control_section_target("user_home")); });
    PageManager::instance().set_menu_target_resolver(
        "user_monitor",
        []() { return std::string(control_section_target("user_monitor")); });

    ui_keyboard_init(screen);
    control_init(screen);
    g_ota_screen = screen;

    PageManager::instance().switch_to("user_home");
}

} // namespace ui
