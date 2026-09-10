#ifndef UI_PERF_PROBE_HPP
#define UI_PERF_PROBE_HPP

#include "lvgl_wrap.hpp"

#include <cstdint>

namespace ui {

/**
 * @brief UI 主线程性能探针：主循环分段 + LVGL 刷新分段（纯诊断，不改行为）。
 *
 * 要解决的问题：`decode 5s:` 里 `show_n` 只有约 40/5s（≈8Hz），而解码线程每
 * 5 秒发布 150 帧——三分之二的帧转换完就被下一帧覆盖，从没上屏。相机采集、
 * SHM 传递、RGA 转换都已确认稳在 30fps，损失全落在最后一步「上屏」。
 *
 * 为什么不能只看 `lv_timer` 的周期设置：LVGL 一次 `lv_timer_handler()` 里每个
 * 到期定时器**最多只跑一次**，不管已经过了几个周期。所以 `ui_timer_cb` 的实际
 * 触发周期等于主循环周期，`lv_timer_create(..., 16ms)` 写的 16 只是下限。要定位
 * 8Hz，必须先量出主循环那一圈的时间花在哪。
 *
 * 两行输出都走 module=CameraPreview，落在 `/var/log/topic/topic_ui_preview.log`，
 * 和 `decode 5s:` / `stream 5s:` 同一个文件，便于按时间窗对齐三方数据。
 */

/** 单调微秒时钟。与 camera_preview 的 monotonic_us() 同为 steady_clock，可直接相减。 */
uint64_t perf_now_us();

/**
 * @brief 记录主循环一圈的四段耗时，每圈调一次；满 5s 输出一行 `uiloop 5s:`。
 *
 * 四段对应 main.cpp 循环体的四个调用，加起来就是一圈的时间，因此
 * `loops` 直接给出主循环频率（也就是 `ui_timer_cb` 的真实触发频率）。
 *
 * @param ipc_wait_us  port.process(8)：带 8ms 超时的 IPC 收发，一圈的下限
 * @param lv_timer_us  lv_timer_handler()：LVGL 所有到期定时器 + 屏幕刷新
 * @param ipc_flush_us port.process(0)：非阻塞补发一次
 * @param monitor_us   upc_sys_monitor_tick()：系统监控采样
 */
void perf_loop_sample(uint64_t ipc_wait_us, uint64_t lv_timer_us,
                      uint64_t ipc_flush_us, uint64_t monitor_us);

/**
 * @brief 给显示器挂上刷新分段探针，`platform_init()` 成功后调一次。
 *
 * 挂在 main.cpp 而不是 platform.cpp，是为了 1.0 / 2.0 两个平台变体共用一份，
 * 不必在两个 platform.cpp 里各写一遍。
 */
void perf_attach_display(lv_display_t* disp);

} // namespace ui

#endif /* UI_PERF_PROBE_HPP */
