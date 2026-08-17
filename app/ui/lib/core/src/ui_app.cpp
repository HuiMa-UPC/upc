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

#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "ui_device_state.hpp"
#include "ui_message_store.hpp"
#include "ui_alarm_state.hpp"
#include "control.hpp"
#include "nav_icons.h"

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

} // namespace

void app_init(lv_obj_t* screen) {
    ui_theme_init();
    raw_thumb_decoder_init();

    lv_obj_set_style_bg_color(screen, user_color_bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    UiMsgProxy::instance().init();
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
                                     page_user_control_refresh),
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

    PageManager::instance().switch_to("user_home");
}

} // namespace ui
