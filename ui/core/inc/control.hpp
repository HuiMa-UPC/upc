#ifndef CONTROL_H
#define CONTROL_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file control.hpp
 * @brief 打印全流程：状态机 + 步骤 overlay UI + UiMsgProxy 后端适配（见 control.cpp）
 */

typedef enum {
    CONTROL_STATE_IDLE = 0,
    CONTROL_STATE_D1_INSERT_PROMPT,
    CONTROL_STATE_D2_DOOR_OPENING,
    CONTROL_STATE_D2_PURIFYING,
    CONTROL_STATE_D4_DOOR_CLOSE_CHECK,
    CONTROL_STATE_FILE_SELECT,
    CONTROL_STATE_FILE_CONFIRM,
    CONTROL_STATE_PRINTING,
} control_state_t;

typedef struct {
    uint32_t index;
    char     name[64];
    char     path[128];
    uint32_t duration_min;
    float    material_g;
} control_file_t;

typedef struct {
    int      progress_percent;
    uint32_t remaining_minutes;
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
} control_progress_t;

/** 在 screen 上创建全屏 overlay；内部完成 MsgProxy 订阅与文件列表预取 */
void control_init(lv_obj_t *parent);

void control_start(void);
void control_cancel(void);
bool control_is_printing(void);

/** 打印中由首页调用：终止任务二次确认 */
void control_terminate_request(void);

#ifdef __cplusplus
}
#endif

#endif
