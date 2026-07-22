#ifndef PLATFORM_H
#define PLATFORM_H

#include "lvgl_wrap.hpp"

#ifndef PLATFORM_TOUCH_DEVICE
#define PLATFORM_TOUCH_DEVICE "/dev/input/event2"
#endif

/** 鼠标（相对坐标）设备。用 by-id 稳定路径，避免 eventX 编号漂移。
 *  可用环境变量 LV_LINUX_EVDEV_MOUSE_DEVICE 覆盖；设为空串可禁用鼠标。 */
#ifndef PLATFORM_MOUSE_DEVICE
#define PLATFORM_MOUSE_DEVICE "/dev/input/by-id/usb-Logitech_USB_Receiver-if01-event-mouse"
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
