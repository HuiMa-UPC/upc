/**
 * @file    ui_pm25.c
 * @brief   PM2.5 界面布局与传感器数值刷新实现。
 */
#include "ui_pm25.h"
#include "lcd_st7735.h"
#include "font_rany.h"

/* ========== 布局参数（改动会影响画面排版） ========== */
#define UI_PM25_SCALE           1U      /* 正文字号放大倍数 */
#define UI_PM25_GAP_TITLE       2U      /* 标题与数值行间距 */
#define UI_PM25_GAP_BAR         2U      /* 数值行与绿条间距 */
#define UI_PM25_BAR_H           4U      /* 绿条高度 */
#define UI_PM25_UG_X10_MAX      9999U   /* 显示上限 999.9 ug/m³ */

/* 运行时布局缓存：仅保存刷新数值行需要的信息 */
typedef struct {
    uint16_t value_y;       /* 数值行 Y */
    uint16_t value_area_x;  /* 上一帧数值区 X */
    uint16_t value_area_w;  /* 上一帧数值区宽度 */
    uint16_t ch_h;          /* 正文字高 */
} ui_pm25_layout_t;

static ui_pm25_layout_t s_layout;
static uint8_t s_initialized;   /* 0=未 Init，Set 接口直接返回 */

/** 将 ug_x10 格式化为 “x.xug/m” 字符串（上标 3 单独绘制）。 */
static void UI_Pm25_FormatUgX10(char *buf, uint16_t ug_x10)
{
    uint16_t ip;
    uint16_t fp;
    char *p = buf;

    if(ug_x10 > UI_PM25_UG_X10_MAX) {
        ug_x10 = UI_PM25_UG_X10_MAX;
    }

    ip = (uint16_t)(ug_x10 / 10U);
    fp = (uint16_t)(ug_x10 % 10U);

    if(ip >= 100U) {
        *p++ = (char)('0' + (ip / 100U));
        ip = (uint16_t)(ip % 100U);
        *p++ = (char)('0' + (ip / 10U));
        *p++ = (char)('0' + (ip % 10U));
    } else if(ip >= 10U) {
        *p++ = (char)('0' + (ip / 10U));
        *p++ = (char)('0' + (ip % 10U));
    } else {
        *p++ = (char)('0' + ip);
    }

    *p++ = '.';
    *p++ = (char)('0' + fp);
    *p++ = 'u';
    *p++ = 'g';
    *p++ = '/';
    *p++ = 'm';
    *p = '\0';
}

/** 刷新第二行浓度：擦除旧区后居中绘制正文与上标 “3”。 */
static void UI_Pm25_DrawValueLine(const char *line2)
{
    uint16_t body_w;
    uint16_t super_w;
    uint16_t line2_w;
    uint16_t x_line2;
    uint16_t super_x;
    uint16_t super_y;
    uint16_t clear_x;
    uint16_t clear_right;
    uint16_t new_right;

    body_w = LCD_MeasureStringScaled(line2, UI_PM25_SCALE);
    super_w = LCD_MeasureStringHalf("3");
    line2_w = (uint16_t)(body_w + 1U + super_w);
    if(line2_w > LCD_W) {
        line2_w = LCD_W;
    }
    x_line2 = (uint16_t)((LCD_W - line2_w) / 2U);

    /* 擦除新旧区域的并集，位数增加或减少都不会残留。 */
    clear_x = (x_line2 < s_layout.value_area_x) ? x_line2 : s_layout.value_area_x;
    clear_right = (uint16_t)(s_layout.value_area_x + s_layout.value_area_w);
    new_right = (uint16_t)(x_line2 + line2_w);
    if(new_right > clear_right) {
        clear_right = new_right;
    }
    LCD_FillRect(clear_x, s_layout.value_y,
                 (uint16_t)(clear_right - clear_x), s_layout.ch_h, LCD_BLACK);

    LCD_ShowStringScaled(x_line2, s_layout.value_y, line2,
                         LCD_WHITE, LCD_BLACK, UI_PM25_SCALE);
    super_x = (uint16_t)(x_line2 + body_w + 1U);
    super_y = (uint16_t)(s_layout.value_y + 1U);
    LCD_ShowStringHalf(super_x, super_y, "3", LCD_WHITE, LCD_BLACK);

    s_layout.value_area_x = x_line2;
    s_layout.value_area_w = line2_w;
}

/** 绘制标题、绿条，并显示默认 0.6。 */
void UI_Pm25_Init(void)
{
    const uint8_t scale = UI_PM25_SCALE;
    uint16_t title_w;
    uint16_t bar_w;
    uint16_t block_h;
    uint16_t title_y;
    uint16_t bar_y;
    uint16_t bar_x;
    uint16_t x_title;

    s_layout.ch_h = (uint16_t)(FONT_RANY_H * scale);
    title_w = LCD_MeasureStringScaled("PM2.5", scale);

    bar_w = title_w;
    if(bar_w < 40U) {
        bar_w = 40U;
    }

    block_h = (uint16_t)(s_layout.ch_h + UI_PM25_GAP_TITLE
                         + s_layout.ch_h + UI_PM25_GAP_BAR + UI_PM25_BAR_H);
    title_y = (uint16_t)((LCD_H - block_h) / 2U);
    s_layout.value_y = (uint16_t)(title_y + s_layout.ch_h + UI_PM25_GAP_TITLE);
    bar_y = (uint16_t)(s_layout.value_y + s_layout.ch_h + UI_PM25_GAP_BAR);
    bar_x = (uint16_t)((LCD_W - bar_w) / 2U);
    x_title = (uint16_t)((LCD_W - title_w) / 2U);

    /* 数值区先按整屏宽预留，首次绘制后再收窄 */
    s_layout.value_area_x = 0U;
    s_layout.value_area_w = LCD_W;

    LCD_ShowStringScaled(x_title, title_y, "PM2.5",
                         LCD_WHITE, LCD_BLACK, scale);
    LCD_FillRoundBar(bar_x, bar_y, bar_w, UI_PM25_BAR_H, LCD_GREEN);

    s_initialized = 1U;
    UI_Pm25_SetUgX10(6U);   /* 默认占位 0.6ug/m³，后续由传感器覆盖 */
}

/** 整数入口：单位 0.1 ug/m³。 */
void UI_Pm25_SetUgX10(uint16_t ug_x10)
{
    char line2[12];

    if(s_initialized == 0U) {
        return;
    }

    UI_Pm25_FormatUgX10(line2, ug_x10);
    UI_Pm25_DrawValueLine(line2);
}

/** 浮点入口：单位 ug/m³，内部转成 ug_x10。 */
void UI_Pm25_SetUgM3(float ug_m3)
{
    float scaled;
    uint16_t ug_x10;

    /* 同时处理负数和 NaN；NaN 与自身比较也为 false。 */
    if(!(ug_m3 >= 0.0f)) {
        ug_m3 = 0.0f;
    }

    scaled = (ug_m3 * 10.0f) + 0.5f;   /* 四舍五入到 0.1 */
    if(scaled > (float)UI_PM25_UG_X10_MAX) {
        ug_x10 = UI_PM25_UG_X10_MAX;
    } else {
        ug_x10 = (uint16_t)scaled;
    }

    UI_Pm25_SetUgX10(ug_x10);
}
