#ifndef UI_PM25_H
#define UI_PM25_H

#include <stdint.h>

/**
 * PM2.5 界面模块
 *
 * 画面结构：
 *   上：标题 “PM2.5”
 *   中：浓度值，如 “0.6ug/m³”（“3” 为半高上标）
 *   下：绿色圆角条
 *
 * 使用顺序：
 *   1) LCD_Init();
 *   2) UI_Pm25_Init();
 *   3) 传感器采样后调用 UI_Pm25_SetUgX10() 或 UI_Pm25_SetUgM3()
 */

/** 绘制静态布局，并显示默认占位值 0.6。上电调用一次。 */
void UI_Pm25_Init(void);

/**
 * 【推荐】传感器数据入口（整数，无浮点）。
 * @param ug_x10  浓度，单位 0.1 ug/m³
 *                例：6 → 0.6，123 → 12.3，上限 999.9
 * @note 请在主循环调用，勿在中断里刷屏。
 */
void UI_Pm25_SetUgX10(uint16_t ug_x10);

/**
 * 传感器数据入口（浮点）。
 * @param ug_m3  浓度，单位 ug/m³，显示四舍五入到 1 位小数
 *               例：0.6f → “0.6ug/m³”
 */
void UI_Pm25_SetUgM3(float ug_m3);

#endif /* UI_PM25_H */
