#ifndef UI_XFR_DIALOG_H
#define UI_XFR_DIALOG_H

#include "lvgl_wrap.hpp"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_xfr_dialog.hpp
 * @brief WiFi 传输（rongsu_xfr）的两个模态弹窗：PIN 配对码 / 文件接收进度
 *
 * 对齐 Figma 两稿：「配对码」6363:44046、「接收文件」6363:44211。
 * 两者共用同一外壳（ui_modal_dialog 的 light_theme + 一处行距覆盖），差异只在正文：
 *   - PIN 弹窗   ：四位数字 + "Cancel Pairing"
 *   - 接收弹窗   ：文件名 + 进度条 + "Cancel Transfer"
 *
 * 稿 6363:44364 另画了一版「文件名 + 转圈」的接收态，未落地：XFR 的 Uploading 与
 * Slicing 都带百分比，两态统一走进度条。
 *
 * 配对/传输是网络事件，任何时刻都可能到达，调用方应挂在 lv_layer_top()，
 * 而不是 control 的 g_overlay（后者在 IDLE 态是隐藏的）。
 *
 * 文案：稿上为中文，此处统一用英文。CJK 子集字库
 * （lib/assets/src/lv_font_cjk_data.c）只覆盖调试测试页用字，
 * 缺「备 / 码 / 消」三字，且只有单一 18px 字号。
 */

/** 取消按钮回调（在 LVGL 主线程内调用） */
typedef void (*ui_xfr_dialog_cb_t)(void *user_data);

typedef struct ui_xfr_pin_dialog_t  ui_xfr_pin_dialog_t;
typedef struct ui_xfr_recv_dialog_t ui_xfr_recv_dialog_t;

/**
 * 打开 PIN 配对码弹窗。
 *
 * @param parent    父容器，通常传 lv_layer_top()
 * @param pin       PIN 字符串；只取前 4 位数字，缺位/非法字符显示 "-"
 * @param on_cancel "Cancel Pairing" 回调，NULL 时按钮仍在但不做事。
 *                  回调只负责关窗——目前没有取消配对的契约，后端会话不受影响
 * @param user_data 透传给 on_cancel
 * @return 句柄；参数非法或分配失败返回 NULL
 */
ui_xfr_pin_dialog_t *ui_xfr_pin_dialog_open(lv_obj_t *parent, const char *pin,
                                            ui_xfr_dialog_cb_t on_cancel,
                                            void *user_data);

/** 关闭并销毁 PIN 弹窗；self 为 NULL 时直接返回 */
void ui_xfr_pin_dialog_close(ui_xfr_pin_dialog_t *self);

/**
 * 打开文件接收进度弹窗。
 *
 * @param parent    父容器，通常传 lv_layer_top()
 * @param file_name 文件名；过长时中间省略，保留扩展名
 * @param on_cancel "Cancel Transfer" 回调，NULL 时按钮仍在但不做事
 * @param user_data 透传给 on_cancel
 * @return 句柄；参数非法或分配失败返回 NULL
 */
ui_xfr_recv_dialog_t *ui_xfr_recv_dialog_open(lv_obj_t *parent, const char *file_name,
                                              ui_xfr_dialog_cb_t on_cancel,
                                              void *user_data);

/**
 * 刷新进度条（0~100，越界自动夹取）。
 * 等 XFR 进度契约落地后由消息回调驱动；在那之前无人调用。
 */
void ui_xfr_recv_dialog_set_progress(ui_xfr_recv_dialog_t *self, int32_t percent);

/** 关闭并销毁接收弹窗；self 为 NULL 时直接返回 */
void ui_xfr_recv_dialog_close(ui_xfr_recv_dialog_t *self);

#ifdef __cplusplus
}
#endif

#endif
