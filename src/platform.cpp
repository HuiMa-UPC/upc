#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "platform.hpp"

uint32_t platform_tick_get(void)
{
    static uint64_t start_ms = 0;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    const uint64_t now_ms = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    if(start_ms == 0) {
        start_ms = now_ms;
    }
    return (uint32_t)(now_ms - start_ms);
}

static lv_display_t *init_drm_display(void)
{
    lv_display_t *disp = lv_linux_drm_create();
    if(!disp) {
        fprintf(stderr, "lv_linux_drm_create failed\n");
        return NULL;
    }

    const char *card = getenv("LV_LINUX_DRM_CARD");
    char *auto_card = NULL;
    if(!card || !card[0]) {
        auto_card = lv_linux_drm_find_device_path();
        card = auto_card;
    }

    if(!card || lv_linux_drm_set_file(disp, card, -1) != LV_RESULT_OK) {
        fprintf(stderr, "DRM init failed (card=%s)\n", card ? card : "(null)");
        lv_free(auto_card);
        lv_display_delete(disp);
        return NULL;
    }
    lv_free(auto_card);

    /* 强制设置为 1024x600（目标屏幕分辨率） */
    lv_display_set_resolution(disp, 1024, 600);
    lv_display_set_physical_resolution(disp, 1024, 600);

    fprintf(stderr, "DRM OK %ux%u\n",
            (unsigned)lv_display_get_horizontal_resolution(disp),
            (unsigned)lv_display_get_vertical_resolution(disp));
    return disp;
}

static int init_touchscreen(lv_display_t *disp)
{
    const char *dev = getenv("LV_LINUX_EVDEV_POINTER_DEVICE");
    if(!dev || !dev[0]) {
        dev = PLATFORM_TOUCH_DEVICE;
    }

    lv_indev_t *indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, dev);
    if(!indev) {
        fprintf(stderr, "touchscreen open failed: %s\n", dev);
        return -1;
    }

    lv_indev_set_display(indev, disp);
    fprintf(stderr, "touchscreen OK (%s)\n", dev);
    return 0;
}

int platform_init(void)
{
    lv_init();
    lv_tick_set_cb(platform_tick_get);

    lv_display_t *disp = init_drm_display();
    if(!disp) {
        return -1;
    }
    return init_touchscreen(disp);
}

void platform_run_loop(void)
{
    while(1) {
        uint32_t delay = lv_timer_handler();
        if(delay < 1) {
            delay = 1;
        }
        if(delay > 20) {
            delay = 20;
        }
        usleep(delay * 1000);
    }
}
