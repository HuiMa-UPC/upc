#ifndef LCD_ST7735_H
#define LCD_ST7735_H

#include <stdint.h>

/* ========== 显示参数（改动会影响画面位置/范围） ========== */
#define LCD_W                   160U    /* 逻辑宽度（横屏） */
#define LCD_H                   80U     /* 逻辑高度（横屏） */
#define LCD_X_OFFSET            0U      /* 显存列偏移，模组相关 */
#define LCD_Y_OFFSET            24U     /* 显存行偏移，模组相关 */

/* ========== RGB565 颜色 ========== */
#define LCD_BLACK               0x0000U
#define LCD_WHITE               0xFFFFU
#define LCD_GREEN               0x07E0U

/* ========== 驱动接口 ========== */

/** 初始化 GPIO/SPI0/ST7735，并清黑屏。上电后先调用。 */
void LCD_Init(void);

/** 全屏填充为指定颜色。 */
void LCD_Clear(uint16_t color);

/**
 * 画实心矩形。
 * @param x,y  左上角逻辑坐标
 * @param w,h  宽、高（像素）
 * @param color RGB565 颜色
 */
void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * 紧排显示字符串（Rany 24x28 字模）。
 * @param fc   前景色
 * @param bc   背景色（空像素不画，bc 只用于抗锯齿混合）
 * @param scale 像素放大倍数，当前 UI 用 1
 */
void LCD_ShowStringScaled(uint16_t x, uint16_t y, const char *str,
                          uint16_t fc, uint16_t bc, uint8_t scale);

/** 测算紧排字符串占用的像素宽度。 */
uint16_t LCD_MeasureStringScaled(const char *str, uint8_t scale);

/**
 * 半高半宽显示字符串，用于单位上标（如 m³ 的 “3”）。
 */
void LCD_ShowStringHalf(uint16_t x, uint16_t y, const char *str,
                        uint16_t fc, uint16_t bc);

/** 测算半高字符串占用的像素宽度。 */
uint16_t LCD_MeasureStringHalf(const char *str);

/**
 * 画两端带抗锯齿的圆角条（胶囊形）。
 * 当前 UI 绿条高度固定为 4。
 */
void LCD_FillRoundBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

#endif /* LCD_ST7735_H */
