#include "ui_air_quality.hpp"

#include "ui_device_state.hpp"

#include <cmath>
#include <cstdio>

namespace {

enum AirSource : int {
    kSourceCabin = 0,   /**< 舱内（默认） */
    kSourceHome,        /**< 舱外，与 SPI 小屏同值 */
    kSourceCount,
};

/** 空气质量分级：三档上界（Good/Fair/Poor 上限）+ Very Poor 插值伪上限。 */
struct AirBands {
    float b_good;     /**< ≤ b_good → Good */
    float b_fair;     /**< ≤ b_fair → Fair */
    float b_poor;     /**< ≤ b_poor → Poor；否则 Very Poor */
    float pseudo_max; /**< Very Poor 段游标插值用的伪上限 */
};

/* 阈值取自设计稿分级表：PM2.5 35/75/150（μg/m³）。舱内、舱外是同一个指标，共用这一张；
 * 改版前两张卡切的是 PM2.5 / PM10，PM10 那张 50/150/350 随之退场。 */
constexpr AirBands kPm25Bands = { 35.0f, 75.0f, 150.0f, 300.0f };

const char *const kSourceTitle[kSourceCount] = { "Cabin PM2.5", "Home PM2.5" };
const char *const kLevelName[4] = { "Good", "Fair", "Poor", "Very Poor" };

/** 读数上限（×10）：与 SPI 小屏的 kUgX10Max 一致，999.9 封顶。 */
constexpr unsigned kUgX10Max = 9999U;

/** 当前档；两页共用，重启复位到舱内。 */
int g_source = kSourceCabin;

/** 数值无效（后端未采集）：负值或 NaN → 卡片显示 "--"。 */
bool value_invalid(float v) { return std::isnan(v) || v < 0.0f; }

/** 由数值定档并给出色条全局比例（0..1）：档内按数值在区间内线性插值。 */
int classify(const AirBands &m, float v, float *frac_global)
{
    int   level;
    float lo, hi;
    if(v <= m.b_good)      { level = 0; lo = 0;        hi = m.b_good; }
    else if(v <= m.b_fair) { level = 1; lo = m.b_good; hi = m.b_fair; }
    else if(v <= m.b_poor) { level = 2; lo = m.b_fair; hi = m.b_poor; }
    else                   { level = 3; lo = m.b_poor; hi = m.pseudo_max; }
    float fin = (hi > lo) ? (v - lo) / (hi - lo) : 0.5f;
    if(fin < 0.0f) fin = 0.0f;
    if(fin > 1.0f) fin = 1.0f;
    if(frac_global) *frac_global = (static_cast<float>(level) + fin) / 4.0f;
    return level;
}

} /* namespace */

void ui_air_source_toggle(void)
{
    g_source = (g_source + 1) % kSourceCount;
}

void ui_air_view_get(ui_air_view_t *out)
{
    if(!out) {
        return;
    }
    const ui_device_snapshot_t &snap = *ui_device_state_get();
    const float v = (g_source == kSourceCabin) ? snap.pm25_cabin : snap.pm25_indoor;

    out->title = kSourceTitle[g_source];
    out->frac  = 0.0f;
    if(value_invalid(v)) {
        out->valid         = false;
        out->level_name    = "--";
        out->value_text[0] = '\0';
        return;
    }
    out->valid      = true;
    out->level_name = kLevelName[classify(kPm25Bands, v, &out->frac)];

    /* 舱外这一档必须与 SPI 小屏逐位一致，所以进位与拆位照抄小屏（spi_pm25_lcd.cpp 的
     * set_value_locked / format_ug_x10）：×10 加 0.5 截断成整数，再拆成「整数.一位」。
     * 不走 %.1f——浮点格式化的舍入方式与小屏不同，恰逢 .x5 时两边会差 0.1。
     *
     * 稿是 `35 μg/m³`，没有括号。**单位只能写成 ug/m3**：LVGL 的 Montserrat 字符集是
     * `0x20-0x7F,0xB0,0x2022`（ASCII + 度号 + 圆点，`23.6°` 能显示就是靠那个 0xB0），
     * `μ`(U+03BC) 与 `³`(U+00B3) 都不在里面。要上稿的写法得扩字符集重新生成整套字体。 */
    const float scaled = v * 10.0f + 0.5f;
    const unsigned x10 = (scaled > static_cast<float>(kUgX10Max))
                             ? kUgX10Max
                             : static_cast<unsigned>(scaled);
    std::snprintf(out->value_text, sizeof(out->value_text), "%u.%u ug/m3",
                  x10 / 10U, x10 % 10U);
}
