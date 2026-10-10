#ifndef UI_PREFS_HPP
#define UI_PREFS_HPP

#include <stdbool.h>

/**
 * @file ui_prefs.hpp
 * @brief 设置页「偏好设置 / 隐私」那几枚开关的落盘值（重启保留）。
 *
 * 存在 `/userdata/ui/prefs.conf`，一行一条 `key=value`；缺文件或缺键按默认值。默认值除
 * auto_record 外与稿 6457:17430 / 6448:53385 上的初始状态一致；auto_record 稿上是关，这里
 * 取开，与 Glavo 上电默认一致（关了 Print History 就不出打印视频）。只在 UI 线程读写。
 *
 * 五项里只有两项真的生效：
 *  - `auto_clean`：结果页进场是否自动开一轮吹粉（page_user_monitor_enter_print_result）；
 *  - `auto_record`：就是调试页 Print H264 Rec（打印期连续 H264 录像）。Glavo 不存这个值，
 *    每次启动都回到上电默认（开），所以由本模块在 Glavo 可达时补发一次（见 ui_prefs_init）。
 * 其余三项（延时摄影、录像存储位置、用户体验计划）后端还没有，只存值、不生效。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 录像存储位置。只做界面：选了也不改录像落点（录像仍写内置存储）。 */
typedef enum {
    UI_RECORD_STORAGE_NONE = 0,
    UI_RECORD_STORAGE_USB,
    UI_RECORD_STORAGE_SD,
} ui_record_storage_t;

/**
 * 读盘 + 起 1 s 轮询：Glavo 由断到连时补发一次 auto_record（Glavo 重启也覆盖）。
 * 须在 DebugMsgProxy::init() 之后调用一次（见 ui_app.cpp）。重复调用无副作用。
 */
void ui_prefs_init(void);

/** 打印完成后自动吹粉，默认开。关掉后结果页不再自动吹，底部吹粉钮照常可手动吹。 */
bool ui_prefs_auto_clean(void);
void ui_prefs_set_auto_clean(bool on);

/** 自动延时摄影，默认开。只存值：换层拍照目前是编译期 GLAVO_ENABLE_LAYER_SNAP。 */
bool ui_prefs_auto_timelapse(void);
void ui_prefs_set_auto_timelapse(bool on);

/** 自动录像（= 调试页 Print H264 Rec），默认开。设值即写盘并立刻下发 Glavo。 */
bool ui_prefs_auto_record(void);
void ui_prefs_set_auto_record(bool on);

/** 录像存储位置，默认 NONE。只存值。 */
ui_record_storage_t ui_prefs_record_storage(void);
void ui_prefs_set_record_storage(ui_record_storage_t storage);

/** 加入用户体验计划，默认开。只存值。 */
bool ui_prefs_ux_program(void);
void ui_prefs_set_ux_program(bool on);

#ifdef __cplusplus
}
#endif

#endif
