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

/* 流程顺序（对齐 Figma「打印前预确认」1.1.x 弹窗序列）：
 *   My Files → PLATE_PROMPT（是否已放入打印版：两卡片入口）
 *     ├─ 已放入 → 选择打印任务 → 选文件页
 *     └─ 未放入 → 滑动开门 → DOOR_OPENING（PM2.5/开门 loading）
 *                 → DOOR_CONFIRM（门已开启等待 → 关门 Rsp → 用户确认关门）→ 选文件页
 *   选文件页 → Start Print → PREPARE → PRINTING（预览 + Print Start）
 *              → PRINT_DONE（打印结束，预览保活等 Camera 出片）→ 结果页 → IDLE
 */
typedef enum {
    CONTROL_STATE_IDLE = 0,
    CONTROL_STATE_PLATE_PROMPT,     /**< 入口：是否已放入打印版（两卡片 + 已放入/未放入子视图） */
    CONTROL_STATE_DOOR_OPENING,     /**< 未放入分支：PM2.5 检测 + 开门 loading */
    CONTROL_STATE_DOOR_CONFIRM,     /**< 舱门：DoorOpened 等待关门监测 → DoorClosed 用户确认 */
    CONTROL_STATE_PREPARE_WAIT,     /**< 选文件后：预处理 loading */
    CONTROL_STATE_PRINTING,         /**< 打印中（预览 + 进度） */
    /**
     * 打印结束（完成或终止）后不直接回 IDLE：摄像头预览保活，界面先维持打印中不变，
     * 等 Camera 的 APP_Print_Video_Manifest_Ntf 到达后切结果页浮层。
     * 两个出口：右上圆钮直接回 IDLE；滑动取件回 IDLE 并走开门流程。
     */
    CONTROL_STATE_PRINT_DONE,
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

/** 打印结果页展示数据；来源见 control.cpp on_print_video_manifest()。 */
typedef struct {
    bool     terminated;    /**< true=PRINT TERMINATED，false=PRINT COMPLETE */
    bool     has_stats;     /**< manifest 无对应条目（0 帧）时为 false，耗时/耗材显示 "--" */
    char     job_name[64];
    char     video_dir[96]; /**< 录像目录键（通常等于 manifest.frames_dir） */
    uint32_t duration_sec;
    float    material_g;    /**< <0 视为无数据 */
} control_print_result_t;

void control_init(lv_obj_t *parent);

void control_start(void);
void control_cancel(void);
/** 打印占用中（PRINTING 或 PRINT_DONE）：期间禁止进文件页开新任务 */
bool control_is_printing(void);

/* ---- 打印结果页（CONTROL_STATE_PRINT_DONE）的两个出口 ---- */
/** 右上圆钮：直接退出打印状态回待机首页（不开门） */
void control_print_result_dismiss(void);
/** 滑动取件：退出打印状态后走开门流程（PM2.5/最短停留/关门轮询） */
void control_print_result_collect(void);

/* 首页/监控「SWIPE TO OPEN」独立开门入口：走门弹窗流程（PM2.5/最短停留/关门轮询），
* 确认关门后回首页。仅 IDLE 态生效。 */
void control_open_door_request(void);

lv_obj_t *page_user_files_create(lv_obj_t *parent);
void      page_user_files_refresh(bool force);
void      page_user_files_exit(void);

const char *control_home_section_target(void);

void control_terminate_request(void);

#ifdef __cplusplus
}
#endif

#endif
