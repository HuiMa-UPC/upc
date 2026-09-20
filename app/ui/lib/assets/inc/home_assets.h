/* 手维护。原先由 tools/gen_home_assets.py 生成，那个脚本已不在仓库里；
 * 本头里的三枚图标（ic_pm / ic_temp / ic_o2）也已经退场：
 *   - ic_temp / ic_o2 重新从稿描过，收进 tools/gen_home_metric_icons.py，
 *     现在按档位出多份原生尺寸，见 home_metric_icons.h 的 ic_temp_set / ic_o2_set
 *   - ic_pm 随舱内 PM2.5 那行一起删掉后已无调用点
 * 随之 home_assets.c 整个删除，本头只剩背景图声明。
 */
#ifndef HOME_ASSETS_H
#define HOME_ASSETS_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* 首页待机态的两张预生成位图，都按屏分辨率两份，由 tools/gen_home_bg.py 生成。
 * 别直接引用某一份——用 page_user_home.cpp 的 home_device_dsc() / home_card_bg_dsc()
 * 按当前 profile 挑；1920 档复用 1280 那份（放大 1.5×、会软，该档不出货）。
 *
 * 设备渲染图（稿 6039:66132）：592 见方、RGB565A8。浅色稿把整机从「铺满整屏的背景」
 * 改成了「浅底上定位摆放的一张图」，旧的 home_bg_* 已随之退场，来龙去脉见生成脚本。 */
extern const lv_image_dsc_t home_device_1280;
extern const lv_image_dsc_t home_device_1024;

/* HOME 空气质量卡的背景（稿 6064:70631）：径向渐变 + 点阵 + 底缘柔光烤成一张
 * 不透明 RGB565；卡的圆角不在图里，运行时由 LVGL 的 radius 裁。 */
extern const lv_image_dsc_t home_card_bg_1280;
extern const lv_image_dsc_t home_card_bg_1024;

#ifdef __cplusplus
}
#endif

#endif
