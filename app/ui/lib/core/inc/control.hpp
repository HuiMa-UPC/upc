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

/* 流程顺序：
 *   选文件页 → Start Print → PREPARE → D1 基板 → D2 PM2.5/开门 → D4 关门
 *   → CONFIRM 二次确认 → PRINTING（预览 + Print Start）
 */
typedef enum {
    CONTROL_STATE_IDLE = 0,
    CONTROL_STATE_PREPARE_WAIT,
    CONTROL_STATE_D1_INSERT_PROMPT,
    CONTROL_STATE_D2_DOOR_OPENING,
    CONTROL_STATE_D2_PURIFYING,
    CONTROL_STATE_D4_DOOR_CLOSE_CHECK,
    CONTROL_STATE_CONFIRM_START,
    CONTROL_STATE_PRINTING,
} control_state_t;

typedef struct {
    uint32_t index;
    char     name[64];
    char     path[128];
    uint32_t duration_min;
    float    material_g;
    uint32_t file_size;
    uint32_t line_count;
} control_file_t;

typedef struct {
    int      progress_percent;
    uint32_t remaining_minutes;
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
} control_progress_t;

void control_init(lv_obj_t *parent);

void control_start(void);
void control_cancel(void);
bool control_is_printing(void);

lv_obj_t *page_user_files_create(lv_obj_t *parent);
void      page_user_files_refresh(bool force);
void      page_user_files_exit(void);

const char *control_home_section_target(void);

void control_terminate_request(void);

#ifdef __cplusplus
}
#endif

#endif
