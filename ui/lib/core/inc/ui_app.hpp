#ifndef UI_APP_HPP
#define UI_APP_HPP

#include "lvgl_wrap.hpp"

namespace ui {

/** 初始化 PageManager、UiMsgProxy 并注册 PageDemo */
void app_init(lv_obj_t* screen);

} // namespace ui

#endif
