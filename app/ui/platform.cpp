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

    lv_display_set_rotation(disp, PLATFORM_DISPLAY_ROTATION);

    fprintf(stderr, "DRM OK %ux%u (rotation=%d)\n",
            (unsigned)lv_display_get_horizontal_resolution(disp),
            (unsigned)lv_display_get_vertical_resolution(disp),
            (int)PLATFORM_DISPLAY_ROTATION);
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
#if PLATFORM_EVDEV_SWAP_AXES
    lv_evdev_set_swap_axes(indev, true);
#endif
    fprintf(stderr, "touchscreen OK (%s)\n", dev);
    return 0;
}

/* ===== 灰色半透明水波纹光标 ===== */
#define CURSOR_RIPPLE_SIZE    44                 /* 水波纹最大直径(px) */
#define CURSOR_RIPPLE_COLOR   lv_color_hex(0xC8C8C8)   /* 浅灰，在深色 UI 上可见 */
#define CURSOR_OUTLINE_COLOR  lv_color_hex(0x303030)   /* 描边，保证浅色背景上也可见 */
#define CURSOR_DOT_SIZE       12                 /* 圆心小圆点直径(px) */

static void ripple_scale_cb(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, 0);
}

static void ripple_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* 一层水波纹：灰色半透明圆环，循环“放大 + 淡出”，delay 用于错开多层 */
static void make_ripple_ring(lv_obj_t *parent, int32_t size, uint32_t delay)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_flag(ring, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(ring, size, size);
    lv_obj_center(ring);

    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);          /* 只要圆环，不要填充 */
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, CURSOR_RIPPLE_COLOR, 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);       /* 淡出交给对象 opa 控制 */
    lv_obj_set_style_transform_pivot_x(ring, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(ring, size / 2, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);

    lv_anim_set_exec_cb(&a, ripple_scale_cb);
    lv_anim_set_values(&a, 40, 256);                          /* 15% -> 100% */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, ripple_opa_cb);
    lv_anim_set_values(&a, LV_OPA_80, LV_OPA_TRANSP);         /* 同步淡出 */
    lv_anim_start(&a);
}

/* 构建光标对象：中心灰色半透明小圆点 + 多层灰色水波纹 */
static lv_obj_t *make_ripple_cursor(void)
{
    lv_obj_t *cursor = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(cursor, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(cursor, CURSOR_RIPPLE_SIZE, CURSOR_RIPPLE_SIZE);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cursor, 0, 0);
    lv_obj_set_style_pad_all(cursor, 0, 0);
    /* 左上角热点 -> 圆心对准指针点 */
    lv_obj_set_style_translate_x(cursor, -CURSOR_RIPPLE_SIZE / 2, 0);
    lv_obj_set_style_translate_y(cursor, -CURSOR_RIPPLE_SIZE / 2, 0);

    make_ripple_ring(cursor, CURSOR_RIPPLE_SIZE, 0);
    make_ripple_ring(cursor, CURSOR_RIPPLE_SIZE, 400);
    make_ripple_ring(cursor, CURSOR_RIPPLE_SIZE, 800);

    /* 圆心固定的灰色半透明小圆点：浅灰填充 + 深色描边，深浅背景都可见 */
    lv_obj_t *dot = lv_obj_create(cursor);
    lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(dot, CURSOR_DOT_SIZE, CURSOR_DOT_SIZE);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, CURSOR_RIPPLE_COLOR, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, CURSOR_OUTLINE_COLOR, 0);
    lv_obj_set_style_border_opa(dot, LV_OPA_60, 0);

    return cursor;
}

/* 鼠标为可选设备：失败不影响触摸屏，仅打印告警。
 * 光标对象建在 top layer，保证跨页面切换常驻并置顶；仅挂到鼠标 indev，
 * 触摸 indev 不设光标，避免手指按下时光标瞬移。 */
static void init_mouse(lv_display_t *disp)
{
    const char *dev = getenv("LV_LINUX_EVDEV_MOUSE_DEVICE");
    if(!dev) {
        dev = PLATFORM_MOUSE_DEVICE;
    }
    if(!dev[0]) {
        fprintf(stderr, "mouse disabled\n");
        return;
    }

    lv_indev_t *mouse = lv_evdev_create(LV_INDEV_TYPE_POINTER, dev);
    if(!mouse) {
        fprintf(stderr, "mouse open failed: %s (continuing without mouse)\n", dev);
        return;
    }
    lv_indev_set_display(mouse, disp);

    lv_indev_set_cursor(mouse, make_ripple_cursor());

    fprintf(stderr, "mouse OK (%s)\n", dev);
}

int platform_init(void)
{
    lv_init();
    lv_tick_set_cb(platform_tick_get);

    lv_display_t *disp = init_drm_display();
    if(!disp) {
        return -1;
    }
    if(init_touchscreen(disp) != 0) {
        return -1;
    }
    init_mouse(disp);
    return 0;
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
