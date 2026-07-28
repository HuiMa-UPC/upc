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

void page_user_home_enter_printing_mode(const control_file_t *file);
void page_user_home_exit_printing_mode(void);
void page_user_home_printing_update(const control_progress_t *p);

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
/** 按 viewport 宽度等比缩放预览（宽贴满，高超出的居中裁剪）；src_h=0 时用流高或 1280 默认 */
void page_user_home_update_camera_preview_layout(uint32_t src_w, uint32_t src_h);

#ifdef __cplusplus
}
#endif

#endif
