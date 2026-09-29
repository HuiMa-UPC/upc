#ifndef UI_AIR_QUALITY_H
#define UI_AIR_QUALITY_H

/**
 * @file ui_air_quality.hpp
 * @brief PM2.5 空气质量卡的取值口径与舱内/舱外切换状态：首页左卡与监控页 Status 卡共用。
 *
 * 两张卡画法不同（首页蓝底白色刻度、监控页白底彩色刻度），但标题、分级、游标位置、
 * 读数文本是同一套，都从 `ui_air_view_get()` 取，页面只管画。
 *
 * 切换状态只在这里存一份，所以两页联动：一边切了，另一边下次刷新（进页 / 环境推送）
 * 就是同一档。不落盘，重启回到舱内。
 *
 * 取值口径（都读 `ui_device_state` 的快照）：
 * - 舱内 Cabin 读 `pm25_cabin`：舱内 PMSA 实时值，与调试页 PM Cabin、开门前 PM2.5 检测、
 *   云端 chamber.pm25 是同一个数。电磁阀平时是关的，这个读数照样一直在刷。
 * - 舱外 Home 读 `pm25_indoor`：Glavo 填的是 SPI 小屏同一个显示值（舱外 PMSA 暂不可靠，
 *   现为 3~5 µg/m³ 缓变模拟，见 GLAVO.md），云端 chamber_ext.pm25 也是它。
 *
 * 读数按小屏的格式出（×10 加 0.5 截断、一位小数），两边逐位一致。
 */

#ifdef __cplusplus
extern "C" {
#endif

/** 一张 PM2.5 卡此刻该画的内容。 */
typedef struct {
    const char *title;          /**< "Cabin PM2.5" / "Home PM2.5" */
    bool        valid;          /**< false=无数据：分级显示 "--"、读数留空、游标隐藏 */
    const char *level_name;     /**< Good / Fair / Poor / Very Poor；无数据时 "--" */
    float       frac;           /**< 游标在四段色条上的全局比例 0..1（档内线性插值） */
    char        value_text[24]; /**< 如 "3.7 ug/m3"；无数据时空串 */
} ui_air_view_t;

/** 舱内 ⇄ 舱外循环切换。两页的切换钮都调这一个。 */
void ui_air_source_toggle(void);

/** 按当前档与最新环境快照填 `out`。 */
void ui_air_view_get(ui_air_view_t *out);

#ifdef __cplusplus
}
#endif

#endif
