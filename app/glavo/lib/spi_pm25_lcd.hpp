#ifndef SPI_PM25_LCD_HPP
#define SPI_PM25_LCD_HPP

/**
 * @file spi_pm25_lcd.hpp
 * @brief ST7735 SPI 小屏（fbdev `st7735rdrmfb`）常显舱外 PM2.5。
 *
 * 内核 DRM tiny 已完成初始化/offset/背光；本模块只写 shadow + pwrite。
 * UI 布局移植自 doc/spilcd/ui_pm25.*（160×80 逻辑横屏，用户态顺时针 90°→80×160 fb）。
 * 无效值（传感器未就绪 / <0）显示 "--"。节点缺失只 Warn 一次。
 */

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace upc {
namespace glavo {

class SpiPm25Lcd {
public:
    static SpiPm25Lcd& instance();

    /** 打开 fb、画静态布局；失败则后续 update 静默。 */
    void start();

    /**
     * @brief 刷新舱外 PM2.5 数值。
     * @param pm25_ug_m3 浓度 ug/m³；&lt;0 或 NaN 显示 "--"
     */
    void update(float pm25_ug_m3);

    /**
     * @brief 调试页测试刷屏：开则忽略 PM，约随 poll 周期整屏轮换纯色并 pwrite。
     *        关则恢复 PM2.5 布局。仅本次运行。
     */
    void set_test_pattern(bool on);

    /** 调试态回读。 */
    [[nodiscard]] bool test_pattern_on() const;

private:
    SpiPm25Lcd() = default;
    ~SpiPm25Lcd();
    SpiPm25Lcd(const SpiPm25Lcd&) = delete;
    SpiPm25Lcd& operator=(const SpiPm25Lcd&) = delete;

    bool ensure_open_locked();
    void close_locked();
    void flush_locked();
    void draw_static_locked();
    void set_value_locked(float pm25_ug_m3);
    void draw_value_line_locked(const char* line2, bool with_super3);
    void flush_test_locked();

    mutable std::mutex mtx_;
    int fb_fd_ = -1;
    uint16_t* shadow_ = nullptr;
    size_t shadow_bytes_ = 0;
    uint32_t stride_px_ = 0; /**< fix.line_length / 2 */
    uint32_t fb_w_ = 0;      /**< 物理宽度，期望 80 */
    uint32_t fb_h_ = 0;      /**< 物理高度，期望 160 */
    bool started_ = false;
    bool open_warn_done_ = false;
    bool ui_ready_ = false;
    bool test_pattern_ = false;
    uint8_t test_idx_ = 0;

    /* 数值行布局缓存（擦旧区用） */
    uint16_t value_y_ = 0;
    uint16_t value_area_x_ = 0;
    uint16_t value_area_w_ = 0;
    uint16_t ch_h_ = 0;
    float last_pm_ = -2.f; /**< 哨兵，强制首帧刷新 */
};

} // namespace glavo
} // namespace upc

#endif
