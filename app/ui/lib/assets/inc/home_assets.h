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

/* 首页背景层，按屏分辨率两份（定义在 home_bg.c，由 tools/gen_home_bg.py 生成）。
 * 别直接引用某一份——用 page_user_home.cpp 的 home_bg_dsc() 按当前 profile 挑。
 * 1280 档来自「背景图1」、1024 档来自「背景图2」，两个节点的取舍见那个脚本的注释。 */
extern const lv_image_dsc_t home_bg_1280;
extern const lv_image_dsc_t home_bg_1024;

#ifdef __cplusplus
}
#endif

#endif
