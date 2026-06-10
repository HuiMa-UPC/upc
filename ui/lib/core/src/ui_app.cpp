#include "ui_app.hpp"

#include "page_manager.hpp"
#include "ui_msg_proxy.hpp"
#include "camera_msg_proxy.hpp"
#include "camera_preview.hpp"
#include "ui_simple_page.hpp"

#include "page_user_home.hpp"
#include "page_user_history.hpp"
#include "page_user_control.hpp"
#include "page_user_settings.hpp"
#include "page_user_message.hpp"

#include "ui_theme.hpp"
#include "ui_keyboard.hpp"
#include "control.hpp"

namespace ui {

namespace {

void settings_page_exit(void)
{
    page_user_settings_dismiss_dialog();
    ui_keyboard_hide();
}

} // namespace

void app_init(lv_obj_t* screen) {
    ui_theme_init();

    lv_obj_set_style_bg_color(screen, user_color_bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    UiMsgProxy::instance().init();
    CameraMsgProxy::instance().init();
    CameraPreviewPlayer::instance().init();

    PageManager::instance().init(screen);

    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_home",
                                     page_user_home_create,
                                     page_user_home_refresh),
        LV_SYMBOL_HOME);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_history",
                                     page_user_history_create,
                                     page_user_history_refresh),
        LV_SYMBOL_LIST);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_control",
                                     page_user_control_create,
                                     page_user_control_refresh),
        LV_SYMBOL_SETTINGS);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_settings",
                                     page_user_settings_create,
                                     page_user_settings_refresh,
                                     settings_page_exit),
        LV_SYMBOL_DRIVE);
    PageManager::instance().register_page(
        std::make_unique<SimplePage>("user_message",
                                     page_user_message_create,
                                     page_user_message_refresh),
        LV_SYMBOL_BELL);

    ui_keyboard_init(screen);
    control_init(screen);

    PageManager::instance().switch_to("user_home");
}

} // namespace ui
