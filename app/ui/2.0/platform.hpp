#ifndef PLATFORM_H
#define PLATFORM_H

#include "lvgl_wrap.hpp"

#ifndef PLATFORM_TOUCH_DEVICE
#define PLATFORM_TOUCH_DEVICE "/dev/input/event1"
#endif

/** 显示旋转：LV_DISPLAY_ROTATION_0 / _90 / _180 / _270（见 platform.cpp） */
#ifndef PLATFORM_DISPLAY_ROTATION
#define PLATFORM_DISPLAY_ROTATION LV_DISPLAY_ROTATION_270
#endif

/** 1=触摸 raw X/Y 与屏幕轴向相反时启用 lv_evdev_set_swap_axes */
#ifndef PLATFORM_EVDEV_SWAP_AXES
#define PLATFORM_EVDEV_SWAP_AXES 1
#endif

/* 触摸 raw 轴反向：面板贴装方向与触控 IC 坐标原点相反时置 1。
 * 按 raw 轴（面板自身坐标，非屏幕方向）配置，因此改 PLATFORM_DISPLAY_ROTATION
 * 时这两项不用跟着动。swap_axes 的换轴由 platform.cpp 内部换算。 */
#ifndef PLATFORM_TOUCH_INVERT_RAW_X
#define PLATFORM_TOUCH_INVERT_RAW_X 1
#endif
#ifndef PLATFORM_TOUCH_INVERT_RAW_Y
#define PLATFORM_TOUCH_INVERT_RAW_Y 0
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
