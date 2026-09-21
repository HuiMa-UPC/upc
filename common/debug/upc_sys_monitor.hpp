#ifndef UPC_SYS_MONITOR_HPP
#define UPC_SYS_MONITOR_HPP

/**
 * @file upc_sys_monitor.hpp
 * @brief Host 资源采样：CPU 温度 / 整机与本进程占用 / 内存。
 *
 * - `upc_sys_monitor_tick()`：主循环每帧调用；内部按 60s 间隔落盘（受
 *   UPC_DEBUG + UPC_MOD_SYS_MONITOR 控制；关掉时为空函数）。
 * - `upc_sys_monitor_sample()`：始终可用，给 UI 调试蒙版等读数用；CPU% 是
 *   「两次调用之间的均值」，首次调用 cpu_*_pct 为 -1。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UpcSysMonitorSample {
    float         cpu_temp_c;   /**< SoC 温度 °C；读失败为 -1 */
    float         cpu_sys_pct;  /**< 整机 CPU%（0..100）；尚未形成差值时为 -1 */
    float         cpu_proc_pct; /**< 本进程 CPU%（相对整机）；同上 */
    unsigned long mem_total_kb;
    unsigned long mem_avail_kb;
    unsigned long rss_kb;       /**< 本进程 VmRSS */
} UpcSysMonitorSample;

/** 主循环每帧调用；内部按 60s 间隔采样并输出（受 UPC_DEBUG + UPC_MOD_SYS_MONITOR）。 */
void upc_sys_monitor_tick(void);

/**
 * 立刻读一轮 Host 资源写入 out。
 * @return false=out 空或 CPU jiffies 读失败；温度/内存单项失败时字段为哨兵值，仍返回 true。
 */
bool upc_sys_monitor_sample(UpcSysMonitorSample* out);

#ifdef __cplusplus
}
#endif

#endif /* UPC_SYS_MONITOR_HPP */
