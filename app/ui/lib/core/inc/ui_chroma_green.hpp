#ifndef UI_CHROMA_GREEN_HPP
#define UI_CHROMA_GREEN_HPP

/**
 * @file ui_chroma_green.hpp
 * @brief 调试用全屏绿幕：纯绿盖在 `lv_layer_sys()`，中心与四角带黑色十字，便于后期抠图。
 *
 * 不进业务布局、不经 IPC；开/关仅本次运行。打开后整屏可点一次关闭。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 状态变化回调（含点击绿幕关闭）；enabled 为新状态。 */
typedef void (*ui_chroma_green_changed_cb_t)(bool enabled);

/** 打开或关闭全屏绿幕。重复调用幂等。 */
void ui_chroma_green_set_enabled(bool enabled);

bool ui_chroma_green_enabled(void);

/** 注册状态回调（调试页用来同步开关勾选态）；传 nullptr 取消。 */
void ui_chroma_green_set_changed_cb(ui_chroma_green_changed_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_CHROMA_GREEN_HPP */
