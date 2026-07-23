#ifndef PLATFORM_H
#define PLATFORM_H

#include "lvgl_wrap.hpp"

#ifndef PLATFORM_TOUCH_DEVICE
#define PLATFORM_TOUCH_DEVICE "/dev/input/event2"
#endif

/** 显示旋转：LV_DISPLAY_ROTATION_0 / _90 / _180 / _270（见 platform.cpp） */
#ifndef PLATFORM_DISPLAY_ROTATION
#define PLATFORM_DISPLAY_ROTATION LV_DISPLAY_ROTATION_0
#endif

/** 1=触摸 raw X/Y 与屏幕轴向相反时启用 lv_evdev_set_swap_axes */
#ifndef PLATFORM_EVDEV_SWAP_AXES
#define PLATFORM_EVDEV_SWAP_AXES 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint32_t platform_tick_get(void);
int platform_init(void);
void platform_run_loop(void);

#ifdef __cplusplus
}
#endif

#endif
