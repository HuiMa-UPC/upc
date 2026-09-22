#ifndef PAGE_USER_HOME_H
#define PAGE_USER_HOME_H

#include "lvgl_wrap.hpp"
#include "control.hpp"
#include "upc_contract.hpp"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *page_user_home_create(lv_obj_t *parent);
void page_user_home_refresh(bool force);

/* ===========================================================================
 *  以下这一组目前**没有调用方**，是存档不是活代码。
 *
 *  打印态与待机监控态整套已经搬到 `page_user_monitor.*`（新稿 6338:27240 一族把
 *  它们做成了独立一页，监控入口也从首页左下挪到了侧栏第 2 格）。`control.cpp` 现在
 *  调的是 `page_user_monitor_*` 那一组同名函数，本组连同 `.cpp` 里的 `g_print_area`
 *  / `g_monitor_area` 两层一起留着，等「首页打印态」那张稿到位后再决定是删是改。
 *
 *  **别再往这边接线**：两层都建在首页上，重新接通会和监控页争同一个
 *  `CameraPreviewPlayer` 单例。
 * =========================================================================== */

void page_user_home_enter_printing_mode(const control_file_t *file);
void page_user_home_exit_printing_mode(void);
void page_user_home_printing_update(const control_progress_t *p);
/**
 * 光路检测逐点进度。state 见 OpticalPathPointState：
 * Moving/Passed/Failed 切到 OPTICAL PATH CHECK 徽标并用层号位显示 n/5；
 * Complete 恢复 PRINTING 徽标，进度条仍为 0 直到正式进度到达。
 */
void page_user_home_optical_path_update(uint8_t point_index, uint8_t point_total,
                                       uint8_t state);

/**
 * 打印结果浮层（PRINT COMPLETE / PRINT TERMINATED）。
 * 在打印中浮层内部切换：摄像头预览不停，只把底部双条换成结果页元素。
 * 由 control.cpp 收到 Camera 的 manifest 通知后调用；退出统一走
 * page_user_home_exit_printing_mode()。
 */
void page_user_home_enter_print_result(const control_print_result_t *result);
/**
 * 把结果页的 "SWIPE TO COLLECT" 滑块推回起点。
 * 取件开门流程被取消时用：结果页浮层一直没收起，但滑块停在解锁位，
 * 不复位的话退回来就没法再滑第二次。
 */
void page_user_home_reset_result_slide(void);
void page_user_home_request_camera_preview(void);
/* --------- 以上为存档（无调用方），见本组开头那段说明 --------- */
/** 按 viewport 宽度等比缩放预览（宽贴满，高超出的居中裁剪）；src_h=0 时用流高或 1280 默认。
 *  src_w/src_h 传的是**上屏图像**尺寸（`CameraPreviewPlayer::stream_width()` 等），
 *  与 viewport 一致时 zoom 为 1.000，走 lv_image 无缩放贴图快路径。
 *  打印态与监控态两个摄像头层会一起刷新。 */
void page_user_home_update_camera_preview_layout(uint32_t src_w, uint32_t src_h);
/** 打印预览层根容器（用于在预览区域内挂载局部弹窗）；不可用时返回 nullptr。 */
lv_obj_t *page_user_home_print_preview_parent(void);

#ifdef __cplusplus
}
#endif

#endif
