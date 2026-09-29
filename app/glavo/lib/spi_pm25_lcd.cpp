#include "spi_pm25_lcd.hpp"
#include "spi_pm25_lcd_font.hpp"
#include "device_config.hpp"

#include "debug/rs_log.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fb.h>
#include <sys/ioctl.h>
#endif

namespace upc {
namespace glavo {
namespace {

using pm25_font::FontRany_GetGlyph;

/* 逻辑横屏（与 MCU UI 一致）；fb 为竖屏 80×160，写 shadow 时逆时针 90° */
constexpr uint16_t kUiW = 160U;
constexpr uint16_t kUiH = 80U;
constexpr uint16_t kColorBlack = 0x0000U;
constexpr uint16_t kColorWhite = 0xFFFFU;
constexpr uint16_t kColorGreen = 0x07E0U;

constexpr uint16_t kUiScale = 1U;
constexpr uint16_t kGapTitle = 2U;
constexpr uint16_t kGapBar = 2U;
constexpr uint16_t kBarH = 4U;
constexpr uint16_t kUgX10Max = 9999U;

constexpr const char* kFbName = GLAVO_SPI_PM25_LCD_FB_NAME;

struct DrawCtx {
    uint16_t* shadow = nullptr;
    size_t shadow_words = 0;
    uint32_t stride_px = 0;
    uint32_t fb_w = 0;
};

uint16_t mix565(uint16_t fc, uint16_t bc, uint8_t w_fc)
{
    const uint16_t fr = static_cast<uint16_t>((fc >> 11) & 0x1FU);
    const uint16_t fg = static_cast<uint16_t>((fc >> 5) & 0x3FU);
    const uint16_t fb = static_cast<uint16_t>(fc & 0x1FU);
    const uint16_t br = static_cast<uint16_t>((bc >> 11) & 0x1FU);
    const uint16_t bg = static_cast<uint16_t>((bc >> 5) & 0x3FU);
    const uint16_t bb = static_cast<uint16_t>(bc & 0x1FU);
    const uint8_t w_bc = static_cast<uint8_t>(16U - w_fc);
    const uint16_t r = static_cast<uint16_t>((fr * w_fc + br * w_bc) / 16U);
    const uint16_t g = static_cast<uint16_t>((fg * w_fc + bg * w_bc) / 16U);
    const uint16_t b = static_cast<uint16_t>((fb * w_fc + bb * w_bc) / 16U);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

uint16_t level_color(uint8_t lv, uint16_t fc, uint16_t bc)
{
    if(lv >= 3U) {
        return fc;
    }
    if(lv == 2U) {
        return mix565(fc, bc, 11U);
    }
    if(lv == 1U) {
        return mix565(fc, bc, 5U);
    }
    return bc;
}

uint8_t rany_plane_bit(const uint8_t* glyph, uint8_t plane, uint8_t col, uint8_t row)
{
    const uint16_t base = static_cast<uint16_t>(plane) * FONT_RANY_PLANE_BYTES +
                          static_cast<uint16_t>(col) * FONT_RANY_BPC;
    uint32_t bits = 0U;
    for(uint8_t i = 0U; i < FONT_RANY_BPC; ++i) {
        bits |= (static_cast<uint32_t>(glyph[base + i]) << (8U * i));
    }
    return static_cast<uint8_t>((bits >> row) & 0x01U);
}

uint8_t rany_level(const uint8_t* glyph, uint8_t col, uint8_t row)
{
    if((glyph == nullptr) || (col >= FONT_RANY_W) || (row >= FONT_RANY_H)) {
        return 0U;
    }
    if(rany_plane_bit(glyph, 0U, col, row) != 0U) {
        return 3U;
    }
    if(rany_plane_bit(glyph, 1U, col, row) != 0U) {
        return 2U;
    }
    if(rany_plane_bit(glyph, 2U, col, row) != 0U) {
        return 1U;
    }
    return 0U;
}

void rany_glyph_cols(const uint8_t* glyph, uint8_t* c0, uint8_t* c1)
{
    uint8_t min_c = FONT_RANY_W;
    uint8_t max_c = 0U;
    uint8_t found = 0U;
    for(uint8_t col = 0U; col < FONT_RANY_W; ++col) {
        for(uint8_t row = 0U; row < FONT_RANY_H; ++row) {
            if(rany_level(glyph, col, row) != 0U) {
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
        *c1 = static_cast<uint8_t>(FONT_RANY_W - 1U);
    } else {
        *c0 = min_c;
        *c1 = max_c;
    }
}

uint8_t rany_level_2x2(const uint8_t* glyph, uint8_t col, uint8_t row)
{
    uint8_t best = 0U;
    for(uint8_t dr = 0U; dr < 2U; ++dr) {
        for(uint8_t dc = 0U; dc < 2U; ++dc) {
            const uint8_t lv =
                rany_level(glyph, static_cast<uint8_t>(col + dc), static_cast<uint8_t>(row + dr));
            if(lv > best) {
                best = lv;
            }
        }
    }
    return best;
}

bool find_fb_by_name(const char* want_name, char* out, size_t out_sz)
{
    DIR* d = opendir("/sys/class/graphics");
    if(!d) {
        return false;
    }
    bool ok = false;
    while(const dirent* ent = readdir(d)) {
        if(std::strncmp(ent->d_name, "fb", 2) != 0) {
            continue;
        }
        char name_path[288];
        if(std::snprintf(name_path, sizeof(name_path), "/sys/class/graphics/%s/name",
                         ent->d_name) >= static_cast<int>(sizeof(name_path))) {
            continue;
        }
        FILE* fp = std::fopen(name_path, "r");
        if(!fp) {
            continue;
        }
        char name[64] = {};
        const char* got = std::fgets(name, sizeof(name), fp);
        std::fclose(fp);
        if(!got) {
            continue;
        }
        size_t n = std::strlen(name);
        while(n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) {
            name[--n] = '\0';
        }
        if(std::strcmp(name, want_name) != 0) {
            continue;
        }
        if(std::snprintf(out, out_sz, "/dev/%s", ent->d_name) >= static_cast<int>(out_sz)) {
            continue;
        }
        ok = true;
        break;
    }
    closedir(d);
    return ok;
}

/* UI→fb：逆时针 90°（相对旧顺时针 90° 再转 180°，适配当前装配朝向） */
void draw_point(DrawCtx& c, uint16_t x, uint16_t y, uint16_t color)
{
    if((x >= kUiW) || (y >= kUiH) || !c.shadow || c.fb_w == 0) {
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(kUiW - 1U - x) * c.stride_px + y;
    if(idx < c.shadow_words) {
        c.shadow[idx] = color;
    }
}

void fill_rect(DrawCtx& c, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if((w == 0U) || (h == 0U) || (x >= kUiW) || (y >= kUiH)) {
        return;
    }
    uint16_t x1 = static_cast<uint16_t>(x + w - 1U);
    uint16_t y1 = static_cast<uint16_t>(y + h - 1U);
    if(x1 >= kUiW) {
        x1 = static_cast<uint16_t>(kUiW - 1U);
    }
    if(y1 >= kUiH) {
        y1 = static_cast<uint16_t>(kUiH - 1U);
    }
    for(uint16_t yy = y; yy <= y1; ++yy) {
        for(uint16_t xx = x; xx <= x1; ++xx) {
            draw_point(c, xx, yy, color);
        }
    }
}

uint16_t measure_scaled(const char* str, uint8_t scale)
{
    if((scale == 0U) || (str == nullptr) || (*str == '\0')) {
        return 0U;
    }
    uint16_t w = 0U;
    while(*str != '\0') {
        const uint8_t* p = FontRany_GetGlyph(*str);
        uint8_t c0 = 0, c1 = 0;
        rany_glyph_cols(p, &c0, &c1);
        const uint8_t gw = static_cast<uint8_t>(c1 - c0 + 1U);
        w = static_cast<uint16_t>(w + static_cast<uint16_t>(gw + 1U) * scale);
        ++str;
    }
    if(w >= scale) {
        w = static_cast<uint16_t>(w - scale);
    }
    return w;
}

uint16_t measure_half(const char* str)
{
    if((str == nullptr) || (*str == '\0')) {
        return 0U;
    }
    uint16_t w = 0U;
    while(*str != '\0') {
        const uint8_t* p = FontRany_GetGlyph(*str);
        uint8_t c0 = 0, c1 = 0;
        rany_glyph_cols(p, &c0, &c1);
        const uint8_t gw = static_cast<uint8_t>(c1 - c0 + 1U);
        w = static_cast<uint16_t>(w + static_cast<uint16_t>((gw + 1U) / 2U) + 1U);
        ++str;
    }
    if(w > 0U) {
        w = static_cast<uint16_t>(w - 1U);
    }
    return w;
}

void show_scaled(DrawCtx& c, uint16_t x, uint16_t y, const char* str, uint16_t fc, uint16_t bc,
                 uint8_t scale)
{
    if((scale == 0U) || (str == nullptr)) {
        return;
    }
    while(*str != '\0') {
        const uint8_t* p = FontRany_GetGlyph(*str);
        uint8_t c0 = 0, c1 = 0;
        rany_glyph_cols(p, &c0, &c1);
        const uint8_t gw = static_cast<uint8_t>(c1 - c0 + 1U);
        for(uint8_t row = 0U; row < FONT_RANY_H; ++row) {
            for(uint8_t col = c0; col <= c1; ++col) {
                const uint8_t lv = rany_level(p, col, row);
                if(lv == 0U) {
                    continue;
                }
                const uint16_t color = level_color(lv, fc, bc);
                const uint16_t px = static_cast<uint16_t>(x + static_cast<uint16_t>(col - c0) * scale);
                const uint16_t py = static_cast<uint16_t>(y + static_cast<uint16_t>(row) * scale);
                if(scale == 1U) {
                    draw_point(c, px, py, color);
                } else {
                    fill_rect(c, px, py, scale, scale, color);
                }
            }
        }
        x = static_cast<uint16_t>(x + static_cast<uint16_t>(gw + 1U) * scale);
        if(x > (kUiW - scale)) {
            break;
        }
        ++str;
    }
}

void show_half(DrawCtx& c, uint16_t x, uint16_t y, const char* str, uint16_t fc, uint16_t bc)
{
    if(str == nullptr) {
        return;
    }
    const uint8_t half_h = static_cast<uint8_t>((FONT_RANY_H + 1U) / 2U);
    while(*str != '\0') {
        const uint8_t* p = FontRany_GetGlyph(*str);
        uint8_t c0 = 0, c1 = 0;
        rany_glyph_cols(p, &c0, &c1);
        const uint8_t gw = static_cast<uint8_t>(c1 - c0 + 1U);
        for(uint8_t oy = 0U; oy < half_h; ++oy) {
            for(uint8_t ox = 0U; ox < static_cast<uint8_t>((gw + 1U) / 2U); ++ox) {
                const uint8_t lv = rany_level_2x2(
                    p, static_cast<uint8_t>(c0 + static_cast<uint8_t>(ox * 2U)),
                    static_cast<uint8_t>(oy * 2U));
                if(lv != 0U) {
                    draw_point(c, static_cast<uint16_t>(x + ox), static_cast<uint16_t>(y + oy),
                               level_color(lv, fc, bc));
                }
            }
        }
        x = static_cast<uint16_t>(x + static_cast<uint16_t>((gw + 1U) / 2U) + 1U);
        if(x >= kUiW) {
            break;
        }
        ++str;
    }
}

void fill_round_bar(DrawCtx& c, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if((w == 0U) || (h == 0U)) {
        return;
    }
    if((h < 3U) || (w < 6U)) {
        fill_rect(c, x, y, w, h, color);
        return;
    }
    const uint16_t aa_hi = mix565(color, kColorBlack, 10U);
    const uint16_t aa_lo = mix565(color, kColorBlack, 5U);
    for(uint16_t row = 0U; row < h; ++row) {
        const uint16_t yy = static_cast<uint16_t>(y + row);
        uint16_t solid_inset = 0U;
        if(h == 4U) {
            if((row == 0U) || (row == 3U)) {
                solid_inset = 2U;
                draw_point(c, static_cast<uint16_t>(x + 1U), yy, aa_hi);
                draw_point(c, static_cast<uint16_t>(x + w - 2U), yy, aa_hi);
            } else {
                solid_inset = 1U;
                draw_point(c, x, yy, aa_lo);
                draw_point(c, static_cast<uint16_t>(x + w - 1U), yy, aa_lo);
            }
        } else if((row == 0U) || (row == static_cast<uint16_t>(h - 1U))) {
            solid_inset = 2U;
            draw_point(c, static_cast<uint16_t>(x + 1U), yy, aa_hi);
            draw_point(c, static_cast<uint16_t>(x + w - 2U), yy, aa_hi);
        } else if((row == 1U) || (row == static_cast<uint16_t>(h - 2U))) {
            solid_inset = 1U;
            draw_point(c, x, yy, aa_lo);
            draw_point(c, static_cast<uint16_t>(x + w - 1U), yy, aa_lo);
        }
        if(w > static_cast<uint16_t>(2U * solid_inset)) {
            fill_rect(c, static_cast<uint16_t>(x + solid_inset), yy,
                      static_cast<uint16_t>(w - (2U * solid_inset)), 1U, color);
        }
    }
}

void format_ug_x10(char* buf, uint16_t ug_x10)
{
    if(ug_x10 > kUgX10Max) {
        ug_x10 = kUgX10Max;
    }
    uint16_t ip = static_cast<uint16_t>(ug_x10 / 10U);
    const uint16_t fp = static_cast<uint16_t>(ug_x10 % 10U);
    char* p = buf;
    if(ip >= 100U) {
        *p++ = static_cast<char>('0' + (ip / 100U));
        ip = static_cast<uint16_t>(ip % 100U);
        *p++ = static_cast<char>('0' + (ip / 10U));
        *p++ = static_cast<char>('0' + (ip % 10U));
    } else if(ip >= 10U) {
        *p++ = static_cast<char>('0' + (ip / 10U));
        *p++ = static_cast<char>('0' + (ip % 10U));
    } else {
        *p++ = static_cast<char>('0' + ip);
    }
    *p++ = '.';
    *p++ = static_cast<char>('0' + fp);
    *p++ = 'u';
    *p++ = 'g';
    *p++ = '/';
    *p++ = 'm';
    *p = '\0';
}

} // namespace

SpiPm25Lcd& SpiPm25Lcd::instance()
{
    static SpiPm25Lcd inst;
    return inst;
}

SpiPm25Lcd::~SpiPm25Lcd()
{
    std::lock_guard<std::mutex> lock(mtx_);
    close_locked();
}

void SpiPm25Lcd::close_locked()
{
    if(shadow_) {
        delete[] shadow_;
        shadow_ = nullptr;
    }
    shadow_bytes_ = 0;
    stride_px_ = 0;
    fb_w_ = 0;
    fb_h_ = 0;
    if(fb_fd_ >= 0) {
        ::close(fb_fd_);
        fb_fd_ = -1;
    }
    ui_ready_ = false;
}

bool SpiPm25Lcd::ensure_open_locked()
{
    if(fb_fd_ >= 0 && shadow_) {
        return true;
    }
#if defined(__linux__)
    char path[64];
    if(!find_fb_by_name(kFbName, path, sizeof(path))) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd: fb name %s not found", kFbName);
        }
        return false;
    }
    fb_fd_ = ::open(path, O_RDWR | O_CLOEXEC);
    if(fb_fd_ < 0) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd open %s failed errno=%d", path, errno);
        }
        return false;
    }

    fb_var_screeninfo vinfo{};
    fb_fix_screeninfo finfo{};
    if(::ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
       ::ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd ioctl failed errno=%d", errno);
        }
        close_locked();
        return false;
    }
    if(vinfo.bits_per_pixel != 16) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd bpp=%u want 16", vinfo.bits_per_pixel);
        }
        close_locked();
        return false;
    }
    if(finfo.line_length < 2) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd bad line_length=%u", finfo.line_length);
        }
        close_locked();
        return false;
    }

    fb_w_ = vinfo.xres;
    fb_h_ = vinfo.yres;
    stride_px_ = finfo.line_length / 2U;
    shadow_bytes_ = static_cast<size_t>(finfo.line_length) * fb_h_;
    shadow_ = new (std::nothrow) uint16_t[shadow_bytes_ / sizeof(uint16_t)];
    if(!shadow_) {
        if(!open_warn_done_) {
            open_warn_done_ = true;
            UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd shadow alloc failed");
        }
        close_locked();
        return false;
    }
    std::memset(shadow_, 0, shadow_bytes_);
    UPC_LOG_GLAVO_SERVICE(Info, "spi pm25 lcd open %s %ux%u stride=%u", path, fb_w_, fb_h_,
                          stride_px_);
    return true;
#else
    if(!open_warn_done_) {
        open_warn_done_ = true;
        UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd not supported on this host");
    }
    return false;
#endif
}

void SpiPm25Lcd::flush_locked()
{
    if(fb_fd_ < 0 || !shadow_ || shadow_bytes_ == 0) {
        return;
    }
#if defined(__linux__)
    const ssize_t n = ::pwrite(fb_fd_, shadow_, shadow_bytes_, 0);
    if(n < 0 && !open_warn_done_) {
        open_warn_done_ = true;
        UPC_LOG_GLAVO_SERVICE(Warn, "spi pm25 lcd pwrite failed errno=%d", errno);
    }
#else
    (void)0;
#endif
}

void SpiPm25Lcd::draw_value_line_locked(const char* line2, bool with_super3)
{
    DrawCtx c{shadow_, shadow_bytes_ / sizeof(uint16_t), stride_px_, fb_w_};
    uint16_t body_w = measure_scaled(line2, static_cast<uint8_t>(kUiScale));
    uint16_t super_w = with_super3 ? measure_half("3") : 0U;
    uint16_t line2_w = static_cast<uint16_t>(body_w + (with_super3 ? (1U + super_w) : 0U));
    if(line2_w > kUiW) {
        line2_w = kUiW;
    }
    const uint16_t x_line2 = static_cast<uint16_t>((kUiW - line2_w) / 2U);

    uint16_t clear_x = (x_line2 < value_area_x_) ? x_line2 : value_area_x_;
    uint16_t clear_right = static_cast<uint16_t>(value_area_x_ + value_area_w_);
    const uint16_t new_right = static_cast<uint16_t>(x_line2 + line2_w);
    if(new_right > clear_right) {
        clear_right = new_right;
    }
    fill_rect(c, clear_x, value_y_, static_cast<uint16_t>(clear_right - clear_x), ch_h_,
              kColorBlack);

    show_scaled(c, x_line2, value_y_, line2, kColorWhite, kColorBlack,
                static_cast<uint8_t>(kUiScale));
    if(with_super3) {
        const uint16_t super_x = static_cast<uint16_t>(x_line2 + body_w + 1U);
        const uint16_t super_y = static_cast<uint16_t>(value_y_ + 1U);
        show_half(c, super_x, super_y, "3", kColorWhite, kColorBlack);
    }

    value_area_x_ = x_line2;
    value_area_w_ = line2_w;
}

void SpiPm25Lcd::draw_static_locked()
{
    if(shadow_) {
        std::memset(shadow_, 0, shadow_bytes_);
    }
    DrawCtx c{shadow_, shadow_bytes_ / sizeof(uint16_t), stride_px_, fb_w_};
    ch_h_ = static_cast<uint16_t>(FONT_RANY_H * kUiScale);
    const uint16_t title_w = measure_scaled("PM2.5", static_cast<uint8_t>(kUiScale));
    uint16_t bar_w = title_w;
    if(bar_w < 40U) {
        bar_w = 40U;
    }
    const uint16_t block_h = static_cast<uint16_t>(ch_h_ + kGapTitle + ch_h_ + kGapBar + kBarH);
    const uint16_t title_y = static_cast<uint16_t>((kUiH - block_h) / 2U);
    value_y_ = static_cast<uint16_t>(title_y + ch_h_ + kGapTitle);
    const uint16_t bar_y = static_cast<uint16_t>(value_y_ + ch_h_ + kGapBar);
    const uint16_t bar_x = static_cast<uint16_t>((kUiW - bar_w) / 2U);
    const uint16_t x_title = static_cast<uint16_t>((kUiW - title_w) / 2U);

    value_area_x_ = 0U;
    value_area_w_ = kUiW;

    show_scaled(c, x_title, title_y, "PM2.5", kColorWhite, kColorBlack,
                static_cast<uint8_t>(kUiScale));
    fill_round_bar(c, bar_x, bar_y, bar_w, kBarH, kColorGreen);

    ui_ready_ = true;
    last_pm_ = -2.f;
    draw_value_line_locked("--", false);
    flush_locked();
}

void SpiPm25Lcd::set_value_locked(float pm25_ug_m3)
{
    if(!ui_ready_) {
        return;
    }
    if(std::fabs(pm25_ug_m3 - last_pm_) < 0.05f) {
        return;
    }
    last_pm_ = pm25_ug_m3;

    if(!(pm25_ug_m3 >= 0.0f)) {
        draw_value_line_locked("--", false);
        flush_locked();
        return;
    }

    /* 首页/监控页 Home PM2.5 与本屏同值，UI 侧（ui_air_quality.cpp）照抄这里的进位与拆位
     * 做到逐位一致；改这里要同步改那边。 */
    const float scaled = (pm25_ug_m3 * 10.0f) + 0.5f;
    uint16_t ug_x10 = 0;
    if(scaled > static_cast<float>(kUgX10Max)) {
        ug_x10 = kUgX10Max;
    } else {
        ug_x10 = static_cast<uint16_t>(scaled);
    }
    char line2[12];
    format_ug_x10(line2, ug_x10);
    draw_value_line_locked(line2, true);
    flush_locked();
}

void SpiPm25Lcd::flush_test_locked()
{
    if(!ensure_open_locked() || !shadow_) {
        return;
    }
    static const uint16_t kColors[] = {
        0xF800U, /* 红 */
        0x07E0U, /* 绿 */
        0x001FU, /* 蓝 */
        0xFFFFU, /* 白 */
        0x0000U, /* 黑 */
    };
    constexpr uint8_t kN = static_cast<uint8_t>(sizeof(kColors) / sizeof(kColors[0]));
    const uint16_t color = kColors[test_idx_ % kN];
    test_idx_ = static_cast<uint8_t>((test_idx_ + 1U) % kN);

    DrawCtx c{shadow_, shadow_bytes_ / sizeof(uint16_t), stride_px_, fb_w_};
    fill_rect(c, 0, 0, kUiW, kUiH, color);
    flush_locked();
    ui_ready_ = false; /* 退出测试后须重画静态布局 */
    last_pm_ = -2.f;
    UPC_LOG_GLAVO_SERVICE(Info, "spi pm25 lcd test pwrite color=0x%04X",
                          static_cast<unsigned>(color));
}

void SpiPm25Lcd::set_test_pattern(bool on)
{
    std::lock_guard<std::mutex> lock(mtx_);
    test_pattern_ = on;
    started_ = true;
    if(!on) {
        if(ensure_open_locked()) {
            draw_static_locked();
        }
        UPC_LOG_GLAVO_SERVICE(Info, "spi pm25 lcd test off");
        return;
    }
    test_idx_ = 0;
    flush_test_locked();
}

bool SpiPm25Lcd::test_pattern_on() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return test_pattern_;
}

void SpiPm25Lcd::start()
{
    std::lock_guard<std::mutex> lock(mtx_);
    started_ = true;
    if(!ensure_open_locked()) {
        return;
    }
    if(test_pattern_) {
        flush_test_locked();
        return;
    }
    draw_static_locked();
}

void SpiPm25Lcd::update(float pm25_ug_m3)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(!started_) {
        return;
    }
    if(test_pattern_) {
        flush_test_locked();
        return;
    }
    if(!ensure_open_locked()) {
        return;
    }
    if(!ui_ready_) {
        draw_static_locked();
    }
    set_value_locked(pm25_ug_m3);
}

} // namespace glavo
} // namespace upc
