#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>

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

/* 判断某个 evdev 节点是否为鼠标：能上报相对 X/Y 位移即视为鼠标。
 * 这样与厂商/型号/命名无关，任何鼠标（Logitech、小米、有线/无线）都能识别。 */
static bool evdev_is_mouse(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        return false;
    }

    unsigned long ev_bits = 0;
    unsigned long rel_bits = 0;
    bool is_mouse = false;

    /* REL_X / REL_Y / EV_REL 编号都 < 32，用单个 unsigned long 判断足够 */
    if(ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), &ev_bits) >= 0 &&
       (ev_bits & (1UL << EV_REL))) {
        if(ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), &rel_bits) >= 0) {
            if((rel_bits & (1UL << REL_X)) && (rel_bits & (1UL << REL_Y))) {
                is_mouse = true;
            }
        }
    }

    close(fd);
    return is_mouse;
}

/* 扫描 /dev/input/event* 找到第一个鼠标设备，写入 out。找到返回 true。 */
static bool find_mouse_device(char *out, size_t out_sz)
{
    DIR *dir = opendir("/dev/input");
    if(!dir) {
        return false;
    }

    bool found = false;
    struct dirent *ent;
    while((ent = readdir(dir)) != NULL) {
        if(strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[64];
        int n = snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        if(n < 0 || (size_t)n >= sizeof(path)) {
            /* 名字过长导致路径被截断，跳过该设备 */
            continue;
        }
        if(evdev_is_mouse(path)) {
            snprintf(out, out_sz, "%s", path);
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

/* 给一个鼠标 indev 挂上水波纹光标，并在设备移除时自动删除光标。 */
static void attach_ripple_cursor(lv_indev_t *mouse);

/* 设备被移除（拔出鼠标）时删除对应光标对象。 */
static void mouse_indev_deleted_cb(lv_event_t *e)
{
    lv_obj_t *cursor = (lv_obj_t *)lv_event_get_user_data(e);
    if(cursor) {
        lv_obj_delete(cursor);
    }
}

static void attach_ripple_cursor(lv_indev_t *mouse)
{
    lv_obj_t *cursor = make_ripple_cursor();
    lv_indev_set_cursor(mouse, cursor);
    /* 拔出时随 indev 一起删除光标，避免残留 */
    lv_indev_add_event_cb(mouse, mouse_indev_deleted_cb, LV_EVENT_DELETE, cursor);
}

/* 热插拔发现回调：LVGL 扫描到（或新插入）输入设备时调用。
 * 只有相对指针设备（鼠标, LV_EVDEV_TYPE_REL）才挂光标；
 * 已手动创建的触摸屏会被 LVGL 内置去重跳过，不会重复。 */
static void mouse_discovery_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
    lv_display_t *disp = (lv_display_t *)user_data;
    lv_indev_set_display(indev, disp);

    if(type == LV_EVDEV_TYPE_REL) {
        attach_ripple_cursor(indev);
        fprintf(stderr, "mouse discovered (hotplug), cursor attached\n");
    }
}

/* 鼠标为可选设备：失败不影响触摸屏，仅打印告警。
 * 光标对象建在 top layer，保证跨页面切换常驻并置顶；仅挂到鼠标 indev，
 * 触摸 indev 不设光标，避免手指按下时光标瞬移。
 *
 * 设备选择策略：
 *   1) 环境变量 LV_LINUX_EVDEV_MOUSE_DEVICE 显式指定 -> 固定该设备（不启用热插拔）；
 *      设为空串则禁用鼠标。
 *   2) 未设置 -> 启用 LVGL 热插拔发现，任何插入的鼠标（任意厂商）都会被自动识别并挂光标，
 *      拔出后自动移除；启动时已连接的鼠标也会被立即发现。
 *   3) 发现机制启动失败时，回退为一次性能力扫描；扫描不到鼠标则不启用。 */
static void init_mouse(lv_display_t *disp)
{
    const char *dev = getenv("LV_LINUX_EVDEV_MOUSE_DEVICE");

    /* 情况 1：显式指定设备（含设空串禁用） */
    if(dev) {
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
        attach_ripple_cursor(mouse);
        fprintf(stderr, "mouse OK (%s, fixed)\n", dev);
        return;
    }

    /* 情况 2：启用热插拔发现（推荐，兼容任意鼠标、支持插拔） */
    if(lv_evdev_discovery_start(mouse_discovery_cb, disp) == LV_RESULT_OK) {
        fprintf(stderr, "mouse hotplug discovery started\n");
        return;
    }

    /* 情况 3：发现机制启动失败，回退为一次性能力扫描 */
    fprintf(stderr, "mouse discovery start failed, fallback to one-shot scan\n");
    char auto_path[64];
    if(!find_mouse_device(auto_path, sizeof(auto_path))) {
        fprintf(stderr, "no mouse found (continuing without mouse)\n");
        return;
    }
    lv_indev_t *mouse = lv_evdev_create(LV_INDEV_TYPE_POINTER, auto_path);
    if(!mouse) {
        fprintf(stderr, "mouse open failed: %s (continuing without mouse)\n", auto_path);
        return;
    }
    lv_indev_set_display(mouse, disp);
    attach_ripple_cursor(mouse);
    fprintf(stderr, "mouse OK (%s)\n", auto_path);
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
