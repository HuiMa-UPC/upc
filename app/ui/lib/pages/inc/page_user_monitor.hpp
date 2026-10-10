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
 * Manual 6534:35742（旧稿 6307:40977）。
 *
 * 打印态界面在本页完成，不经过首页。
 */

lv_obj_t *page_user_monitor_create(lv_obj_t *parent);
void page_user_monitor_refresh(bool force);
/**
 * 离页收尾。四件事：
 *  - Manual 若正在长按点动，先发松手停，不等松手事件；
 *  - 收起数字键盘（Manual 的 Step / 占空比 / 打印参数输入框可能还开着）；
 *  - 清掉 `DebugMsgProxy` 的 axis / device / soc_led / inner_door_busy / fan_state
 *    **单槽**回调（Manual 照明、轴读数、内门与风扇在用，调试页也在用同一组）。不清的话
 *    从本页切去调试页，那边会静默失联。风扇状态另有 proxy 常驻缓存，清回调不丢帧；
 *  - 待机态停掉摄像头预览。**打印态不停**——打印中用户可能切去别的页看消息，
 *    回来时不该黑一下，且结果页要停在最后一帧上。
 */
void page_user_monitor_exit(void);

/* ---- 打印流程接线（由 control.cpp 调用）---- */

void page_user_monitor_enter_printing_mode(const control_file_t *file);
void page_user_monitor_exit_printing_mode(void);
void page_user_monitor_printing_update(const control_progress_t *p);
/**
 * 起印环境检查进度。按实际会跑的步骤更新 Job 卡标题与进度条；
 * `step=Ready` 切回 Printing 并把条复位到 0%（与打印层进度分开）。
 * Prepare 阶段不驱动缩略图蒙版。跳过的步骤后端不推，UI 直接看到下一步。
 * step / step_index / step_total / sub_index / sub_total 对齐 PrintPrepProgressPayload。
 */
void page_user_monitor_print_prep_update(uint8_t step, uint8_t step_index, uint8_t step_total,
                                         uint8_t sub_index, uint8_t sub_total);
/**
 * 打印结果（Print Complete / Print Terminated）。
 * 仍在本页内切：摄像头不停，只把 Job 卡的状态行与底部按钮组换掉。
 */
void page_user_monitor_enter_print_result(const control_print_result_t *result);
/**
 * 把取件弹窗里那条滑条（`SLIDE TO RETRIEVE`，自检阶段终止时是 `SLIDE TO UNLOCK`）推回起点。
 * 取件开门流程被取消时用：结果页一直没收起，但滑块停在解锁位，不复位就滑不了第二次。
 */
void page_user_monitor_reset_result_slide(void);
/**
 * 首页 RETRIEVE（稿 6683:34027）的落点：调用方先切到本页，再调这个，直接弹出取件弹窗。
 * 与本页底部那颗 RETRIEVE 走同一条路——吹粉中照样被 toast 拦下、自检阶段终止照样换文案。
 * 不在结果态（`phase_finished()` 为假）时什么也不做。
 */
void page_user_monitor_request_retrieve(void);

/**
 * 当前结果是否在自检（起印环境检查，Ready 之前）阶段终止：作业还没出光、舱里没有工件。
 * 这种结果不自动清粉、不提示先清粉，取件开门后也不走转运盒三步（control.cpp 的
 * transfer_box_flow_active()）。用户手动终止与自检失败被动终止都算。
 * 进结果页时按当时的 phase 记下，开始 / 退出打印态时清零；不在结果态时恒为 false。
 */
bool page_user_monitor_result_aborted_in_prep(void);

/**
 * 一键转运（Manual 的 Retrieve / 调试页 Transfer Demo，`control_transfer_demo_start()`）的准备阶段
 * 开始或结束时由 control.cpp 调用。准备期间 Glavo 在降 Z、开外门：Manual 的 Run、Plate 卡五颗钮与
 * 底部 OPEN 置灰，结束后按 `control_transfer_demo_pending()` 恢复。页面还没建时什么也不做。
 */
void page_user_monitor_transfer_demo_changed(void);

/**
 * 外门补关遮罩开关（调试页 Other → Print Gates → Clearance Mask）。纯 UI 本地标志，
 * 不发消息也不落盘，重启回到默认开。关掉立即撤下正在显示的遮罩；打开时若补关还在跑、
 * 本轮也没有未解除的报警，立即盖上。补关状态与报警照常记录，不受开关影响。
 */
void page_user_monitor_set_clearance_mask_enabled(bool on);
bool page_user_monitor_clearance_mask_enabled(void);

/** 按 viewport 缩放预览。几何固定按**全屏宽**算，见 .cpp 里 cam_apply_layout 的说明。 */
void page_user_monitor_update_camera_preview_layout(uint32_t src_w, uint32_t src_h);
/** 摄像头层根容器（供 control.cpp 在预览区域内挂局部弹窗）；不可用时返回 nullptr。 */
lv_obj_t *page_user_monitor_print_preview_parent(void);

#ifdef __cplusplus
}
#endif

#endif
