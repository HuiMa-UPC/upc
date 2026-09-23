#ifndef PAGE_USER_MONITOR_H
#define PAGE_USER_MONITOR_H

#include "lvgl_wrap.hpp"
#include "control.hpp"
#include "upc_contract.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file page_user_monitor.hpp
 * @brief 监控页（侧栏第 2 格）：摄像头 + 右栏三 Tab
 *
 * 待机态与打印态是同一页，不是两套界面——打印流程从选文件页确认后直接切到本页，
 * 摄像头全程不断流。稿：待机 6338:27049 一族、打印 6338:27240 一族、
 * Manual 6307:40977。
 *
 * **打印态的界面从首页搬到了这里。** `page_user_home.hpp` 里那一组同名导出
 * （`page_user_home_enter_printing_mode` 等）已经没有调用方，作为「首页打印态稿」
 * 到位前的存档留着，别再往那边接线，见该头文件的说明。
 */

lv_obj_t *page_user_monitor_create(lv_obj_t *parent);
void page_user_monitor_refresh(bool force);
/**
 * 离页收尾。两件事：
 *  - 清掉 `DebugMsgProxy` 的 axis / device / soc_led **单槽**回调（Manual 照明与轴
 *    读数在用，调试页也在用同一组）。不清的话从本页切去调试页，那边会静默失联；
 *  - 待机态停掉摄像头预览。**打印态不停**——打印中用户可能切去别的页看消息，
 *    回来时不该黑一下，且结果页要停在最后一帧上。
 */
void page_user_monitor_exit(void);

/* ---- 打印流程接线（由 control.cpp 调用，语义与原首页那一组一一对应）---- */

void page_user_monitor_enter_printing_mode(const control_file_t *file);
void page_user_monitor_exit_printing_mode(void);
void page_user_monitor_printing_update(const control_progress_t *p);
/**
 * 光路检测逐点进度。state 见 OpticalPathPointState：
 * Moving/Passed/Failed 切到 `OPTICAL PATH CHECK` 状态行并在右侧显示 n/5；
 * Complete 恢复 `Printing...`，进度条仍为 0 直到正式进度到达。
 */
void page_user_monitor_optical_path_update(uint8_t point_index, uint8_t point_total,
                                           uint8_t state);
/**
 * 打印结果（Print Complete / Print Terminated）。
 * 仍在本页内切：摄像头不停，只把 Job 卡的状态行与底部按钮组换掉。
 */
void page_user_monitor_enter_print_result(const control_print_result_t *result);
/**
 * 把取件弹窗里那条 `SLIDE TO RETRIEVE` 推回起点。
 * 取件开门流程被取消时用：结果页一直没收起，但滑块停在解锁位，不复位就滑不了第二次。
 */
void page_user_monitor_reset_result_slide(void);

/** 按 viewport 缩放预览。几何固定按**全屏宽**算，见 .cpp 里 cam_apply_layout 的说明。 */
void page_user_monitor_update_camera_preview_layout(uint32_t src_w, uint32_t src_h);
/** 摄像头层根容器（供 control.cpp 在预览区域内挂局部弹窗）；不可用时返回 nullptr。 */
lv_obj_t *page_user_monitor_print_preview_parent(void);

#ifdef __cplusplus
}
#endif

#endif
