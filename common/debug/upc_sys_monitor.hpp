#ifndef UPC_SYS_MONITOR_HPP
#define UPC_SYS_MONITOR_HPP

/** 主循环每帧调用；内部按 60s 间隔采样并输出 CPU/内存/温度（受 UPC_DEBUG + UPC_MOD_SYS_MONITOR 控制）。 */
void upc_sys_monitor_tick();

#endif /* UPC_SYS_MONITOR_HPP */
