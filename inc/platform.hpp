#ifndef PLATFORM_H
#define PLATFORM_H

#include "lvgl_wrap.hpp"

/** 本机触摸屏节点（已确认 event2，不做自动扫描） */
#ifndef PLATFORM_TOUCH_DEVICE
#define PLATFORM_TOUCH_DEVICE "/dev/input/event2"
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
