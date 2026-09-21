/** @file lcd_st7735.c
 *  @brief ST7735 底层驱动、基础图形和 Rany 字模渲染。
 */
#include "lcd_st7735.h"
#include "font_rany.h"
#include "systick.h"
#include "gd32e23x.h"

/* ========== 硬件引脚（板端 J9）==========
 * PA5=SCK，PA7=MOSI，PB0=DC，PA6=RESET
 * CS 接地常低，背光接 3V3，不由 MCU 控制
 */
#define LCD_SPI                 SPI0            /* 使用 SPI0 */
#define LCD_SCL_PORT            GPIOA           /* SCK 端口 */
#define LCD_SCL_PIN             GPIO_PIN_5      /* SCK 引脚 */
#define LCD_MOSI_PORT           GPIOA           /* MOSI 端口 */
#define LCD_MOSI_PIN            GPIO_PIN_7      /* MOSI 引脚 */
#define LCD_RS_PORT             GPIOB           /* DC/RS 端口 */
#define LCD_RS_PIN              GPIO_PIN_0      /* DC/RS 引脚：低=命令，高=数据 */
#define LCD_RESET_PORT          GPIOA           /* RESET 端口 */
#define LCD_RESET_PIN           GPIO_PIN_6      /* RESET 引脚，低有效 */

#define LCD_RS_SET()            gpio_bit_set(LCD_RS_PORT, LCD_RS_PIN)       /* DC=1 数据 */
#define LCD_RS_CLR()            gpio_bit_reset(LCD_RS_PORT, LCD_RS_PIN)     /* DC=0 命令 */
#define LCD_RESET_SET()         gpio_bit_set(LCD_RESET_PORT, LCD_RESET_PIN) /* 释放复位 */
#define LCD_RESET_CLR()         gpio_bit_reset(LCD_RESET_PORT, LCD_RESET_PIN) /* 拉低复位 */

/* 微秒忙等。板上 DC/RST 有 100 nF，切换 DC 后需等待稳定。 */
static void LCD_DelayUs(uint32_t us)
{
    volatile uint32_t n = us * 10U;

    while(n--) {
    }
}

/* 读某一平面的 1bit：0=solid，1=mid，2=soft */
static uint8_t LCD_RanyPlaneBit(const uint8_t *glyph, uint8_t plane,
                                uint8_t col, uint8_t row)
{
    uint32_t bits;
    uint16_t base;
    uint8_t i;

    base = (uint16_t)plane * FONT_RANY_PLANE_BYTES
         + (uint16_t)col * FONT_RANY_BPC;
    bits = 0U;
    for(i = 0U; i < FONT_RANY_BPC; i++) {
        bits |= ((uint32_t)glyph[base + i] << (8U * i));
    }
    return (uint8_t)((bits >> row) & 0x01U);
}

/* 0=空，1=软边，2=中灰边，3=实心 */
static uint8_t LCD_RanyLevel(const uint8_t *glyph, uint8_t col, uint8_t row)
{
    if((glyph == 0) || (col >= FONT_RANY_W) || (row >= FONT_RANY_H)) {
        return 0U;
    }
    if(LCD_RanyPlaneBit(glyph, 0U, col, row) != 0U) {
        return 3U;
    }
    if(LCD_RanyPlaneBit(glyph, 1U, col, row) != 0U) {
        return 2U;
    }
    if(LCD_RanyPlaneBit(glyph, 2U, col, row) != 0U) {
        return 1U;
    }
    return 0U;
}

/* RGB565 前景/背景混合，w_fc 为前景权重 0..16。 */
static uint16_t LCD_Mix565(uint16_t fc, uint16_t bc, uint8_t w_fc)
{
    uint16_t fr = (uint16_t)((fc >> 11) & 0x1FU);
    uint16_t fg = (uint16_t)((fc >> 5) & 0x3FU);
    uint16_t fb = (uint16_t)(fc & 0x1FU);
    uint16_t br = (uint16_t)((bc >> 11) & 0x1FU);
    uint16_t bg = (uint16_t)((bc >> 5) & 0x3FU);
    uint16_t bb = (uint16_t)(bc & 0x1FU);
    uint8_t w_bc = (uint8_t)(16U - w_fc);
    uint16_t r = (uint16_t)((fr * w_fc + br * w_bc) / 16U);
    uint16_t g = (uint16_t)((fg * w_fc + bg * w_bc) / 16U);
    uint16_t b = (uint16_t)((fb * w_fc + bb * w_bc) / 16U);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 将灰度等级映射为颜色：3=实心，2≈70%，1≈30%，0=背景。 */
static uint16_t LCD_LevelColor(uint8_t lv, uint16_t fc, uint16_t bc)
{
    if(lv >= 3U) {
        return fc;
    }
    if(lv == 2U) {
        return LCD_Mix565(fc, bc, 11U); /* ~70% */
    }
    if(lv == 1U) {
        return LCD_Mix565(fc, bc, 5U);  /* ~30% */
    }
    return bc;
}

/* 返回字形有像素的列范围 [c0,c1]，空字返回整格 */
static void LCD_RanyGlyphCols(const uint8_t *glyph, uint8_t *c0, uint8_t *c1)
{
    uint8_t col;
    uint8_t row;
    uint8_t min_c = FONT_RANY_W;
    uint8_t max_c = 0U;
    uint8_t found = 0U;

    for(col = 0U; col < FONT_RANY_W; col++) {
        for(row = 0U; row < FONT_RANY_H; row++) {
            if(LCD_RanyLevel(glyph, col, row) != 0U) {
                if(col < min_c) {
                    min_c = col;
                }
                if(col > max_c) {
                    max_c = col;
                }
                found = 1U;
                break;
            }
        }
    }
    if(found == 0U) {
        *c0 = 0U;
        *c1 = (uint8_t)(FONT_RANY_W - 1U);
    } else {
        *c0 = min_c;
        *c1 = max_c;
    }
}

/* 通过 SPI0 发送 1 字节，并读空接收缓冲。 */
static void LCD_SPI_WriteByte(uint8_t data)
{
    while(RESET == spi_i2s_flag_get(LCD_SPI, SPI_FLAG_TBE)) {
    }
    spi_i2s_data_transmit(LCD_SPI, data);
    while(RESET == spi_i2s_flag_get(LCD_SPI, SPI_FLAG_RBNE)) {
    }
    (void)spi_i2s_data_receive(LCD_SPI);
}

/* 写命令。 */
static void LCD_WriteCmd(uint8_t cmd)
{
    LCD_RS_CLR();                   /* DC=0：命令 */
    LCD_DelayUs(20U);
    LCD_SPI_WriteByte(cmd);
}

/* 写 8 位参数。 */
static void LCD_WriteData8(uint8_t data)
{
    LCD_RS_SET();                   /* DC=1：数据 */
    LCD_DelayUs(20U);
    LCD_SPI_WriteByte(data);
}

/* 写一个 RGB565 像素；连续写像素时 DC 保持高电平。 */
static void LCD_WriteData16(uint16_t data)
{
    LCD_SPI_WriteByte((uint8_t)(data >> 8));
    LCD_SPI_WriteByte((uint8_t)(data & 0xFFU));
}

/* 进入连续像素数据写入状态。 */
static void LCD_BeginWriteData(void)
{
    LCD_RS_SET();
    LCD_DelayUs(20U);
}

/* 配置 GPIO 和硬件 SPI0，CS/背光不由 MCU 控制。 */
static void LCD_GPIO_SPI_Init(void)
{
    spi_parameter_struct spi_init_struct;

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_SPI0);

    /* PA5=SPI0_SCK，PA7=SPI0_MOSI，复用 AF0。 */
    gpio_af_set(LCD_SCL_PORT, GPIO_AF_0, LCD_SCL_PIN | LCD_MOSI_PIN);
    gpio_mode_set(LCD_SCL_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, LCD_SCL_PIN | LCD_MOSI_PIN);
    gpio_output_options_set(LCD_SCL_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, LCD_SCL_PIN | LCD_MOSI_PIN);

    /* DC 和 RESET 使用 GPIO 推挽输出。 */
    gpio_mode_set(LCD_RS_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LCD_RS_PIN);
    gpio_output_options_set(LCD_RS_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, LCD_RS_PIN);

    gpio_mode_set(LCD_RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LCD_RESET_PIN);
    gpio_output_options_set(LCD_RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, LCD_RESET_PIN);

    LCD_RS_SET();
    LCD_RESET_SET();

    spi_i2s_deinit(LCD_SPI);
    spi_struct_para_init(&spi_init_struct);
    spi_init_struct.device_mode          = SPI_MASTER;
    spi_init_struct.trans_mode           = SPI_TRANSMODE_FULLDUPLEX;
    spi_init_struct.frame_size           = SPI_FRAMESIZE_8BIT;
    spi_init_struct.nss                  = SPI_NSS_SOFT;
    spi_init_struct.endian               = SPI_ENDIAN_MSB;
    spi_init_struct.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE; /* Mode 0 */
    /* 板上 SCL/SDA 有 330 pF 电容，保留当前低速 SPI 以确保稳定。 */
    spi_init_struct.prescale             = SPI_PSC_64;
    spi_init(LCD_SPI, &spi_init_struct);
    spi_enable(LCD_SPI);
    /* 软件 NSS 必须拉高，主机才能正常输出时钟。 */
    spi_nss_internal_high(LCD_SPI);
}

/* 设置物理显存窗口，随后可连续写入像素。 */
static void LCD_SetAddressRaw(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_WriteCmd(0x2AU);                /* Column Address Set */
    LCD_WriteData8((uint8_t)(x0 >> 8));
    LCD_WriteData8((uint8_t)(x0 & 0xFFU));
    LCD_WriteData8((uint8_t)(x1 >> 8));
    LCD_WriteData8((uint8_t)(x1 & 0xFFU));

    LCD_WriteCmd(0x2BU);                /* Row Address Set */
    LCD_WriteData8((uint8_t)(y0 >> 8));
    LCD_WriteData8((uint8_t)(y0 & 0xFFU));
    LCD_WriteData8((uint8_t)(y1 >> 8));
    LCD_WriteData8((uint8_t)(y1 & 0xFFU));

    LCD_WriteCmd(0x2CU);                /* Memory Write */
    LCD_BeginWriteData();
}

/* 将逻辑坐标转换为物理显存坐标。 */
static void LCD_SetAddress(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_SetAddressRaw((uint16_t)(x0 + LCD_X_OFFSET),
                      (uint16_t)(y0 + LCD_Y_OFFSET),
                      (uint16_t)(x1 + LCD_X_OFFSET),
                      (uint16_t)(y1 + LCD_Y_OFFSET));
}

/* 绘制单个像素。 */
static void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    if((x >= LCD_W) || (y >= LCD_H)) {
        return;
    }
    LCD_SetAddress(x, y, x, y);
    LCD_WriteData16(color);
}

/* 画实心矩形（逻辑坐标）。 */
void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t n;
    uint16_t x1;
    uint16_t y1;

    if((w == 0U) || (h == 0U) || (x >= LCD_W) || (y >= LCD_H)) {
        return;
    }

    x1 = (uint16_t)(x + w - 1U);
    y1 = (uint16_t)(y + h - 1U);
    if(x1 >= LCD_W) {
        x1 = (uint16_t)(LCD_W - 1U);
    }
    if(y1 >= LCD_H) {
        y1 = (uint16_t)(LCD_H - 1U);
    }

    n = (uint32_t)(x1 - x + 1U) * (uint32_t)(y1 - y + 1U);
    LCD_SetAddress(x, y, x1, y1);
    while(n--) {
        LCD_WriteData16(color);
    }
}

/* 全屏填充；比逻辑区域多刷一圈，覆盖偏移误差边缘残留。 */
void LCD_Clear(uint16_t color)
{
    uint32_t n;
    uint16_t ax0, ay0, ax1, ay1;

    ax0 = LCD_X_OFFSET;
    ay0 = LCD_Y_OFFSET;
    ax1 = (uint16_t)(LCD_W - 1U + LCD_X_OFFSET);
    ay1 = (uint16_t)(LCD_H - 1U + LCD_Y_OFFSET);

    /* 保留现有控制器窗口边界，保证显示效果不变。 */
    if(ax0 > 0U) {
        ax0--;
    }
    if(ay0 > 0U) {
        ay0--;
    }
    if(ax1 < 131U) {
        ax1++;
    }
    if(ay1 < 161U) {
        ay1++;
    }

    n = (uint32_t)(ax1 - ax0 + 1U) * (uint32_t)(ay1 - ay0 + 1U);
    LCD_SetAddressRaw(ax0, ay0, ax1, ay1);
    while(n--) {
        LCD_WriteData16(color);
    }
}

/* 紧排绘制字符串，返回实际占用宽度。 */
static uint16_t LCD_DrawStringRany(uint16_t x, uint16_t y, const char *str,
                                   uint16_t fc, uint16_t bc, uint8_t scale)
{
    uint8_t row;
    uint8_t col;
    uint8_t c0;
    uint8_t c1;
    uint8_t gw;
    const uint8_t *p;
    uint16_t px;
    uint16_t py;
    uint16_t x0 = x;

    if((scale == 0U) || (str == 0)) {
        return 0U;
    }

    while(*str != '\0') {
        p = FontRany_GetGlyph(*str);
        LCD_RanyGlyphCols(p, &c0, &c1);
        gw = (uint8_t)(c1 - c0 + 1U);

        {
            uint8_t lv;
            uint16_t color;

            for(row = 0U; row < FONT_RANY_H; row++) {
                for(col = c0; col <= c1; col++) {
                    lv = LCD_RanyLevel(p, col, row);
                    if(lv == 0U) {
                        continue;
                    }
                    color = LCD_LevelColor(lv, fc, bc);
                    px = (uint16_t)(x + (uint16_t)(col - c0) * scale);
                    py = (uint16_t)(y + (uint16_t)row * scale);
                    if(scale == 1U) {
                        LCD_DrawPoint(px, py, color);
                    } else {
                        LCD_FillRect(px, py, scale, scale, color);
                    }
                }
            }
        }

        /* 字距 1 点阵列 */
        x = (uint16_t)(x + (uint16_t)(gw + 1U) * scale);
        if(x > (LCD_W - scale)) {
            break;
        }
        str++;
    }

    if(x > x0) {
        return (uint16_t)(x - x0 - scale); /* 去掉末尾字距 */
    }
    return 0U;
}

/* 公开接口：按 scale 紧排显示字符串。 */
void LCD_ShowStringScaled(uint16_t x, uint16_t y, const char *str,
                          uint16_t fc, uint16_t bc, uint8_t scale)
{
    (void)LCD_DrawStringRany(x, y, str, fc, bc, scale);
}

/* 公开接口：测算紧排字符串宽度。 */
uint16_t LCD_MeasureStringScaled(const char *str, uint8_t scale)
{
    uint8_t c0;
    uint8_t c1;
    uint8_t gw;
    uint16_t w = 0U;
    const uint8_t *p;

    if((scale == 0U) || (str == 0) || (*str == '\0')) {
        return 0U;
    }
    while(*str != '\0') {
        p = FontRany_GetGlyph(*str);
        LCD_RanyGlyphCols(p, &c0, &c1);
        gw = (uint8_t)(c1 - c0 + 1U);
        w = (uint16_t)(w + (uint16_t)(gw + 1U) * scale);
        str++;
    }
    if(w >= scale) {
        w = (uint16_t)(w - scale);
    }
    return w;
}

/* 2x2 取最大灰度，缩成半尺寸时笔画更连贯 */
static uint8_t LCD_RanyLevel2x2(const uint8_t *glyph, uint8_t col, uint8_t row)
{
    uint8_t lv;
    uint8_t best = 0U;
    uint8_t dc;
    uint8_t dr;

    for(dr = 0U; dr < 2U; dr++) {
        for(dc = 0U; dc < 2U; dc++) {
            lv = LCD_RanyLevel(glyph, (uint8_t)(col + dc), (uint8_t)(row + dr));
            if(lv > best) {
                best = lv;
            }
        }
    }
    return best;
}

/* 公开接口：测算半高字符串宽度。 */
uint16_t LCD_MeasureStringHalf(const char *str)
{
    uint8_t c0;
    uint8_t c1;
    uint8_t gw;
    uint16_t w = 0U;
    const uint8_t *p;

    if((str == 0) || (*str == '\0')) {
        return 0U;
    }
    while(*str != '\0') {
        p = FontRany_GetGlyph(*str);
        LCD_RanyGlyphCols(p, &c0, &c1);
        gw = (uint8_t)(c1 - c0 + 1U);
        w = (uint16_t)(w + (uint16_t)((gw + 1U) / 2U) + 1U);
        str++;
    }
    if(w > 0U) {
        w = (uint16_t)(w - 1U);
    }
    return w;
}

/* 公开接口：半高半宽显示，用于上标。 */
void LCD_ShowStringHalf(uint16_t x, uint16_t y, const char *str,
                        uint16_t fc, uint16_t bc)
{
    uint8_t c0;
    uint8_t c1;
    uint8_t gw;
    uint8_t ox;
    uint8_t oy;
    uint8_t lv;
    uint16_t px;
    uint16_t py;
    const uint8_t *p;
    uint8_t half_h = (uint8_t)((FONT_RANY_H + 1U) / 2U);

    if(str == 0) {
        return;
    }

    while(*str != '\0') {
        p = FontRany_GetGlyph(*str);
        LCD_RanyGlyphCols(p, &c0, &c1);
        gw = (uint8_t)(c1 - c0 + 1U);

        for(oy = 0U; oy < half_h; oy++) {
            for(ox = 0U; ox < (uint8_t)((gw + 1U) / 2U); ox++) {
                lv = LCD_RanyLevel2x2(p,
                                     (uint8_t)(c0 + (uint8_t)(ox * 2U)),
                                     (uint8_t)(oy * 2U));
                if(lv != 0U) {
                    px = (uint16_t)(x + ox);
                    py = (uint16_t)(y + oy);
                    LCD_DrawPoint(px, py, LCD_LevelColor(lv, fc, bc));
                }
            }
        }

        x = (uint16_t)(x + (uint16_t)((gw + 1U) / 2U) + 1U);
        if(x >= LCD_W) {
            break;
        }
        str++;
    }
}

/* 公开接口：画两端抗锯齿圆角条；当前 UI 使用 h=4。 */
void LCD_FillRoundBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint16_t row;
    uint16_t yy;
    uint16_t solid_inset;
    uint16_t x_start;
    uint16_t x_len;
    uint16_t aa_lo; /* 端头外沿灰 */
    uint16_t aa_hi; /* 端头次外沿灰 */

    if((w == 0U) || (h == 0U)) {
        return;
    }
    if((h < 3U) || (w < 6U)) {
        LCD_FillRect(x, y, w, h, color);
        return;
    }

    /* 仅柔化两端台阶：实心内收，外侧铺两级半透明绿 */
    aa_hi = LCD_Mix565(color, LCD_BLACK, 10U); /* ~60% */
    aa_lo = LCD_Mix565(color, LCD_BLACK, 5U);  /* ~30% */

    for(row = 0U; row < h; row++) {
        yy = (uint16_t)(y + row);

        if(h == 4U) {
            /* 4px 高专用端头（当前 UI）：
             *   .a######a.
             *   a########a
             *   a########a
             *   .a######a.
             */
            if((row == 0U) || (row == 3U)) {
                solid_inset = 2U;
                LCD_DrawPoint((uint16_t)(x + 1U), yy, aa_hi);
                LCD_DrawPoint((uint16_t)(x + w - 2U), yy, aa_hi);
            } else {
                solid_inset = 1U;
                LCD_DrawPoint(x, yy, aa_lo);
                LCD_DrawPoint((uint16_t)(x + w - 1U), yy, aa_lo);
            }
        } else if(h == 5U) {
            if((row == 0U) || (row == 4U)) {
                solid_inset = 2U;
                LCD_DrawPoint((uint16_t)(x + 1U), yy, aa_hi);
                LCD_DrawPoint((uint16_t)(x + w - 2U), yy, aa_hi);
            } else if((row == 1U) || (row == 3U)) {
                solid_inset = 1U;
                LCD_DrawPoint(x, yy, aa_lo);
                LCD_DrawPoint((uint16_t)(x + w - 1U), yy, aa_lo);
                LCD_DrawPoint((uint16_t)(x + 1U), yy, aa_hi);
                LCD_DrawPoint((uint16_t)(x + w - 2U), yy, aa_hi);
            } else {
                solid_inset = 0U;
                LCD_DrawPoint(x, yy, aa_hi);
                LCD_DrawPoint((uint16_t)(x + w - 1U), yy, aa_hi);
            }
        } else {
            /* 其它高度：顶/底行内收并加灰边 */
            if((row == 0U) || (row == (uint16_t)(h - 1U))) {
                solid_inset = 2U;
                LCD_DrawPoint((uint16_t)(x + 1U), yy, aa_hi);
                LCD_DrawPoint((uint16_t)(x + w - 2U), yy, aa_hi);
            } else if((row == 1U) || (row == (uint16_t)(h - 2U))) {
                solid_inset = 1U;
                LCD_DrawPoint(x, yy, aa_lo);
                LCD_DrawPoint((uint16_t)(x + w - 1U), yy, aa_lo);
            } else {
                solid_inset = 0U;
            }
        }

        if(w > (uint16_t)(2U * solid_inset)) {
            x_start = (uint16_t)(x + solid_inset);
            x_len = (uint16_t)(w - (2U * solid_inset));
            LCD_FillRect(x_start, yy, x_len, 1U, color);
        }
    }
}

/* 公开接口：初始化硬件与 ST7735，最后清黑屏。 */
void LCD_Init(void)
{
    LCD_GPIO_SPI_Init();

    /* 硬件复位；板上 RST 有 100 nF 电容，保持当前脉宽。 */
    LCD_RESET_SET();
    delay_1ms(20U);
    LCD_RESET_CLR();
    delay_1ms(50U);
    LCD_RESET_SET();
    delay_1ms(150U);

    LCD_WriteCmd(0x01U);                /* Software Reset */
    delay_1ms(150U);

    LCD_WriteCmd(0x11U);                /* Sleep Out */
    delay_1ms(150U);

    /* 帧率和电源：当前模组验证通过的 ST7735R 初始化序列。 */
    LCD_WriteCmd(0xB1U);
    LCD_WriteData8(0x01U);
    LCD_WriteData8(0x2CU);
    LCD_WriteData8(0x2DU);

    LCD_WriteCmd(0xB2U);
    LCD_WriteData8(0x01U);
    LCD_WriteData8(0x2CU);
    LCD_WriteData8(0x2DU);

    LCD_WriteCmd(0xB3U);
    LCD_WriteData8(0x01U);
    LCD_WriteData8(0x2CU);
    LCD_WriteData8(0x2DU);
    LCD_WriteData8(0x01U);
    LCD_WriteData8(0x2CU);
    LCD_WriteData8(0x2DU);

    LCD_WriteCmd(0xB4U);
    LCD_WriteData8(0x07U);

    LCD_WriteCmd(0xC0U);
    LCD_WriteData8(0xA2U);
    LCD_WriteData8(0x02U);
    LCD_WriteData8(0x84U);

    LCD_WriteCmd(0xC1U);
    LCD_WriteData8(0xC5U);

    LCD_WriteCmd(0xC2U);
    LCD_WriteData8(0x0AU);
    LCD_WriteData8(0x00U);

    LCD_WriteCmd(0xC3U);
    LCD_WriteData8(0x8AU);
    LCD_WriteData8(0x2AU);

    LCD_WriteCmd(0xC4U);
    LCD_WriteData8(0x8AU);
    LCD_WriteData8(0xEEU);

    LCD_WriteCmd(0xC5U);
    LCD_WriteData8(0x0EU);

    /* 关闭反色，保持当前 RGB565 显示效果。 */
    LCD_WriteCmd(0x20U);                /* INVOFF */

    LCD_WriteCmd(0x36U);                /* MADCTL：横屏 MV|MY|BGR */
    LCD_WriteData8(0xA8U);

    LCD_WriteCmd(0x3AU);                /* RGB565 */
    LCD_WriteData8(0x05U);

    LCD_WriteCmd(0xE0U);
    LCD_WriteData8(0x02U);
    LCD_WriteData8(0x1CU);
    LCD_WriteData8(0x07U);
    LCD_WriteData8(0x12U);
    LCD_WriteData8(0x37U);
    LCD_WriteData8(0x32U);
    LCD_WriteData8(0x29U);
    LCD_WriteData8(0x2DU);
    LCD_WriteData8(0x29U);
    LCD_WriteData8(0x25U);
    LCD_WriteData8(0x2BU);
    LCD_WriteData8(0x39U);
    LCD_WriteData8(0x00U);
    LCD_WriteData8(0x01U);
    LCD_WriteData8(0x03U);
    LCD_WriteData8(0x10U);

    LCD_WriteCmd(0xE1U);
    LCD_WriteData8(0x03U);
    LCD_WriteData8(0x1DU);
    LCD_WriteData8(0x07U);
    LCD_WriteData8(0x06U);
    LCD_WriteData8(0x2EU);
    LCD_WriteData8(0x2CU);
    LCD_WriteData8(0x29U);
    LCD_WriteData8(0x2DU);
    LCD_WriteData8(0x2EU);
    LCD_WriteData8(0x2EU);
    LCD_WriteData8(0x37U);
    LCD_WriteData8(0x3FU);
    LCD_WriteData8(0x00U);
    LCD_WriteData8(0x00U);
    LCD_WriteData8(0x02U);
    LCD_WriteData8(0x10U);

    LCD_WriteCmd(0x13U);                /* NORON */
    delay_1ms(10U);

    LCD_WriteCmd(0x29U);                /* DISPON */
    delay_1ms(100U);

    LCD_Clear(LCD_BLACK);
}
