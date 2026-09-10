#include "page_user_control.hpp"

#include "ui_theme.hpp"
#include "ui_user_widgets.hpp"
#include "ui_feedback.hpp"
#include "ui_keyboard.hpp"
#include "ui_slide_unlock.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_device_state.hpp"
#include "ui_notice_stack.hpp"
#include "ui_clean_state.hpp"
#include "ui_msg_proxy.hpp"
#include "control.hpp"
#include "debug_msg_proxy.hpp"
#include "home_metric_icons.h"
#include "config/cal_config.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

/**
 * @file page_user_control.cpp
 * @brief 控制页：主面板（稿 5782:12911）+ 更多控制（稿 5814:16936 / 5815:17390）
 *        + 两个调整弹窗（稿 5788:14284 / 15012）。
 *
 * 两个视图共存于同一个 Page，靠 `show_view()` 显隐切换，理由见头文件。
 *
 * 数据来源分三路，都不是本页自己订阅的——三处订阅点全是单槽的，抢了会静默弄坏别人：
 *   - 环境四项（舱内/室内 PM2.5、舱温、氧浓度）→ `ui_device_state` 观察者；
 *   - 耗材/滤芯 → `control_supply_*`（Proxy 那个单槽回调由 control.cpp 持有）；
 *   - 传感器条 / 三轴位置 / 设备开关态 → `DebugMsgProxy` 的 sensor/axis/device
 *     回调，与调试页共用同一组单槽，故**进页注册、离页清空**（见 page_user_control_exit）。
 *
 * 稿上文案是中文，本工程 UI 统一英文（Montserrat 字库，CJK 子集只覆盖调试测试页），
 * 术语一律沿用调试页已有的英文串，见下面 kAxes / 设备控制那几处注释。
 */

/* ============================================================================
 *                  开 发 期 临 时 代 码 —— 上 机 前 必 须 删 除
 *
 * 清粉 10s 超时兜底。Powder Clean 按设计是「跑起来就置灰、只能等 MCU 回
 * APP_Clean_Test_Result_Ntf 才解锁」，联调期 MCU 不回就把界面永久锁在忙态、没有退路。
 * 这个开关给一条 10s 的自解锁路径，纯粹为了把界面捞出来。
 *
 * **它是假的兜底**：真实清粉一轮远超 10s，所以开发期它会在清粉还在跑的时候把开关翻回
 * Off。这是预期行为，别当成 bug 去查。
 *
 * 删除清单（少删一样就编不过或留下死代码）：
 *   1. 本注释块 + 紧随的 #define UPC_CONTROL_DEV_CLEAN_TIMEOUT + 那个 #warning 的 #if 块；
 *   2. 「清粉忙态复位」后面那整个 #if / #else / #endif 块（含 #else 里的空实现）；
 *   3. 五个调用点：dev_clean_timeout_arm() 两处（on_pm_purify / on_clean_toggle）、
 *      dev_clean_timeout_disarm() 三处（REJECTED / DONE / page_user_control_exit）；
 *   4. app/ui/UI.md「控制页」一节里那条「⚠ 待删（上机前）」引用块。
 *
 * grep `UPC_CONTROL_DEV_CLEAN_TIMEOUT` 与 `dev_clean_timeout` 两个词能把 1~3 全找出来。
 * ========================================================================== */
#define UPC_CONTROL_DEV_CLEAN_TIMEOUT 1

#if UPC_CONTROL_DEV_CLEAN_TIMEOUT
/* 每次编译都刷这一条，是为了让它不可能被忘掉（工程没开 -Werror，只提示不中断构建）。 */
#warning "DEV: control page powder-clean 10s timeout is ENABLED - remove before release"
#endif

namespace {

using middleware::DebugAxis;
using middleware::DebugDeviceTarget;
using middleware::DebugJogMode;
using middleware::DebugMotorAction;
using middleware::DebugSwitchTarget;

/* ============================ 页面几何 ============================
 * 稿是 1280×800 原生，1024 档按屏宽/屏高折算。这些值只有本页用，故放页面局部
 * helper 而不进 ui_theme——同 page_user_history.cpp 的 chrome_pad_l() 那套做法。 */

bool is_1280(void) { return ui_theme_get_profile() == UI_RES_1280; }

/** 内容区左内距：稿上左列在 x=176，内容区从侧栏右缘（128）起，故 48。 */
int32_t body_pad_l(void)  { return is_1280() ? 48 : USER_PAD; }
/** 卡片顶距：稿上卡片顶在 y=125，内容区顶在 USER_CONTENT_TOP(112)，差 13。 */
int32_t body_pad_t(void)  { return is_1280() ? 13 : USER_PAD_SM / 2; }
/** 两列间距：稿 463 − (176+272) = 15。 */
int32_t col_gap(void)     { return is_1280() ? 15 : USER_PAD_SM; }
/** 列宽：稿左列 272、右列 275，差 3 是稿自身取整，实现取同一个值。 */
int32_t col_w(void)       { return is_1280() ? 272 : 220; }
/** 卡片间距：稿右列 frame 明写 gap 7px，左列各卡间距实测也是 7~8。 */
int32_t card_gap(void)    { return is_1280() ? 7 : 6; }
/** 卡内边距：稿 16。 */
int32_t card_pad(void)    { return USER_PAD_SM; }
/** 进度条高：稿 8，圆角 2。 */
int32_t bar_h(void)       { return is_1280() ? 8 : 6; }
/** 开关轨道 64×32、圆钮 40 宽（稿上是圆角矩形不是圆，故单独给宽）。 */
int32_t sw_track_w(void)  { return is_1280() ? 64 : 52; }
int32_t sw_track_h(void)  { return is_1280() ? 32 : 26; }
int32_t sw_knob_w(void)   { return is_1280() ? 40 : 32; }

/* ---- 列高与卡高一律钉死，不让内容累加决定 ----
 *
 * 稿上两列都精确地从 y=125 铺到 y=729、**各 604 高**，靠的是每张卡自己的确定高度：
 *   左：162 + 7 + 156 + 7 + 200 + 8 + 64 = 604
 *   右：156 + 7 + 156 + 7 + 100 + 7 + 100 + 7 + 64 = 604
 * （左列首张 162 比第二张 156 高 6，是稿上 INTERIOR 那枚胶囊内距用了四周 8px、
 *   而其余是 4px 8px；这 6px 不单独建模，落进 SPACE_BETWEEN 的行距里。）
 *
 * **不能让卡高由内容自适应**，两个理由：
 *   1. 两列的卡种类不同（左 4 张、右 5 张），累加出来的高度必然不等，底边对不齐；
 *   2. 指标卡的状态胶囊在无数据时要隐掉，而 flex 会跳过 HIDDEN 子节点——卡片会当场
 *      矮一截（约 37px），等数据一到又长回去，**列高跟着运行时数据跳**。实机上就是
 *      这个现象：无环境数据时左列反而比右列短。
 * 钉死之后胶囊显隐不再影响卡高（且下面 apply_metric() 改用透明占位而非 HIDDEN，
 * 连卡内那条进度条的位置也不会挪）。
 *
 * 1280 档取稿值，内容正好装满（逐项核过）：
 *   指标卡 = 内距 32 + 卡头 67 + 12 + 条 8 + 12 + 胶囊 25 = 156，一像素不剩；
 *   组合卡 199 ≤ 200、状态卡 99 ≤ 100、窄卡 32 + 32 = 64 刚好。
 *   左列 576 + 3×7 = 597 ≤ 604（余 7 进行距）；右列 576 + 4×7 = 604，正好。
 *
 * **1024 档不能按 0.75 折算**：那样得 117/150/75/48，而按该档字体实测（value 24 行高 33、
 * caption_sm 12 行高 17、内距 11、图标 26）指标卡要 118、组合卡要 151，会裁掉 1~3px。
 * 故这一档按算术给值、各留几像素余量，col_h 取两列所需的较大者（右列 468）再加 2。
 * 可用高度是 600 − 84(content_top) − 8(body_pad_t) = 508，仍余 38。 */
int32_t col_h(void)         { return is_1280() ? 604 : 470; }
int32_t card_h_metric(void) { return is_1280() ? 156 : 120; }  /**< 指标卡（条+胶囊） */
int32_t card_h_combo(void)  { return is_1280() ? 200 : 152; }  /**< 温度+氧组合卡（稿明写 200） */
int32_t card_h_state(void)  { return is_1280() ? 100 : 78;  }  /**< 带状态字/数值的卡 */
int32_t card_h_slim(void)   { return is_1280() ? 64  : 48;  }  /**< LIGHT 与 More Controls */

/** 数字输入框尺寸（稿 80×48）。风扇的 Send 也取这个高度，见那处注释。 */
int32_t num_input_w(void)   { return is_1280() ? 80 : 66; }
int32_t num_input_h(void)   { return is_1280() ? 48 : 40; }

/* ---- 更多控制的 chrome 纵向节奏 ----
 * 稿（两个 Tab 共用）：chrome 行 y=125 高 40、传感器条 y=185 高 109、内容 y=318，
 * 而 Motion 的轴卡高 434（`layout_4c04db04` 里显式写死）→ 底边距 800−318−434 = 48。
 * 原先两段间距都用了统一的 USER_PAD(24)、底边距也是 24，于是条与内容各下移 4、
 * 内容区高出 20。按下面三个数给完，四个坐标全部对上稿。
 *
 * **稿自身在两个 Tab 上不自洽**：Motion 的内容收在 752，Device 的却收在 779
 * （`#5815:17746` 在 y=555 高 224）。共享同一个内容区高只能取一个，取 Motion 那个
 * 434——它是稿里显式的尺寸，Device 那边是由两块坐标反推的。 */
int32_t more_gap_chrome(void) { return is_1280() ? 20 : 15; }  /**< chrome → 传感器条 */
int32_t more_gap_strip(void)  { return is_1280() ? 24 : 18; }  /**< 传感器条 → 内容 */
/**
 * 底边距。1280 档取稿的 48（于是内容区正好 434）。
 *
 * **1024 档不能按 0.75 折算成 36**：那样内容区只有 317，而该档最高的那张卡（Z 轴，多一行
 * 工作零点）按字体实测约需 318——又是 1px 之差。取 28 留出约 7px 余量。同 card_h_* 那组
 * 的处理，理由一样：这一档的字体行高不是 1280 档的 0.75 倍。
 */
int32_t more_pad_bot(void)    { return is_1280() ? 48 : 28; }

/* 稿上这几个色值主题里没有，只本页用 */
const lv_color_t kCapColor  = LV_COLOR_MAKE(0x8c, 0x8c, 0x87); /**< 卡头 caption（稿 #8C8C87） */
const lv_color_t kLightOn   = LV_COLOR_MAKE(0xff, 0xcd, 0x42); /**< LIGHT 开态轨道（稿 #FFCD42） */
const lv_color_t kSwOff     = LV_COLOR_MAKE(0x47, 0x47, 0x47); /**< 开关关态轨道（稿 #474747） */
const lv_color_t kKnobOff   = LV_COLOR_MAKE(0x81, 0x81, 0x81); /**< 关态圆钮（稿 #818181） */
const lv_color_t kTabBlue   = LV_COLOR_MAKE(0x4b, 0x87, 0xff); /**< Tab 选中字（稿 #4B87FF） */

/**
 * 卡片底色：**不透明**，且必须比页面底（#272628）更亮。
 *
 * 稿上写的是白 4%，折成不透明只有 #302F31、比页面底高 9 级——实机两块屏上都读不出轮廓
 * （1024 那块几乎完全看不见卡片边界）。这里推到等效白 10% 的 #3D3C3E，比底高 22 级。
 *
 * **别改成 `user_color_panel`(#1c1c1c) 或 `user_color_panel_alt`(#262626)**：前者比页面底
 * 更暗，卡片会变成凹陷进去的、与稿的「浮起」层次正好反过来；后者与页面底几乎同色，
 * 等于没有对比。历史页/消息页用 `user_color_panel` 是因为它们的页面底是近黑的
 * `user_color_bg`，那个前提在本页不成立。
 */
const lv_color_t kCardBg = LV_COLOR_MAKE(0x3d, 0x3c, 0x3e);

/** 稿上舱内 PM2.5 超标提示。门限不写死，见 pm_limit()。 */
const char *const kPmBadHint = "Too poor to open the gate.";

/* ============================ 页面状态 ============================ */

lv_obj_t *g_root      = nullptr;
lv_obj_t *g_main_view = nullptr;
lv_obj_t *g_more_view = nullptr;
bool      g_active    = false;   /**< 本页是否为当前页（挡住离页后到达的回调） */

ui_slide_unlock_t *g_door_slide = nullptr;

/** 一张「指标 + 进度条 + 状态胶囊」卡的可刷新句柄。 */
struct MetricCard {
    lv_obj_t *value  = nullptr;  /**< 大数值 */
    lv_obj_t *unit   = nullptr;  /**< 单位（可空） */
    lv_obj_t *track  = nullptr;  /**< 进度条轨道（可空） */
    lv_obj_t *fill   = nullptr;  /**< 进度条填充（可空） */
    lv_obj_t *pill   = nullptr;  /**< 状态胶囊底（可空） */
    lv_obj_t *pill_lbl = nullptr;
};

MetricCard g_pm_in;      /**< INTERIOR PM2.5 */
MetricCard g_pm_out;     /**< EXTERIOR PM2.5 */
MetricCard g_temp;       /**< DEVICE TEMP（组合卡上半） */
MetricCard g_o2;         /**< O2 CONCENTRATION（组合卡下半） */
MetricCard g_filter;     /**< Filter */
MetricCard g_material;   /**< Material */

lv_obj_t *g_laser_value = nullptr;   /**< 主面板 Laser Power 卡的档位词 */
uint8_t   g_laser_pct   = 50;        /**< 当前激光功率百分比（0..100） */
bool      g_laser_known = false;     /**< 是否已收到过 GLAVO_Debug_Laser_Ntf */

/* 两个开关卡：轨道 / 圆钮 / 状态字 */
struct ToggleCard {
    lv_obj_t *track = nullptr;
    lv_obj_t *knob  = nullptr;
    lv_obj_t *state = nullptr;
    bool      on    = false;
};
ToggleCard g_clean_sw;   /**< Powder Clean（一次性作业：跑起来就置灰） */
ToggleCard g_light_sw;   /**< LIGHT */
bool g_clean_busy = false;   /**< 清粉在跑：开关置灰不可取消 */

/* 弹窗 */
ui_modal_dialog_t *g_pm_dlg    = nullptr;
ui_modal_dialog_t *g_laser_dlg = nullptr;
lv_obj_t *g_pm_dlg_value  = nullptr;
lv_obj_t *g_pm_dlg_hint   = nullptr;
lv_obj_t *g_pm_dlg_hint_lbl = nullptr;
lv_obj_t *g_laser_dlg_word = nullptr;
lv_obj_t *g_laser_dlg_pct  = nullptr;
lv_obj_t *g_laser_dlg_fill = nullptr;
uint8_t   g_laser_draft    = 50;   /**< 弹窗里正在调的值，确认后才下发 */

/* ============================ 小工具 ============================ */

/**
 * 舱内 PM2.5 的「不可开门」门限。
 *
 * **不要写死 50**：后端 `pm25_clean_threshold_` 默认 50 但可由 `cal.csv` 的 Config 段
 * `PM2.5Clean` 覆盖，且开门前清粉与 `APP_PM25_Rsp.is_high` 都按那个值判。写死会让改
 * 过 cal.csv 的机器上「红胶囊说不能开门」和「实际能开门」对不上。
 */
float pm_limit(void)
{
    upc::config::CalConfigOptions cfg{};
    if(upc::config::load_cal_config_options(upc::config::kCalDefaultPath, &cfg) &&
       cfg.has_pm25_clean_threshold)
    {
        return cfg.pm25_clean_threshold;
    }
    return 50.0f;
}

/** 后端无数据的哨兵：负值或 NaN（见 EnvSnapshotPayload 的字段注释）。 */
bool valid(float v) { return !(v < 0.0f) && !std::isnan(v); }

/** 占位符一律 ASCII "--"：内置 Montserrat 不收 U+2014，破折号会渲染成豆腐块。 */
void set_num(lv_obj_t *lbl, float v, int decimals)
{
    if(!lbl) {
        return;
    }
    if(!valid(v)) {
        lv_label_set_text(lbl, "--");
        return;
    }
    lv_label_set_text_fmt(lbl, "%.*f", decimals, (double)v);
}

/**
 * 从多档 set 里挑与卡头图标尺寸等宽的那份，让上屏 1:1 不缩放。
 *
 * `lv_draw_sw_transform` 是 2 抽头点采样、不做面积平均，缩小时会整行整列跳过源像素，
 * 而这批图标描边只有 1.6px——跳掉一行就断一截。同 page_user_home.cpp 的 pick_icon()。
 *
 * `ic_layers` / `ic_cube` / `ic_purify` 那批手维护图标没有多档 set，调用方直接把单张
 * dsc 传给 `make_icon()`，不经本函数。
 */
const lv_image_dsc_t *pick_metric(const lv_image_dsc_t *const *set)
{
    const lv_image_dsc_t *best = set[0];
    int32_t best_d = INT32_MAX;
    for(const lv_image_dsc_t *const *p = set; *p != nullptr; ++p) {
        const int32_t d = LV_ABS((int32_t)(*p)->header.w - USER_METRIC_ICON_SZ);
        if(d < best_d) { best_d = d; best = *p; }
    }
    return best;
}

/** 卡头 A8 图标：统一 USER_METRIC_ICON_SZ、caption 灰。 */
lv_obj_t *make_icon(lv_obj_t *parent, const lv_image_dsc_t *src)
{
    return ui_user_make_icon_img(parent, src, USER_METRIC_ICON_SZ, kCapColor);
}

/**
 * 透明无边框容器：本文件建的所有布局盒子都从这里起手。
 *
 * **那句 `lv_obj_set_size` 不能省。** `lv_obj_class` 的默认宽高是 `LV_DPI_DEF` 见方
 * （=130，工程未覆盖），是**固定值而不是 `LV_SIZE_CONTENT`**；不显式改的话本文件几十个
 * 布局容器全是 130×130 的死盒子，后果是大数值被裁掉下半截（行容器 130 高 + 交叉轴
 * END 对齐把文字推到底部，再被上层固定高度裁掉）、卡片撑到 218 高把整列顶出屏幕、
 * 弹窗里 "INTERIOR PM2.5" 被裁成 "INTERIOR PM"。实机上全踩过一遍。
 *
 * 需要百分比宽/高或 `flex_grow` 的调用点在这之后各自覆盖对应那一维，互不冲突。
 */
lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *make_row(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = make_box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

lv_obj_t *make_col(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = make_box(parent);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                     const lv_font_t *font, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

/**
 * 稿上那种玻璃卡：白 4% 底 + 白 10% 描边 0.5px + 圆角 8。
 *
 * 稿另有 `backdrop blur(25px)`，**LVGL 画不了背景模糊**，与首页那两枚玻璃钮的
 * inset 阴影同样略去（见 UI.md「左下两枚入口胶囊」）。描边取 1px：0.5px 在整数
 * 像素栅格上画不出来，LVGL 会按 1 处理。底色见 kCardBg 的说明。
 */
lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, kCardBg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, user_color_text, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_10, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, USER_BTN_RADIUS, 0);
    lv_obj_set_style_pad_all(c, card_pad(), 0);
    lv_obj_set_style_pad_row(c, USER_PAD_SM * 3 / 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/** 卡右侧那枚 ">" 箭头（稿上 20px、50% 不透明）。 */
lv_obj_t *make_chevron(lv_obj_t *parent)
{
    lv_obj_t *l = make_label(parent, LV_SYMBOL_RIGHT, user_font_text_sm(),
                             user_color_text, LV_OPA_50);
    return l;
}

/* ============================ 指标卡 ============================ */

/**
 * 卡头一行：[图标] [caption / 大数值 + 单位] ---撑开--- [可选箭头]。
 * caption 走 12 号（稿值）并带 0.083em 字距——LVGL 没有字距样式，只落地字号与配色。
 */
void build_metric_head(lv_obj_t *card, MetricCard *out,
                       const lv_image_dsc_t *icon,
                       const char *caption, const char *unit, bool chevron)
{
    lv_obj_t *head = make_row(card, USER_PAD_SM / 2);
    lv_obj_set_width(head, LV_PCT(100));

    make_icon(head, icon);

    lv_obj_t *txt = make_col(head, USER_PAD_SM / 3);
    lv_obj_set_flex_grow(txt, 1);

    make_label(txt, caption, user_font_caption_sm(), kCapColor, LV_OPA_COVER);

    lv_obj_t *vrow = make_row(txt, USER_PAD_SM / 4);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    out->value = make_label(vrow, "--", user_font_value(), user_color_text,
                            LV_OPA_COVER);
    if(unit) {
        out->unit = make_label(vrow, unit, user_font_caption_sm(),
                               user_color_text, LV_OPA_50);
    }

    if(chevron) {
        make_chevron(head);
    }
}

/** 进度条：轨道 + 定宽填充（不用 lv_bar，稿上填充带自己的圆角与外发光）。 */
void build_metric_bar(lv_obj_t *card, MetricCard *out)
{
    out->track = lv_obj_create(card);
    lv_obj_remove_style_all(out->track);
    lv_obj_set_size(out->track, LV_PCT(100), bar_h());
    lv_obj_set_style_radius(out->track, bar_h() / 4 + 1, 0);
    lv_obj_set_style_bg_color(out->track, user_color_text, 0);
    lv_obj_set_style_bg_opa(out->track, LV_OPA_10 / 2, 0);
    lv_obj_remove_flag(out->track, LV_OBJ_FLAG_SCROLLABLE);

    out->fill = lv_obj_create(out->track);
    lv_obj_remove_style_all(out->fill);
    lv_obj_set_height(out->fill, LV_PCT(100));
    lv_obj_set_width(out->fill, 0);
    lv_obj_set_style_radius(out->fill, bar_h() / 4 + 1, 0);
    lv_obj_set_style_bg_color(out->fill, user_color_status_ok, 0);
    lv_obj_set_style_bg_opa(out->fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(out->fill, LV_OBJ_FLAG_SCROLLABLE);
}

/** 状态胶囊：[小圆点] [文字]，底色是文字色的 10%。 */
void build_metric_pill(lv_obj_t *card, MetricCard *out)
{
    out->pill = lv_obj_create(card);
    lv_obj_remove_style_all(out->pill);
    lv_obj_set_size(out->pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(out->pill, USER_PAD_SM / 2, 0);
    lv_obj_set_style_pad_ver(out->pill, USER_PAD_SM / 4, 0);
    lv_obj_set_style_radius(out->pill, USER_BTN_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(out->pill, user_color_status_ok, 0);
    lv_obj_set_style_bg_opa(out->pill, LV_OPA_10, 0);
    lv_obj_set_flex_flow(out->pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(out->pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(out->pill, USER_PAD_SM / 4, 0);
    lv_obj_remove_flag(out->pill, LV_OBJ_FLAG_SCROLLABLE);

    out->pill_lbl = make_label(out->pill, "Good", user_font_caption_sm(),
                               user_color_status_ok, LV_OPA_COVER);
}

/**
 * 按百分比与色调刷新一张指标卡的条与胶囊。
 * @param pct   0..100；<0 表示无数据（条清零、胶囊隐藏）
 * @param good  true 走绿档，false 走红档
 * @param text  胶囊文字；nullptr 则隐藏胶囊
 */
void apply_metric(MetricCard *c, float pct, bool good, const char *text)
{
    const lv_color_t color = good ? user_color_status_ok : user_color_status_err;

    if(c->track && c->fill) {
        const int32_t w = (pct <= 0.0f) ? 0
                        : (pct >= 100.0f) ? 100
                        : (int32_t)pct;
        lv_obj_set_width(c->fill, LV_PCT(w));
        lv_obj_set_style_bg_color(c->fill, color, 0);
        lv_obj_set_style_bg_color(c->track, color, 0);
    }
    if(c->pill) {
        if(text) {
            lv_obj_set_style_opa(c->pill, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(c->pill, color, 0);
            lv_label_set_text(c->pill_lbl, text);
            lv_obj_set_style_text_color(c->pill_lbl, color, 0);
        } else {
            /* 无数据时**只是透明掉、不用 LV_OBJ_FLAG_HIDDEN**：flex 会跳过 HIDDEN 的
             * 子节点，胶囊一藏卡内那条进度条就会往下挪。卡高虽已钉死（card_h_metric），
             * 但内部排布仍会跳；透明占位保住位置。 */
            lv_obj_set_style_opa(c->pill, LV_OPA_TRANSP, 0);
        }
    }
}

/* ============================ 开关控件 ============================ */

/**
 * 稿上的开关（64×32 轨道 + 40 宽圆角矩形钮）。
 *
 * **不用 `lv_switch`**：它的钮是按轨道高推出来的方/圆形，稿这颗是 40 宽的横向圆角
 * 矩形（比轨道高还宽），套 `lv_switch` 要跟它的默认几何反复较劲，不如直接两层
 * `lv_obj` + 一条位移动画。全工程原本也没有任何 `lv_switch` 用例。
 */
void build_toggle(lv_obj_t *parent, ToggleCard *out, lv_event_cb_t cb)
{
    out->track = lv_obj_create(parent);
    lv_obj_remove_style_all(out->track);
    lv_obj_set_size(out->track, sw_track_w(), sw_track_h());
    lv_obj_set_style_radius(out->track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(out->track, kSwOff, 0);
    lv_obj_set_style_bg_opa(out->track, LV_OPA_COVER, 0);
    lv_obj_add_flag(out->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(out->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(out->track, cb, LV_EVENT_CLICKED, out);

    const int32_t inset = 2;   /* 稿 padding 2 */
    out->knob = lv_obj_create(out->track);
    lv_obj_remove_style_all(out->knob);
    lv_obj_set_size(out->knob, sw_knob_w(), sw_track_h() - inset * 2);
    lv_obj_set_style_radius(out->knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(out->knob, kKnobOff, 0);
    lv_obj_set_style_bg_opa(out->knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(out->knob, user_color_text, 0);
    lv_obj_set_style_border_opa(out->knob, LV_OPA_10, 0);
    lv_obj_set_style_border_width(out->knob, 1, 0);
    lv_obj_remove_flag(out->knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(out->knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(out->knob, LV_ALIGN_LEFT_MID, inset, 0);
}

/**
 * 落一次开关外观。
 * @param on_color 开态轨道色（LIGHT 用稿上的黄，Powder Clean 用主色蓝）
 * @param dim      置灰（清粉进行中不可取消）
 */
void apply_toggle(ToggleCard *t, bool on, lv_color_t on_color, bool dim)
{
    if(!t->track || !t->knob) {
        return;
    }
    t->on = on;
    const int32_t inset = 2;
    lv_obj_set_style_bg_color(t->track, on ? on_color : kSwOff, 0);
    lv_obj_set_style_bg_opa(t->track, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    /* 开态钮色取轨道色压暗后的同色系（稿 LIGHT 开态是 #4A4333，即黄的暗版） */
    lv_obj_set_style_bg_color(t->knob, on ? lv_color_darken(on_color, LV_OPA_70)
                                          : kKnobOff, 0);
    lv_obj_align(t->knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                 on ? -inset : inset, 0);
    if(dim) {
        lv_obj_remove_flag(t->track, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(t->track, LV_OBJ_FLAG_CLICKABLE);
    }
}

/** 清粉忙态复位：三处（超时、REJECTED、DONE）共用一份，别各写各的。 */
void clean_reset_idle(void)
{
    g_clean_busy = false;
    apply_toggle(&g_clean_sw, false, user_color_accent, false);
    if(g_clean_sw.state) {
        lv_label_set_text(g_clean_sw.state, "Off");
    }
}

#if UPC_CONTROL_DEV_CLEAN_TIMEOUT
/* ---- 开发期临时代码（见文件头那块说明），上机前删 ---- */
lv_timer_t *g_dev_clean_timer = nullptr;

void dev_clean_timeout_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    g_dev_clean_timer = nullptr;
    if(!g_clean_busy) {
        return;
    }
    clean_reset_idle();
    ui_toast_show("[DEV] clean timeout, unlocked", user_color_status_err, 2200);
}

/**
 * 起 10s 计时。**起点是「发起时」而不是「收到 ACCEPTED 时」**：如果 Glavo 整个没响应，
 * ACCEPTED 同样不会来，那种情况也得能自解锁。
 */
void dev_clean_timeout_arm(void)
{
    if(g_dev_clean_timer) {
        lv_timer_delete(g_dev_clean_timer);
    }
    g_dev_clean_timer = lv_timer_create(dev_clean_timeout_cb, 10000, nullptr);
    lv_timer_set_repeat_count(g_dev_clean_timer, 1);
}

/** 真回执到达 / 离页时必须撤，否则它会晚一步把已经正常的状态再踩一遍。 */
void dev_clean_timeout_disarm(void)
{
    if(g_dev_clean_timer) {
        lv_timer_delete(g_dev_clean_timer);
        g_dev_clean_timer = nullptr;
    }
}
#else
inline void dev_clean_timeout_arm(void)    {}
inline void dev_clean_timeout_disarm(void) {}
#endif

/* ============================ 激光功率 ============================ */

/** 档位词按区间给（<33 Low / 33~66 Medium / >66 High）。 */
const char *laser_word(uint8_t pct)
{
    if(pct < 33) {
        return "Low";
    }
    if(pct <= 66) {
        return "Medium";
    }
    return "High";
}

/** ± 的步长。稿上没给，取 5%。 */
constexpr uint8_t kLaserStep = 5;

/* ============================ 视图切换 ============================ */

void build_more_view(lv_obj_t *parent);          /**< 见「更多控制」一节 */
void more_view_on_enter(void);
void more_view_register_callbacks(void);

/**
 * 主面板 ⇄ 更多控制。
 *
 * 顺带管通知浮层：更多控制那两个 Tab 的传感器条横跨 x=176..1248、顶在 y=185，而浮层
 * 贴右上退 32、宽 400，正好压在上面且会吃掉触摸。`apply_device_bar()` 换页时会把显隐
 * 按默认规则重置，所以本页每次进页（`page_user_control_refresh`）都要按当前视图再设
 * 一次——同首页 `sync_notice_offset()` 的处理。
 */
void show_view(bool more)
{
    if(!g_main_view || !g_more_view) {
        return;
    }
    if(more) {
        lv_obj_add_flag(g_main_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_more_view, LV_OBJ_FLAG_HIDDEN);
        more_view_on_enter();
    } else {
        lv_obj_remove_flag(g_main_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_more_view, LV_OBJ_FLAG_HIDDEN);
    }
    ui_notice_stack_set_visible(!more);
}

bool in_more_view(void)
{
    return g_more_view && !lv_obj_has_flag(g_more_view, LV_OBJ_FLAG_HIDDEN);
}

/* ============================ 数据刷新 ============================ */

/** 环境四项 → 左列前三张卡。 */
void apply_env(const ui_device_snapshot_t *s)
{
    if(!s) {
        return;
    }

    /* 稿上 INTERIOR = 舱内、EXTERIOR = 机器所在房间，按字面语义对字段 */
    set_num(g_pm_in.value, s->pm25_cabin, 1);
    if(valid(s->pm25_cabin)) {
        const float limit = pm_limit();
        const bool ok = s->pm25_cabin < limit;
        /* 条按门限归一化而不是按 100：稿上 73.6 的条占了约一半，而 PM 读数上限并非
         * 100；用门限的两倍做满量程，门限处正好半条，与稿一致且有物理意义。 */
        const float pct = s->pm25_cabin / (limit * 2.0f) * 100.0f;
        apply_metric(&g_pm_in, pct, ok, ok ? "Good" : kPmBadHint);
    } else {
        apply_metric(&g_pm_in, -1.0f, true, nullptr);
    }

    set_num(g_pm_out.value, s->pm25_indoor, 1);
    if(valid(s->pm25_indoor)) {
        const float limit = pm_limit();
        const bool ok = s->pm25_indoor < limit;
        apply_metric(&g_pm_out, s->pm25_indoor / (limit * 2.0f) * 100.0f, ok,
                     ok ? "Good" : "Poor");
    } else {
        apply_metric(&g_pm_out, -1.0f, true, nullptr);
    }

    set_num(g_temp.value, s->temperature_c, 1);
    set_num(g_o2.value, s->oxygen_percent, 1);

    if(g_pm_dlg && g_pm_dlg_value) {
        set_num(g_pm_dlg_value, s->pm25_cabin, 1);
        const bool ok = valid(s->pm25_cabin) && s->pm25_cabin < pm_limit();
        lv_obj_set_style_bg_color(g_pm_dlg_hint,
            ok ? user_color_status_ok : user_color_status_err, 0);
        lv_obj_set_style_text_color(g_pm_dlg_hint_lbl,
            ok ? user_color_status_ok : user_color_status_err, 0);
        lv_label_set_text(g_pm_dlg_hint_lbl,
            ok ? "Cabin air is normal, no purge needed"
               : "Cabin PM is too high, purge manually");
    }
}

/** 耗材/滤芯 → 右列两张卡。 */
void apply_supply(const control_supply_t *s)
{
    if(!s) {
        return;
    }
    set_num(g_filter.value, s->filter_pct, 1);
    apply_metric(&g_filter, s->filter_pct, s->filter_ok,
                 s->filter_ok ? "Good" : "Low");

    set_num(g_material.value, s->material_pct, 1);
    apply_metric(&g_material, s->material_pct, s->material_ok,
                 s->material_ok ? "Good" : "Low");
}

/** 激光功率回读 → 主面板档位词 + 弹窗（弹窗开着时不覆盖用户正在调的草稿值）。 */
void apply_laser(uint8_t pct)
{
    g_laser_pct   = pct > 100 ? 100 : pct;
    g_laser_known = true;
    if(g_laser_value) {
        lv_label_set_text(g_laser_value, laser_word(g_laser_pct));
    }
}

/* ============================ 弹窗：舱内 PM2.5 ============================ */

void close_pm_dlg(void)
{
    if(g_pm_dlg) {
        ui_modal_dialog_close(g_pm_dlg);
        g_pm_dlg = nullptr;
        g_pm_dlg_value = nullptr;
        g_pm_dlg_hint = nullptr;
        g_pm_dlg_hint_lbl = nullptr;
    }
}

void on_pm_dlg_close(lv_event_t *) { close_pm_dlg(); }

/**
 * 「Purify Now」与右列的 Powder Clean 是同一个物理动作，故走同一条 clean_test 路径，
 * 不走 `send_clean_start()`。
 *
 * 两个理由。一是**能拿到完成回执**：`UI_Clean_Start_Req` 的回执由 control.cpp 的开门
 * 流程独占（`register_clean_start_result` → `invoke_done(g_clean_cb)`，单槽且只在门流程
 * 里挂），控制页发了那条消息只会得到静默——`invoke_done` 对空回调是 no-op，既不出错也
 * 没反馈。二是**一个动作一条代码路径**：走这里，Powder Clean 开关会同步显示为忙，
 * 与实际状态一致，而不是两个控件各说一套。
 */
void on_pm_purify(lv_event_t *)
{
    close_pm_dlg();
    if(g_clean_busy) {
        ui_toast_show("Powder cleaning is busy", user_color_status_err, 1800);
        return;
    }
    g_clean_busy = true;
    apply_toggle(&g_clean_sw, true, user_color_accent, true);
    if(g_clean_sw.state) {
        lv_label_set_text(g_clean_sw.state, "Cleaning");
    }
    ui_clean_state_request(UI_CLEAN_OWNER_CONTROL, nullptr);
    dev_clean_timeout_arm();
    ui_toast_show("Purifying cabin air...", user_color_accent, 1800);
}

/**
 * 「Adjust Interior PM2.5」（稿 5788:14284 红态 / 14635 绿态）。
 *
 * 两张稿是同一个弹窗的两态，只差顶部那条提示的配色与文案，按当前 PM 值与门限自动切，
 * 不做两份布局。挂 `lv_layer_top()`：遮罩要盖住侧栏，与工程其它用户层弹窗一致。
 */
void open_pm_dlg(lv_event_t *)
{
    if(g_pm_dlg) {
        return;
    }
    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("Adjust Interior PM2.5",
                                                         is_1280() ? 540 : 432, true);
    cfg.with_close_btn = true;
    g_pm_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_pm_dlg) {
        return;
    }
    if(g_pm_dlg->close_btn) {
        lv_obj_add_event_cb(g_pm_dlg->close_btn, on_pm_dlg_close, LV_EVENT_CLICKED,
                            nullptr);
    }

    lv_obj_set_flex_flow(g_pm_dlg->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_pm_dlg->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_pm_dlg->body, USER_PAD, 0);

    /* 提示条：红/绿两态同一个控件 */
    g_pm_dlg_hint = lv_obj_create(g_pm_dlg->body);
    lv_obj_remove_style_all(g_pm_dlg_hint);
    lv_obj_set_size(g_pm_dlg_hint, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(g_pm_dlg_hint, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(g_pm_dlg_hint, USER_PAD_SM / 2, 0);
    lv_obj_set_style_radius(g_pm_dlg_hint, USER_BTN_RADIUS / 2, 0);
    lv_obj_set_style_bg_color(g_pm_dlg_hint, user_color_status_err, 0);
    lv_obj_set_style_bg_opa(g_pm_dlg_hint, LV_OPA_10, 0);
    lv_obj_remove_flag(g_pm_dlg_hint, LV_OBJ_FLAG_SCROLLABLE);
    /* 夹住宽度并允许换行：这条提示的两态文案长度不同，纯内容宽在实机上末字会压到
     * 描边上（LVGL 算的文本宽比实际字形推进略小）。宽度不够时宁可折行，别裁字。 */
    lv_obj_set_style_max_width(g_pm_dlg_hint, LV_PCT(100), 0);
    g_pm_dlg_hint_lbl = make_label(g_pm_dlg_hint, "", user_font_caption(),
                                   user_color_status_err, LV_OPA_COVER);
    lv_label_set_long_mode(g_pm_dlg_hint_lbl, LV_LABEL_LONG_WRAP);

    /* 大数值 + 单位（稿 64 号；字库最大到 user_font_value，取它） */
    lv_obj_t *vrow = make_row(g_pm_dlg->body, USER_PAD_SM / 2);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    g_pm_dlg_value = make_label(vrow, "--", user_font_value(), user_color_text,
                                LV_OPA_COVER);
    make_label(vrow, "ug/m3", user_font_text_sm(), user_color_text, LV_OPA_50);

    /* 图标 + 指标名 */
    lv_obj_t *nrow = make_row(g_pm_dlg->body, USER_PAD_SM / 2);
    lv_obj_set_flex_align(nrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_icon(nrow, pick_metric(ic_cabin_set));
    make_label(nrow, "INTERIOR PM2.5", user_font_text_sm(), kCapColor, LV_OPA_COVER);

    /* 满宽主按钮「Purify Now」 */
    lv_obj_t *btn = ui_user_make_confirm_footer_btn(g_pm_dlg->footer, "Purify Now",
                                                    true, user_color_accent,
                                                    on_pm_purify, nullptr);
    (void)btn;

    apply_env(ui_device_state_get());
}

/* ============================ 弹窗：激光功率 ============================ */

void laser_dlg_sync(void)
{
    if(!g_laser_dlg) {
        return;
    }
    if(g_laser_dlg_word) {
        lv_label_set_text(g_laser_dlg_word, laser_word(g_laser_draft));
    }
    if(g_laser_dlg_pct) {
        lv_label_set_text_fmt(g_laser_dlg_pct, "%u%%", (unsigned)g_laser_draft);
    }
    if(g_laser_dlg_fill) {
        lv_obj_set_width(g_laser_dlg_fill, LV_PCT(g_laser_draft));
    }
}

void close_laser_dlg(void)
{
    if(g_laser_dlg) {
        ui_modal_dialog_close(g_laser_dlg);
        g_laser_dlg = nullptr;
        g_laser_dlg_word = nullptr;
        g_laser_dlg_pct = nullptr;
        g_laser_dlg_fill = nullptr;
    }
}

void on_laser_dlg_close(lv_event_t *) { close_laser_dlg(); }

void on_laser_step(lv_event_t *e)
{
    const int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int v = (int)g_laser_draft + dir * (int)kLaserStep;
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    g_laser_draft = (uint8_t)v;
    laser_dlg_sync();
    /* 稿上没有确认键，± 即时生效 */
    ui::DebugMsgProxy::instance().send_laser_power_set(g_laser_draft);
    apply_laser(g_laser_draft);
}

/** 圆钮（稿上弹窗里 64、轴卡里 48、设备控制里 80；深底 + 白 10% 描边）。 */
lv_obj_t *make_round_btn(lv_obj_t *parent, const char *sym, int32_t sz,
                         lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, sz, sz);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, user_color_text, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_10 / 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, user_color_text, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_10, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    /* cb 传 nullptr = 调用方要自己挂事件（点动那三枚要听 PRESSED/RELEASED，见 attach_jog） */
    if(cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }

    lv_obj_t *l = make_label(b, sym, user_font_icon(), user_color_text,
                             LV_OPA_COVER);
    lv_obj_center(l);
    return b;
}

/**
 * 「Adjust Laser Power」（稿 5788:15012）。
 *
 * 稿上副标题写的是「当前PM值过高，请手动净化空气」——那是从 PM2.5 弹窗复制过来忘改的，
 * 这里按语义换成操作说明。底部那条按稿是**只有左圆角的填充块**（不是带 knob 的
 * slider），说明只能靠 ± 改值、条本身是被动指示，故用定宽填充而非 lv_slider。
 */
void open_laser_dlg(lv_event_t *)
{
    if(g_laser_dlg) {
        return;
    }
    g_laser_draft = g_laser_pct;

    ui_modal_dialog_cfg_t cfg = ui_modal_dialog_cfg_user("Adjust Laser Power",
                                                         is_1280() ? 540 : 432, false);
    cfg.with_close_btn = true;
    g_laser_dlg = ui_modal_dialog_open(lv_layer_top(), &cfg);
    if(!g_laser_dlg) {
        return;
    }
    if(g_laser_dlg->close_btn) {
        lv_obj_add_event_cb(g_laser_dlg->close_btn, on_laser_dlg_close,
                            LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_set_flex_flow(g_laser_dlg->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_laser_dlg->body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_laser_dlg->body, USER_PAD, 0);

    make_label(g_laser_dlg->body, "Laser power affects print quality and speed",
               user_font_text_sm(), user_color_text, LV_OPA_50);

    /* 中央档位词 + 其下的百分比小字 */
    g_laser_dlg_word = make_label(g_laser_dlg->body, "Medium", user_font_value(),
                                  user_color_text, LV_OPA_COVER);

    lv_obj_t *nrow = make_row(g_laser_dlg->body, USER_PAD_SM / 2);
    lv_obj_set_flex_align(nrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_icon(nrow, pick_metric(ic_bolt_set));
    make_label(nrow, "Current Laser Power", user_font_text_sm(), kCapColor,
               LV_OPA_COVER);
    g_laser_dlg_pct = make_label(nrow, "50%", user_font_text_sm(),
                                 user_color_text, LV_OPA_COVER);

    /* [−] [填充条] [+] */
    lv_obj_t *row = make_row(g_laser_dlg->body, USER_PAD_SM);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    make_round_btn(row, LV_SYMBOL_MINUS, USER_BTN_PRIMARY_H, on_laser_step,
                   (void *)(intptr_t)-1);

    lv_obj_t *track = lv_obj_create(row);
    lv_obj_remove_style_all(track);
    lv_obj_set_height(track, USER_BTN_PRIMARY_H);
    lv_obj_set_flex_grow(track, 1);
    lv_obj_set_style_radius(track, USER_BTN_RADIUS * 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(track, 4, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    g_laser_dlg_fill = lv_obj_create(track);
    lv_obj_remove_style_all(g_laser_dlg_fill);
    lv_obj_set_height(g_laser_dlg_fill, LV_PCT(100));
    lv_obj_set_style_radius(g_laser_dlg_fill, USER_BTN_RADIUS + 4, 0);
    lv_obj_set_style_bg_color(g_laser_dlg_fill, user_color_accent, 0);
    lv_obj_set_style_bg_opa(g_laser_dlg_fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_laser_dlg_fill, LV_OBJ_FLAG_SCROLLABLE);

    make_round_btn(row, LV_SYMBOL_PLUS, USER_BTN_PRIMARY_H, on_laser_step,
                   (void *)(intptr_t)1);

    laser_dlg_sync();
}

/* ============================ 开关回调 ============================ */

void on_light_toggle(lv_event_t *)
{
    const bool next = !g_light_sw.on;
    apply_toggle(&g_light_sw, next, kLightOn, false);
    /* 稿上 LIGHT 卡没有状态字（见 build_toggle_card），state 因此是空的——开关本身的
     * 位置与配色就是状态。这里判空而不是去把状态字加回来。 */
    if(g_light_sw.state) {
        lv_label_set_text(g_light_sw.state, next ? "On" : "Off");
    }
    ui::DebugMsgProxy::instance().send_device(DebugDeviceTarget::Light, next);
}

/**
 * Powder Clean：一次性作业，不是持续开关。
 *
 * 点下去 → 发请求 → 受理后置 ON 并**置灰不可取消**（清粉中途打断会留下舱内 PM 偏高）
 * → MCU 回结束（`APP_Clean_Test_Result_Ntf`）才回 OFF 解灰。
 * 走 `record_video=false` 那一档（`UI_CLEAN_OWNER_CONTROL`）：不开 Media 录像会话、
 * 不写 `/data/media/video/config.json`、不进打印历史。
 */
void on_clean_toggle(lv_event_t *)
{
    if(g_clean_busy) {
        return;   /* 置灰期间已摘掉 CLICKABLE，这里只是双保险 */
    }
    g_clean_busy = true;
    apply_toggle(&g_clean_sw, true, user_color_accent, true);
    if(g_clean_sw.state) {
        lv_label_set_text(g_clean_sw.state, "Cleaning");
    }
    ui_clean_state_request(UI_CLEAN_OWNER_CONTROL, nullptr);
    dev_clean_timeout_arm();
}

/** 清粉回执（经 ui_clean_state 扇出，已在 UI 线程）。只处理本页发起的那一路。 */
void on_clean_event(ui_clean_event_t ev, ui_clean_owner_t owner)
{
    if(owner != UI_CLEAN_OWNER_CONTROL || !g_clean_sw.track) {
        return;
    }
    switch(ev) {
        case UI_CLEAN_EVT_ACCEPTED:
            break;   /* 点击时已置 ON + 置灰 */
        case UI_CLEAN_EVT_REJECTED:
            dev_clean_timeout_disarm();
            clean_reset_idle();
            ui_toast_show("Powder cleaning is busy", user_color_status_err, 1800);
            break;
        case UI_CLEAN_EVT_DONE:
            dev_clean_timeout_disarm();
            clean_reset_idle();
            ui_toast_show("Powder cleaning finished", user_color_status_ok, 1800);
            break;
        default:
            break;
    }
}

/** 稿未提供弹窗的那几张卡：箭头照画、点了给个明确反馈，不装作没这回事。 */
void on_card_todo(lv_event_t *)
{
    ui_toast_show("Not available yet", user_color_text_dim, 1500);
}

void on_more_click(lv_event_t *) { show_view(true); }

void on_door_unlocked(void * /*ud*/)
{
    /* 复用首页/监控那条独立开门流程（PM2.5 检测 → 必要时清粉 → 开门 → 关门确认）。
     * 弹窗建在 lv_layer_top()，与当前页面无关，所以不必先切回首页。 */
    control_open_door_request();
    if(g_door_slide) {
        ui_slide_unlock_reset(g_door_slide);
    }
}

/* ============================ 主面板 ============================ */

/** 一张「指标 + 条 + 胶囊」卡。`chevron` 为 true 时右侧出箭头并整卡可点。 */
lv_obj_t *build_bar_card(lv_obj_t *col, MetricCard *out,
                         const lv_image_dsc_t *icon,
                         const char *caption, const char *unit,
                         lv_event_cb_t on_click)
{
    lv_obj_t *card = make_card(col);
    lv_obj_set_height(card, card_h_metric());
    build_metric_head(card, out, icon, caption, unit, on_click != nullptr);
    build_metric_bar(card, out);
    build_metric_pill(card, out);
    if(on_click) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_click, LV_EVENT_CLICKED, nullptr);
    }
    return card;
}

/**
 * 「[图标] caption（+ 可选状态字）」+ 右侧开关的卡。
 *
 * **稿上 Powder Clean 与 LIGHT 不是同一个模板**，别当成一个：
 *   - Powder Clean（`#5815:18006`）有 28 号状态字「关闭」，卡高 100；
 *   - LIGHT（`#5782:13071`）**只有 caption**（子节点 `#5782:13079` 里只有一个
 *     `[TEXT] "LIGHT"`），卡高 64。
 * 第一版把 LIGHT 也塞了状态字，多出一行 32px，是两列底边对不齐的一个原因。
 *
 * @param state0 状态字初值；传 nullptr 即 LIGHT 那种「只有 caption」的窄卡。
 */
lv_obj_t *build_toggle_card(lv_obj_t *col, ToggleCard *out,
                            const lv_image_dsc_t *icon,
                            const char *caption, const char *state0,
                            lv_event_cb_t cb)
{
    lv_obj_t *card = make_card(col);
    lv_obj_set_height(card, state0 ? card_h_state() : card_h_slim());
    lv_obj_t *row  = make_row(card, USER_PAD_SM / 2);
    lv_obj_set_width(row, LV_PCT(100));

    make_icon(row, icon);

    lv_obj_t *txt = make_col(row, USER_PAD_SM / 3);
    lv_obj_set_flex_grow(txt, 1);
    make_label(txt, caption, user_font_caption_sm(), kCapColor, LV_OPA_COVER);
    if(state0) {
        out->state = make_label(txt, state0, user_font_value(), user_color_text,
                                LV_OPA_COVER);
    }

    build_toggle(row, out, cb);
    return card;
}

/** 「[图标] caption / 值」+ 箭头的卡（Laser Power）。 */
lv_obj_t *build_value_card(lv_obj_t *col, lv_obj_t **out_value,
                           const lv_image_dsc_t *icon,
                           const char *caption, const char *v0,
                           lv_event_cb_t on_click)
{
    lv_obj_t *card = make_card(col);
    lv_obj_set_height(card, card_h_state());
    lv_obj_t *row  = make_row(card, USER_PAD_SM / 2);
    lv_obj_set_width(row, LV_PCT(100));

    make_icon(row, icon);

    lv_obj_t *txt = make_col(row, USER_PAD_SM / 3);
    lv_obj_set_flex_grow(txt, 1);
    make_label(txt, caption, user_font_caption_sm(), kCapColor, LV_OPA_COVER);
    *out_value = make_label(txt, v0, user_font_value(), user_color_text,
                            LV_OPA_COVER);

    make_chevron(row);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_click, LV_EVENT_CLICKED, nullptr);
    return card;
}

/**
 * DEVICE TEMP + O2 组合卡（稿 272×200，中间一条 0.5px 白 10% 分隔线）。
 * 两半各是一个只有卡头、没有条与胶囊的指标块。
 */
void build_combo_card(lv_obj_t *col)
{
    lv_obj_t *card = make_card(col);
    lv_obj_set_height(card, card_h_combo());
    lv_obj_set_style_pad_row(card, USER_PAD_SM, 0);
    /* 两半 + 中间那条线在 200 高里上下均分 */
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *top = make_box(card);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    build_metric_head(top, &g_temp, pick_metric(ic_temp_set), "DEVICE TEMP", "C",
                      true);
    lv_obj_add_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top, on_card_todo, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, user_color_text, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_10, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bot = make_box(card);
    lv_obj_set_width(bot, LV_PCT(100));
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_COLUMN);
    build_metric_head(bot, &g_o2, pick_metric(ic_o2_set), "O2 CONCENTRATION", "%",
                      true);
    lv_obj_add_flag(bot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bot, on_card_todo, LV_EVENT_CLICKED, nullptr);
}

/** 「More Controls」入口卡（稿 275×64、黑 20% 底、右箭头）。 */
void build_more_entry(lv_obj_t *col)
{
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), card_h_slim());   /* 稿 64，与 LIGHT 同高 */
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, USER_BTN_RADIUS, 0);
    lv_obj_set_style_pad_all(card, card_pad(), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, on_more_click, LV_EVENT_CLICKED, nullptr);

    make_label(card, "More Controls", user_font_text_sm(), user_color_text,
               LV_OPA_80);
    make_chevron(card);
}

void build_main_view(lv_obj_t *parent)
{
    g_main_view = make_box(parent);
    lv_obj_set_size(g_main_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_left(g_main_view, body_pad_l(), 0);
    lv_obj_set_style_pad_top(g_main_view, body_pad_t(), 0);
    lv_obj_set_flex_flow(g_main_view, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_main_view, col_gap(), 0);

    /* ---- 左列：舱内/室内 PM2.5、温度+氧浓度、照明 ----
     * 两列都**钉死 col_h()（稿 604）+ SPACE_BETWEEN**，而不是顶对齐 + 内容累加：
     * 稿上两列精确地从 y=125 铺到 y=729、各 604 高，靠的是每张卡自己的确定高度。卡高已
     * 逐个钉死（见 card_h_* 那组），剩下的几像素出入落进行距（7→7.x），于是两列在顶和底
     * 都严丝合缝、底边正好落回稿的 y=729。详见 col_h() 的注释。 */
    lv_obj_t *left = make_col(g_main_view, card_gap());
    lv_obj_set_width(left, col_w());
    lv_obj_set_height(left, col_h());
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    build_bar_card(left, &g_pm_in, pick_metric(ic_cabin_set), "INTERIOR PM2.5",
                   "ug/m3", open_pm_dlg);
    /* 稿上 EXTERIOR 那枚箭头是 opacity 0（节点 5782:12995），即不可点，故不给 on_click */
    build_bar_card(left, &g_pm_out, pick_metric(ic_home_set), "EXTERIOR PM2.5",
                   "ug/m3", nullptr);
    build_combo_card(left);
    /* LIGHT 传 state0=nullptr：稿上这张卡只有 caption、没有状态字，见 build_toggle_card */
    build_toggle_card(left, &g_light_sw, pick_metric(ic_bulb_set), "LIGHT", nullptr,
                      on_light_toggle);

    /* ---- 右列：滤芯、耗材、清粉、激光功率、更多控制 ----
     * 这三枚图标是手维护的单档资源（`ic_layers` / `ic_cube` / `ic_purify`），
     * 不经 pick_metric()。稿上滤芯/耗材/清粉那三枚矢量未导出，按语义就近取用。 */
    lv_obj_t *right = make_col(g_main_view, card_gap());
    lv_obj_set_width(right, col_w());
    lv_obj_set_height(right, col_h());
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    build_bar_card(right, &g_filter, &ic_layers, "Filter", "%", on_card_todo);
    build_bar_card(right, &g_material, &ic_cube, "Material", "%", on_card_todo);
    build_toggle_card(right, &g_clean_sw, &ic_purify, "Powder Clean", "Off",
                      on_clean_toggle);
    build_value_card(right, &g_laser_value, pick_metric(ic_bolt_set),
                     "Laser Power", "--", open_laser_dlg);
    build_more_entry(right);

    /* ---- 右下开门滑块（稿 5782:13121，400 宽、红渐变圆头）---- */
    ui_slide_unlock_cfg_t cfg = {};
    cfg.hint_text     = "SLIDE to OPEN";
    cfg.on_unlocked   = on_door_unlocked;
    cfg.track_w       = USER_DOOR_SLIDE_W;
    cfg.track_h       = USER_DOOR_SLIDE_H;
    cfg.knob_sz       = USER_DOOR_SLIDE_KNOB;
    cfg.custom_accent = true;
    cfg.accent_color  = user_color_status_err;
    g_door_slide = ui_slide_unlock_create(g_main_view, &cfg);
    if(lv_obj_t *sroot = ui_slide_unlock_get_root(g_door_slide)) {
        lv_obj_add_flag(sroot, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(sroot, LV_ALIGN_BOTTOM_RIGHT, -USER_PAD_LG, -USER_PAD_LG);
    }
}

/* ============================================================================
 *  更多控制（稿 5814:16936 运动控制 / 5815:17390 设备控制）
 *
 * 这两个 Tab 的功能与调试页的 Move / Other 两页是同一套后端契约（UI_Debug_* 那一段），
 * 只是换了用户级视觉与英文术语。**术语一律沿用调试页现有的串**，别照稿另起一套：
 * 同一根轴在两个界面叫两个名字是排查事故的开始。稿上 S 轴写的是「打印平面高度（S轴）」，
 * 那是把 Z 卡标题复制过去忘改了——S 轴实际是铺粉刮刀，取调试页的 "Powder Scraper (S)"。
 * ========================================================================== */

/** 传感器条六列：与 DebugSensorPayload 的前六个 float 一一对应、同序。 */
struct SensorCol {
    const char *caption;
    const char *unit;
};
const SensorCol kSensorCols[6] = {
    { "O2",          "%"     },
    { "Pressure",    "kPa"   },
    { "PM2.5",       "ug/m3" },
    { "Humidity",    "%"     },
    { "Cabin Temp",  "C"     },
    { "Base Temp",   "C"     },
};
lv_obj_t *g_sensor_val[6] = { nullptr };

/** 三轴配置。字段含义同调试页 kAxes，此处只留本页用得到的。 */
struct AxisCfg {
    DebugAxis   axis;
    const char *title;
    const char *pos_caption;
    const char *unit;       /**< 位置/步距单位 */
    bool        has_zero;   /**< 仅 Z 轴有工作零点 */
    bool        rotational; /**< D 轴用旋转箭头、且没有 home 键 */
};
const AxisCfg kAxes[3] = {
    { DebugAxis::Z, "Print Plane Height (Z)", "Z Position:", "mm",           true,  false },
    { DebugAxis::S, "Powder Scraper (S)",     "S Position:", "mm",           false, false },
    { DebugAxis::D, "Powder Bucket Rod (D)",  "D Position:", "\xC2\xB0",     false, true  },
};

lv_obj_t *g_axis_pos[3]  = { nullptr };   /**< 位置读数 */
lv_obj_t *g_axis_step[3] = { nullptr };   /**< 单次步距输入 */
lv_obj_t *g_z_zero_lbl   = nullptr;       /**< Z 工作零点读数 */
uint8_t   g_axis_mode[3] = { 0, 0, 0 };   /**< 0=Click move 1=Long-press */
lv_obj_t *g_axis_tab_mark[3][2] = {};     /**< 子 Tab 下划线，用于切换高亮 */
lv_obj_t *g_axis_tab_lbl[3][2]  = {};

lv_obj_t *g_more_tab_pill[2] = { nullptr };  /**< Motion / Device 胶囊 */
lv_obj_t *g_more_tab_lbl[2]  = { nullptr };
lv_obj_t *g_more_pane[2]     = { nullptr };  /**< 两个 Tab 的内容面 */

/* 设备控制那几处状态字 */
lv_obj_t *g_dev_inner_door = nullptr;
lv_obj_t *g_dev_sieve      = nullptr;
lv_obj_t *g_dev_fan_state  = nullptr;
lv_obj_t *g_dev_cabin_door = nullptr;
lv_obj_t *g_dev_gas_wash   = nullptr;
lv_obj_t *g_dev_base_heat  = nullptr;
lv_obj_t *g_fan_duty_ta    = nullptr;
lv_obj_t *g_layer_ta       = nullptr;
lv_obj_t *g_balance_ta     = nullptr;
bool g_fan_on = false, g_gas_on = false, g_heat_on = false;

/* ---------------------------------------------------------------------------
 *  更多控制里的通用小件
 * ------------------------------------------------------------------------- */

/** 数字输入框（稿 80×48，白 5% 底 + 白 10% 描边 + 圆角 8）。点开数字键盘。 */
lv_obj_t *make_num_input(lv_obj_t *parent, float v0, int decimals)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, num_input_w(), num_input_h());
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789.-");
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, (double)v0);
    lv_textarea_set_text(ta, buf);
    lv_obj_set_style_bg_color(ta, user_color_text, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_10 / 2, 0);
    lv_obj_set_style_border_color(ta, user_color_text, 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_10, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, USER_BTN_RADIUS, 0);
    lv_obj_set_style_text_color(ta, user_color_text, 0);
    lv_obj_set_style_text_font(ta, user_font_text_sm(), 0);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(ta, USER_PAD_SM / 2, 0);
    ui_keyboard_attach(ta, LV_KEYBOARD_MODE_NUMBER);
    return ta;
}

float read_ta(lv_obj_t *ta)
{
    if(!ta) {
        return 0.0f;
    }
    const char *s = lv_textarea_get_text(ta);
    return s ? (float)std::atof(s) : 0.0f;
}

/** 「caption / 状态字」两行块（设备控制里到处是这个）。 */
lv_obj_t *make_state_block(lv_obj_t *parent, const char *caption,
                           const char *state0, lv_obj_t **out_state)
{
    lv_obj_t *col = make_col(parent, USER_PAD_SM / 2);
    make_label(col, caption, user_font_text_sm(), user_color_text, LV_OPA_50);
    *out_state = make_label(col, state0, user_font_title(), kTabBlue, LV_OPA_COVER);
    return col;
}

/**
 * 稿上那种描边胶囊按钮（Set Work Zero：128×32、主色描边 + 主色字 + 10% 底）。
 *
 * @param h 视觉高度；<=0 用稿值（1280 档 32）。风扇的 Send 传输入框同高的值——同一行里
 *          两个控件等高看着才像有意为之。
 *
 * 稿值 32 低于 `UI_USER_MIN_TOUCH`(48)，实机上点不灵，所以**不足 48 时自动外扩命中区补足
 * 差额**（视觉尺寸不变，故对稿的还原度不受影响）。同 `ui_user_make_overlay_round_btn()`
 * 的处理，理由见 UI.md「触摸命中区」：视觉尺寸归设计稿，命中区归可用性，两者不必绑死。
 */
lv_obj_t *make_outline_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                           void *ud, int32_t h = 0)
{
    const int32_t btn_h = (h > 0) ? h : (is_1280() ? 32 : 28);

    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, btn_h);
    if(btn_h < UI_USER_MIN_TOUCH) {
        lv_obj_set_ext_click_area(b, (UI_USER_MIN_TOUCH - btn_h) / 2);
    }
    lv_obj_set_style_pad_hor(b, USER_PAD, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, kTabBlue, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, kTabBlue, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = make_label(b, text, user_font_text_sm(), kTabBlue, LV_OPA_COVER);
    lv_obj_center(l);
    return b;
}

/** 大卡（稿 16 圆角 + 白 10% 描边），更多控制那几块都是这个壳。底色见 kCardBg。 */
lv_obj_t *make_pane_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_color(c, kCardBg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, user_color_text, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_10, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(c, USER_PAD, 0);
    lv_obj_set_style_pad_row(c, USER_PAD_SM, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ---------------------------------------------------------------------------
 *  运动控制
 * ------------------------------------------------------------------------- */

void apply_axis_subtab(int idx)
{
    for(int i = 0; i < 2; i++) {
        const bool sel = (g_axis_mode[idx] == (uint8_t)i);
        if(g_axis_tab_lbl[idx][i]) {
            lv_obj_set_style_text_color(g_axis_tab_lbl[idx][i],
                                        sel ? kTabBlue : kCapColor, 0);
        }
        if(g_axis_tab_mark[idx][i]) {
            lv_obj_set_style_bg_opa(g_axis_tab_mark[idx][i],
                                    sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

void on_axis_subtab(lv_event_t *e)
{
    const uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    const int idx  = (int)(v >> 1);
    const int mode = (int)(v & 1u);
    g_axis_mode[idx] = (uint8_t)mode;
    apply_axis_subtab(idx);
}

/**
 * 点动：一个回调按事件码 + 当前子 Tab 分流。
 *
 * **不能只挂 `LV_EVENT_CLICKED`**：LVGL 的 CLICKED 是松手时才发的，长按模式下会退化成
 * 「按住不动、一松手才走一下又立刻停」，跟这个模式的语义正好相反。所以点击模式听
 * CLICKED，长按模式听 PRESSED 起步、RELEASED 补停帧。
 *
 * `PRESS_LOST` 必须和 RELEASED 一起收：手指按住时滑出钮外只发 PRESS_LOST，漏掉它轴会
 * 一直走下去——这是会撞机的那类漏洞，不是观感问题。
 */
void on_axis_jog(lv_event_t *e)
{
    const uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    const int idx = (int)(v >> 1);
    const int dir = (v & 1u) ? 1 : -1;
    const lv_event_code_t code = lv_event_get_code(e);
    const bool hold = (g_axis_mode[idx] == 1);
    auto &proxy = ui::DebugMsgProxy::instance();

    switch(code) {
        case LV_EVENT_PRESSED:
            if(hold) {
                proxy.send_axis_jog(kAxes[idx].axis, DebugJogMode::Hold, dir,
                                    read_ta(g_axis_step[idx]));
            }
            break;
        case LV_EVENT_CLICKED:
            if(!hold) {
                proxy.send_axis_jog(kAxes[idx].axis, DebugJogMode::Click, dir,
                                    read_ta(g_axis_step[idx]));
            }
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            if(hold) {
                proxy.send_axis_hold_stop(kAxes[idx].axis);
            }
            break;
        default:
            break;
    }
}

/** 把上面那四个事件一次挂全，漏一个就是漏一条停止帧。 */
void attach_jog(lv_obj_t *btn, void *ud)
{
    lv_obj_add_event_cb(btn, on_axis_jog, LV_EVENT_PRESSED, ud);
    lv_obj_add_event_cb(btn, on_axis_jog, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(btn, on_axis_jog, LV_EVENT_RELEASED, ud);
    lv_obj_add_event_cb(btn, on_axis_jog, LV_EVENT_PRESS_LOST, ud);
}

void on_axis_home(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui::DebugMsgProxy::instance().send_axis_home(kAxes[idx].axis);
}

void on_axis_set_zero(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui::DebugMsgProxy::instance().send_axis_set_zero(kAxes[idx].axis);
    ui_toast_show("Work zero set", user_color_status_ok, 1500);
}

/** 一张轴卡。 */
void build_axis_card(lv_obj_t *row, int idx)
{
    const AxisCfg &cfg = kAxes[idx];

    lv_obj_t *card = make_pane_card(row);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    /* 底部那块黑 20% 的步进区要贴到卡片下沿，不裁圆角会从圆角处露出直角。
     * 这里是静态页面、不滚动，裁剪掩膜的开销不像列表里那样要紧。 */
    lv_obj_set_style_clip_corner(card, true, 0);

    /* ---- 上半：标题 + 位置（Z 另有工作零点行）---- */
    lv_obj_t *top = make_col(card, USER_PAD_SM);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_style_pad_hor(top, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(top, USER_PAD_SM * 3 / 4, 0);

    make_label(top, cfg.title, user_font_text_sm(), user_color_text, LV_OPA_COVER);

    make_label(top, cfg.pos_caption, user_font_text_sm(), kCapColor, LV_OPA_COVER);
    lv_obj_t *prow = make_row(top, USER_PAD_SM / 2);
    g_axis_pos[idx] = make_label(prow, "0.000", user_font_text_sm(),
                                 user_color_text, LV_OPA_COVER);
    make_label(prow, cfg.unit, user_font_text_sm(), kCapColor, LV_OPA_COVER);

    if(cfg.has_zero) {
        lv_obj_t *zrow = make_row(top, USER_PAD_SM);
        lv_obj_set_width(zrow, LV_PCT(100));
        lv_obj_set_flex_align(zrow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *zcol = make_col(zrow, USER_PAD_SM / 2);
        make_label(zcol, "Work Zero:", user_font_text_sm(), kCapColor,
                   LV_OPA_COVER);
        lv_obj_t *zvrow = make_row(zcol, USER_PAD_SM / 2);
        g_z_zero_lbl = make_label(zvrow, "0.000", user_font_text_sm(),
                                  user_color_text, LV_OPA_COVER);
        make_label(zvrow, cfg.unit, user_font_text_sm(), kCapColor, LV_OPA_COVER);
        make_outline_btn(zrow, "Set Work Zero", on_axis_set_zero,
                         (void *)(intptr_t)idx);
    }

    lv_obj_t *spacer = make_box(card);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_flex_grow(spacer, 1);

    /* ---- 子 Tab：Click move / Long-press（选中项蓝字 + 蓝下划线）---- */
    lv_obj_t *tabs = make_row(card, 0);
    lv_obj_set_width(tabs, LV_PCT(100));
    static const char *const kSub[2] = { "Click move", "Long-press" };
    for(int i = 0; i < 2; i++) {
        lv_obj_t *cell = make_col(tabs, USER_PAD_SM / 4);
        lv_obj_set_flex_grow(cell, 1);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(cell, USER_PAD_SM);
        lv_obj_add_event_cb(cell, on_axis_subtab, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)((idx << 1) | i));
        g_axis_tab_lbl[idx][i] = make_label(cell, kSub[i], user_font_text_sm(),
                                            kCapColor, LV_OPA_COVER);
        lv_obj_t *mark = lv_obj_create(cell);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(mark, kTabBlue, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        g_axis_tab_mark[idx][i] = mark;
    }
    /* 整行底下那条通栏细线（稿 5814:17107） */
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, user_color_text, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_10, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 下半：步距输入 + 步进钮（黑 20% 底）---- */
    lv_obj_t *bot = make_col(card, USER_PAD);
    lv_obj_set_width(bot, LV_PCT(100));
    lv_obj_set_style_bg_color(bot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(bot, USER_PAD_SM, 0);
    lv_obj_set_style_pad_ver(bot, USER_PAD, 0);

    lv_obj_t *srow = make_row(bot, USER_PAD_SM);
    lv_obj_set_width(srow, LV_PCT(100));
    lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_label(srow, "Step Distance", user_font_text_sm(), kCapColor,
               LV_OPA_COVER);
    lv_obj_t *irow = make_row(srow, USER_PAD_SM / 2);
    g_axis_step[idx] = make_num_input(irow, 0.01f, 2);
    make_label(irow, cfg.unit, user_font_text_sm(), kCapColor, LV_OPA_COVER);

    lv_obj_t *brow = make_row(bot, USER_PAD_LG);
    lv_obj_set_width(brow, LV_PCT(100));
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    const int32_t bsz = is_1280() ? 48 : 40;
    void *ud_neg = (void *)(uintptr_t)((idx << 1) | 0u);
    void *ud_pos = (void *)(uintptr_t)((idx << 1) | 1u);
    /* D 轴是旋转轴，稿上用一对回转箭头且没有 home 键。
     * 点动的事件挂在 attach_jog() 里，make_round_btn 这里不再给 CLICKED 回调。 */
    attach_jog(make_round_btn(brow, cfg.rotational ? LV_SYMBOL_LEFT
                                                   : LV_SYMBOL_MINUS,
                              bsz, nullptr, nullptr), ud_neg);
    attach_jog(make_round_btn(brow, cfg.rotational ? LV_SYMBOL_RIGHT
                                                   : LV_SYMBOL_PLUS,
                              bsz, nullptr, nullptr), ud_pos);
    if(!cfg.rotational) {
        make_round_btn(brow, LV_SYMBOL_LOOP, bsz, on_axis_home,
                       (void *)(intptr_t)idx);
    }

    apply_axis_subtab(idx);
}

void build_motion_pane(lv_obj_t *parent)
{
    lv_obj_t *pane = make_box(parent);
    lv_obj_set_size(pane, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pane, USER_PAD_SM + 4, 0);   /* 稿 20 */
    g_more_pane[0] = pane;

    for(int i = 0; i < 3; i++) {
        build_axis_card(pane, i);
    }
}

/* ---------------------------------------------------------------------------
 *  设备控制
 * ------------------------------------------------------------------------- */

void on_print_param_send(lv_event_t *)
{
    const uint16_t layer = (uint16_t)read_ta(g_layer_ta);
    const uint16_t bal   = (uint16_t)read_ta(g_balance_ta);
    ui::DebugMsgProxy::instance().send_print_layer_param(layer, bal);
    ui_toast_show("Print params sent", user_color_status_ok, 1500);
}

void on_fan_toggle(lv_event_t *)
{
    g_fan_on = !g_fan_on;
    ui::DebugMsgProxy::instance().send_fan(g_fan_on);
    if(g_dev_fan_state) {
        lv_label_set_text(g_dev_fan_state, g_fan_on ? "Running" : "Off");
        lv_obj_set_style_text_color(g_dev_fan_state,
            g_fan_on ? kTabBlue : user_color_text_dim, 0);
    }
    /* 开的那一下顺带把输入框里的占空比落下去，省一次点 Send。运行中改数值请按 Send，
     * 见 on_fan_duty_send() 的说明。 */
    if(g_fan_on) {
        ui::DebugMsgProxy::instance().send_fan_duty(read_ta(g_fan_duty_ta));
    }
}

/**
 * 单独下发风扇占空比（`UI_Debug_Fan_Duty_Ntf`，契约里注明「仅设占空比、不开关」）。
 *
 * **没有这个键，风扇转起来之后改 Duty 输入框是完全无效的**——只有开关的那一下会带上
 * 占空比，想改就得先关再开。调试页正因如此配了独立 Send（`on_fan_send_click`），这里
 * 与它对齐。**稿上风扇卡没有这个键**（节点 5815:17974 只有占空比行和开关行），是刻意
 * 偏离：稿在这一处不完整。
 */
void on_fan_duty_send(lv_event_t *)
{
    ui::DebugMsgProxy::instance().send_fan_duty(read_ta(g_fan_duty_ta));
    ui_toast_show("Fan duty sent", user_color_status_ok, 1500);
}

/** 内门 / 筛粉电机的三钮组：[反向] [停止/运行] [正向]。 */
void on_motor_action(lv_event_t *e)
{
    const uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    const bool sieve = (v >> 2) != 0;
    const auto act = (DebugMotorAction)(v & 0x3u);
    auto &proxy = ui::DebugMsgProxy::instance();
    if(sieve) {
        proxy.send_sieve(act);
    } else {
        proxy.send_inner_door(act);
    }
    lv_obj_t *lbl = sieve ? g_dev_sieve : g_dev_inner_door;
    if(lbl) {
        const char *txt = (act == DebugMotorAction::Stop) ? (sieve ? "Stopped" : "Closed")
                        : (act == DebugMotorAction::Forward) ? (sieve ? "Running" : "Open")
                        : (sieve ? "Reversing" : "Closing");
        lv_label_set_text(lbl, txt);
    }
}

/** 洗气 / 底板加热 / 设备门那三枚圆钮。 */
void on_switch_toggle(lv_event_t *e)
{
    const auto target = (DebugSwitchTarget)(intptr_t)lv_event_get_user_data(e);
    auto &proxy = ui::DebugMsgProxy::instance();
    if(target == DebugSwitchTarget::GasWash) {
        g_gas_on = !g_gas_on;
        proxy.send_switch(target, g_gas_on);
        if(g_dev_gas_wash) {
            lv_label_set_text(g_dev_gas_wash, g_gas_on ? "On" : "Off");
            lv_obj_set_style_text_color(g_dev_gas_wash,
                g_gas_on ? kTabBlue : user_color_text_dim, 0);
        }
    } else if(target == DebugSwitchTarget::BaseHeat) {
        g_heat_on = !g_heat_on;
        proxy.send_switch(target, g_heat_on);
        if(g_dev_base_heat) {
            lv_label_set_text(g_dev_base_heat, g_heat_on ? "On" : "Off");
            lv_obj_set_style_text_color(g_dev_base_heat,
                g_heat_on ? kTabBlue : user_color_text_dim, 0);
        }
    }
}

/** 设备门（外门）：走 DebugDeviceTarget::Door，状态由 GLAVO_Debug_Device_Ntf 回读。 */
void on_cabin_door(lv_event_t *)
{
    static bool s_open = false;
    s_open = !s_open;
    ui::DebugMsgProxy::instance().send_device(DebugDeviceTarget::Door, s_open);
}

void build_device_pane(lv_obj_t *parent)
{
    lv_obj_t *pane = make_box(parent);
    lv_obj_set_size(pane, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pane, USER_PAD_SM + 4, 0);
    g_more_pane[1] = pane;

    /* ---- 左列（稿 300 宽）：打印参数 + 风扇 ---- */
    lv_obj_t *left = make_col(pane, USER_PAD_SM + 4);
    lv_obj_set_width(left, is_1280() ? 300 : 240);
    lv_obj_set_height(left, LV_PCT(100));

    lv_obj_t *pp = make_pane_card(left);
    lv_obj_set_width(pp, LV_PCT(100));
    lv_obj_set_height(pp, LV_SIZE_CONTENT);
    make_label(pp, "Print Params", user_font_text_sm(), user_color_text,
               LV_OPA_COVER);
    lv_obj_t *pprow = make_row(pp, USER_PAD_SM);
    lv_obj_set_width(pprow, LV_PCT(100));
    lv_obj_t *c1 = make_col(pprow, USER_PAD_SM / 2);
    make_label(c1, "Layer (um)", user_font_caption(), user_color_text, LV_OPA_50);
    g_layer_ta = make_num_input(c1, 50.0f, 0);
    lv_obj_t *c2 = make_col(pprow, USER_PAD_SM / 2);
    make_label(c2, "Balance", user_font_caption(), user_color_text, LV_OPA_50);
    g_balance_ta = make_num_input(c2, 1000.0f, 0);

    lv_obj_t *send = lv_obj_create(pp);
    lv_obj_remove_style_all(send);
    lv_obj_set_size(send, LV_PCT(100), is_1280() ? 42 : 36);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(send, user_color_accent, 0);
    lv_obj_set_style_bg_opa(send, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(send, user_color_accent_dim, LV_STATE_PRESSED);
    lv_obj_add_flag(send, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(send, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(send, on_print_param_send, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = make_label(send, "Send", user_font_text_sm(), user_color_text,
                              LV_OPA_COVER);
    lv_obj_center(sl);

    /**
     * Fan 卡**吃掉左列的剩余高度**（`flex_grow`），一举解决两件事：
     *   - 底边与右列的 Other Switches 齐平。稿上两列本来就是齐的（右列 318+217+20+224
     *     收在 779，左列两张卡加起来也正好到 779），所以对齐是贴稿而不是偏离；
     *   - 左右两列原先高度策略不对称——右列的 mrow/other 都 `flex_grow` 恒定撑满，左列
     *     两张卡却是 `LV_SIZE_CONTENT`、够不够全看字体推出来的高度凑不凑巧，实机上就是
     *     差了一截。
     * 卡内三行改 SPACE_BETWEEN，多出来的高度分给行距，顺带把 Send 那行撑开一些。
     */
    lv_obj_t *fan = make_pane_card(left);
    lv_obj_set_width(fan, LV_PCT(100));
    lv_obj_set_flex_grow(fan, 1);
    lv_obj_set_flex_align(fan, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    make_label(fan, "Fan", user_font_text_sm(), user_color_text, LV_OPA_COVER);
    lv_obj_t *frow = make_row(fan, USER_PAD_SM);
    lv_obj_set_width(frow, LV_PCT(100));
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_label(frow, "Duty", user_font_caption(), user_color_text, LV_OPA_50);
    /* Send 跟在数值组里、而不是像调试页那样与电源钮同排：
     *   语义上它发的是这一行的数值，挨着值读起来才对；
     *   横向也宽裕得多——这张卡 1280 档只有 300 宽，扣掉内距剩 252，开关那行已经被
     *   状态块 + 80 的电源钮占去大半，1024 档更紧（196 扣掉 64 的钮只剩百来像素）。 */
    lv_obj_t *drow = make_row(frow, USER_PAD_SM / 2);
    g_fan_duty_ta = make_num_input(drow, 0.01f, 2);
    make_label(drow, "%", user_font_text_sm(), kCapColor, LV_OPA_COVER);
    /* 高度取和左边那个输入框一样（1280 档 48 / 1024 档 40）：稿值 32 在实机上点不灵，
     * 而同一行里两个控件等高看着才像有意为之。不足 48 的档位由 make_outline_btn 自动
     * 外扩命中区补足。 */
    make_outline_btn(drow, "Send", on_fan_duty_send, nullptr, num_input_h());

    lv_obj_t *fsrow = make_row(fan, USER_PAD_SM);
    lv_obj_set_width(fsrow, LV_PCT(100));
    lv_obj_set_flex_align(fsrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_state_block(fsrow, "Fan", "Off", &g_dev_fan_state);
    lv_obj_set_style_text_color(g_dev_fan_state, user_color_text_dim, 0);
    make_round_btn(fsrow, LV_SYMBOL_POWER, is_1280() ? 80 : 64, on_fan_toggle,
                   nullptr);

    /* ---- 右列（稿 752 宽）：内门 + 筛粉电机 / 其他开关 ---- */
    lv_obj_t *right = make_col(pane, USER_PAD_SM + 4);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));

    lv_obj_t *mrow = make_row(right, USER_PAD_SM + 4);
    lv_obj_set_width(mrow, LV_PCT(100));
    lv_obj_set_flex_grow(mrow, 1);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    struct MotorSpec { const char *cap; const char *state0; bool sieve; const char *mid; };
    const MotorSpec kMotors[2] = {
        { "Inner Door",  "Closed",  false, LV_SYMBOL_POWER },
        { "Sieve Motor", "Stopped", true,  LV_SYMBOL_PLAY  },
    };
    for(int i = 0; i < 2; i++) {
        lv_obj_t *card = make_pane_card(mrow);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, LV_PCT(100));
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        make_state_block(card, kMotors[i].cap, kMotors[i].state0,
                         kMotors[i].sieve ? &g_dev_sieve : &g_dev_inner_door);

        lv_obj_t *brow = make_row(card, USER_PAD);
        lv_obj_set_width(brow, LV_PCT(100));
        lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        const int32_t bsz = is_1280() ? 80 : 64;
        const uintptr_t base = kMotors[i].sieve ? 4u : 0u;
        make_round_btn(brow, LV_SYMBOL_LEFT, bsz, on_motor_action,
                       (void *)(uintptr_t)(base | (uintptr_t)DebugMotorAction::Reverse));
        make_round_btn(brow, kMotors[i].mid, bsz, on_motor_action,
                       (void *)(uintptr_t)(base | (uintptr_t)DebugMotorAction::Stop));
        make_round_btn(brow, LV_SYMBOL_RIGHT, bsz, on_motor_action,
                       (void *)(uintptr_t)(base | (uintptr_t)DebugMotorAction::Forward));
    }

    lv_obj_t *other = make_pane_card(right);
    lv_obj_set_width(other, LV_PCT(100));
    lv_obj_set_flex_grow(other, 1);
    make_label(other, "Other Switches", user_font_text_sm(), user_color_text,
               LV_OPA_COVER);

    lv_obj_t *orow = make_row(other, USER_PAD_LG);
    lv_obj_set_width(orow, LV_PCT(100));
    lv_obj_set_flex_grow(orow, 1);
    lv_obj_set_flex_align(orow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    const int32_t osz = is_1280() ? 80 : 64;

    lv_obj_t *g1 = make_row(orow, USER_PAD_SM);
    make_state_block(g1, "Cabin Door", "Closed", &g_dev_cabin_door);
    make_round_btn(g1, LV_SYMBOL_POWER, osz, on_cabin_door, nullptr);

    ui_user_make_divider_vline(orow, is_1280() ? 24 : 20);

    lv_obj_t *g2 = make_row(orow, USER_PAD_SM);
    make_state_block(g2, "Gas Wash", "Off", &g_dev_gas_wash);
    lv_obj_set_style_text_color(g_dev_gas_wash, user_color_text_dim, 0);
    make_round_btn(g2, LV_SYMBOL_POWER, osz, on_switch_toggle,
                   (void *)(intptr_t)DebugSwitchTarget::GasWash);

    ui_user_make_divider_vline(orow, is_1280() ? 24 : 20);

    lv_obj_t *g3 = make_row(orow, USER_PAD_SM);
    make_state_block(g3, "Base Heat", "Off", &g_dev_base_heat);
    lv_obj_set_style_text_color(g_dev_base_heat, user_color_text_dim, 0);
    make_round_btn(g3, LV_SYMBOL_POWER, osz, on_switch_toggle,
                   (void *)(intptr_t)DebugSwitchTarget::BaseHeat);
}

/* ---------------------------------------------------------------------------
 *  更多控制外壳
 * ------------------------------------------------------------------------- */

void apply_more_tab(int sel)
{
    for(int i = 0; i < 2; i++) {
        const bool on = (i == sel);
        if(g_more_tab_pill[i]) {
            lv_obj_set_style_bg_opa(g_more_tab_pill[i], on ? LV_OPA_10 : LV_OPA_TRANSP, 0);
        }
        if(g_more_tab_lbl[i]) {
            lv_obj_set_style_text_color(g_more_tab_lbl[i], on ? kTabBlue : user_color_text, 0);
            lv_obj_set_style_text_opa(g_more_tab_lbl[i], on ? LV_OPA_COVER : LV_OPA_30, 0);
        }
        if(g_more_pane[i]) {
            if(on) {
                lv_obj_remove_flag(g_more_pane[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_more_pane[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void on_more_tab(lv_event_t *e)
{
    apply_more_tab((int)(intptr_t)lv_event_get_user_data(e));
}

void on_more_back(lv_event_t *) { show_view(false); }

void build_more_view(lv_obj_t *parent)
{
    g_more_view = make_box(parent);
    lv_obj_set_size(g_more_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_left(g_more_view, body_pad_l(), 0);
    lv_obj_set_style_pad_right(g_more_view, body_pad_l(), 0);
    lv_obj_set_style_pad_top(g_more_view, body_pad_t(), 0);
    lv_obj_set_style_pad_bottom(g_more_view, more_pad_bot(), 0);
    lv_obj_set_flex_flow(g_more_view, LV_FLEX_FLOW_COLUMN);
    /* 两段间距不等（稿 20 / 24），故 pad_row 取前一段、后一段靠 body 的 margin_top 补差 */
    lv_obj_set_style_pad_row(g_more_view, more_gap_chrome(), 0);

    /* ---- 顶行：Back + Motion / Device ---- */
    lv_obj_t *chrome = make_row(g_more_view, USER_PAD);
    lv_obj_set_width(chrome, LV_PCT(100));

    lv_obj_t *back = lv_obj_create(chrome);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, LV_SIZE_CONTENT, is_1280() ? 40 : 34);
    lv_obj_set_style_pad_hor(back, USER_PAD_SM / 2, 0);
    lv_obj_set_style_radius(back, USER_BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(back, user_color_text, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(back, USER_PAD_SM / 4, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(back, USER_PAD_SM);
    lv_obj_add_event_cb(back, on_more_back, LV_EVENT_CLICKED, nullptr);
    make_label(back, LV_SYMBOL_LEFT, user_font_text_sm(), user_color_text, LV_OPA_50);
    make_label(back, "Back", user_font_text_sm(), user_color_text, LV_OPA_COVER);

    static const char *const kTabs[2] = { "Motion", "Device" };
    for(int i = 0; i < 2; i++) {
        lv_obj_t *pill = lv_obj_create(chrome);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, is_1280() ? 120 : 98, is_1280() ? 40 : 34);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pill, kTabBlue, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(pill, on_more_tab, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        g_more_tab_pill[i] = pill;
        g_more_tab_lbl[i] = make_label(pill, kTabs[i], user_font_text_sm(),
                                       user_color_text, LV_OPA_COVER);
        lv_obj_center(g_more_tab_lbl[i]);
    }

    /* ---- 六列传感器条（稿 1072×109，列间 1px 竖线）---- */
    lv_obj_t *strip = lv_obj_create(g_more_view);
    lv_obj_remove_style_all(strip);
    lv_obj_set_width(strip, LV_PCT(100));
    lv_obj_set_height(strip, is_1280() ? 109 : 88);
    lv_obj_set_style_radius(strip, USER_RADIUS, 0);
    lv_obj_set_style_border_color(strip, user_color_text, 0);
    lv_obj_set_style_border_opa(strip, LV_OPA_10, 0);
    lv_obj_set_style_border_width(strip, 1, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < 6; i++) {
        if(i > 0) {
            ui_user_make_divider_vline(strip, is_1280() ? 24 : 20);
        }
        lv_obj_t *cell = make_col(strip, USER_PAD_SM / 2);
        lv_obj_set_flex_grow(cell, 1);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        make_label(cell, kSensorCols[i].caption, user_font_text_sm(),
                   user_color_text, LV_OPA_50);
        lv_obj_t *vrow = make_row(cell, USER_PAD_SM / 4);
        lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_END);
        g_sensor_val[i] = make_label(vrow, "--", user_font_value(),
                                     user_color_text, LV_OPA_80);
        make_label(vrow, kSensorCols[i].unit, user_font_caption_sm(),
                   user_color_text, LV_OPA_40);
    }

    /* ---- 两个 Tab 的内容面（叠在同一格里，靠显隐切）---- */
    lv_obj_t *body = make_box(g_more_view);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    /* 补足「传感器条 → 内容」那一段与 pad_row 的差（稿 24 − 20） */
    lv_obj_set_style_margin_top(body, more_gap_strip() - more_gap_chrome(), 0);

    build_motion_pane(body);
    build_device_pane(body);
    for(int i = 0; i < 2; i++) {
        lv_obj_add_flag(g_more_pane[i], LV_OBJ_FLAG_FLOATING);
        lv_obj_set_pos(g_more_pane[i], 0, 0);
    }
    apply_more_tab(0);
}

void more_view_on_enter(void)
{
    /* 传感器条与三轴位置都靠 Debug 回读，进视图先确保回调在位（切视图不重进页面，
     * 所以不能只靠 page_user_control_refresh 那一次注册）。 */
    more_view_register_callbacks();
}

void more_view_register_callbacks(void)
{
    auto &proxy = ui::DebugMsgProxy::instance();
    proxy.register_sensor([](const middleware::DebugSensorPayload &p) {
        if(!g_active) {
            return;
        }
        /* 顺序与 kSensorCols 一致，见该表注释 */
        const float v[6] = { p.oxygen_percent, p.pressure_kpa, p.pm25,
                             p.humidity_pct, p.cabin_temp_c, p.baseplate_temp_c };
        for(int i = 0; i < 6; i++) {
            set_num(g_sensor_val[i], v[i], 1);
        }
    });
    proxy.register_axis([](const middleware::DebugAxisStatePayload &p) {
        if(!g_active) {
            return;
        }
        set_num(g_axis_pos[0], p.z_pos_mm, 3);
        set_num(g_axis_pos[1], p.s_pos_mm, 3);
        set_num(g_axis_pos[2], p.d_pos_deg, 3);
        set_num(g_z_zero_lbl, p.z_work_zero_mm, 3);
    });
}

/* ============================ 观察者接线 ============================ */

void on_env_change(const ui_device_snapshot_t *s) { if(g_active) { apply_env(s); } }
void on_supply_change(const control_supply_t *s)  { if(g_active) { apply_supply(s); } }

} // namespace

/* ============================ 页面入口 ============================ */

lv_obj_t *page_user_control_create(lv_obj_t *parent)
{
    g_root = make_box(parent);

    /* ---- 整屏出血的页面底（稿 5782:12912 + 12913）----
     * 稿上底是 #272628 铺满 1280×800，再叠一层 -49° 渐变；侧栏与设备条都是压在它上面的
     * 浮层。所以这一层必须跨过 content_panel_ 的左/顶 padding 铺满整屏，否则设备条那条
     * 透明带会露出 user_color_bg(#0a0909)、在 y=USER_CONTENT_TOP 处出现一道横向接缝。
     * 做法照首页 page_user_home_create()：负偏移 + LV_HOR_RES/LV_VER_RES，再用自己的
     * padding 把内容推回内容区。
     *
     * 两个坑（首页注释里也记着）：尺寸**不能**用 LV_PCT(100)，那算的是父内容区、会被
     * pad_top 扣掉一条；纵向抵消的是 content_panel_ 的 pad_top，跟的是 USER_CONTENT_TOP
     * 而**不是**设备条高 USER_TOPBAR_H（1280 档 112 对 128），用错会整页竖直错开 16。 */
    lv_obj_set_size(g_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_root, -USER_SIDEBAR_W, -USER_CONTENT_TOP);
    lv_obj_set_style_pad_left(g_root, USER_SIDEBAR_W, 0);
    lv_obj_set_style_pad_top(g_root, USER_CONTENT_TOP, 0);

    /* 渐变**取不透明两停**而不是稿上那种带 alpha 的叠加层：alpha 停会让那一段背景透到
     * 底下的 user_color_bg 去，就得再养一层实色底。把稿的 rgba(61,58,78,0.4) 预先与
     * #272628 混好（得 #302E37）之后一个对象就够，观感等价。
     * 描述符必须 static 持久——`bg_grad` 只存指针（同 ui_slide_unlock.cpp 的红条）。 */
    {
        static lv_grad_dsc_t s_bg_grad;
        const lv_color_t cols[2] = { lv_color_hex(0x272628), lv_color_hex(0x302E37) };
        const lv_opa_t   opa[2]  = { LV_OPA_COVER, LV_OPA_COVER };
        /* 稿上第一停在 49%，前半段是纯底色，故用 EXTEND_PAD 把 0..49% 撑平 */
        const uint8_t    frac[2] = { 125, 255 };
        lv_grad_init_stops(&s_bg_grad, cols, opa, frac, 2);
        /* -49°：渐变轴指向左上，即 0.4 那端在左上角 */
        lv_grad_linear_init(&s_bg_grad, LV_GRAD_RIGHT, LV_GRAD_BOTTOM,
                            LV_GRAD_LEFT, LV_GRAD_TOP, LV_GRAD_EXTEND_PAD);
        lv_obj_set_style_bg_color(g_root, lv_color_hex(0x272628), 0);
        lv_obj_set_style_bg_grad(g_root, &s_bg_grad, 0);
        lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    }

    build_main_view(g_root);
    build_more_view(g_root);
    lv_obj_add_flag(g_more_view, LV_OBJ_FLAG_HIDDEN);

    /* 三路数据都走别人持有的订阅点再扇出，本页只挂观察者，见文件头说明。 */
    ui_device_state_add_observer(on_env_change);
    control_supply_add_observer(on_supply_change);
    ui_clean_state_add_observer(on_clean_event);

    return g_root;
}

void page_user_control_refresh(bool /*force*/)
{
    g_active = true;

    /* 进页先按现有快照画一遍，别等下一次推送——环境是周期报（最长一个上报间隔），
     * 而供应度只在请求后才回，所以下面还要主动拉一次。 */
    apply_env(ui_device_state_get());
    apply_supply(control_supply_get());
    control_supply_request();

    /* 设备开关态/激光功率/传感器条都靠 DebugMsgProxy 的单槽回调回读，与调试页共用，
     * 所以进页注册、离页清空（page_user_control_exit）。 */
    auto &proxy = ui::DebugMsgProxy::instance();
    proxy.register_device([](const middleware::DebugDeviceStatePayload &p) {
        if(!g_active) {
            return;
        }
        apply_toggle(&g_light_sw, p.light_on != 0, kLightOn, false);
        if(g_light_sw.state) {
            lv_label_set_text(g_light_sw.state, p.light_on ? "On" : "Off");
        }
    });
    proxy.register_laser([](const middleware::DebugLaserStatePayload &p) {
        if(!g_active) {
            return;
        }
        /* power_raw 是原始值，量纲随型号变（JFSC 0..255；AL 单位 0.1%），
         * 换算口径与调试页 Laser 页一致。 */
        const float pct = (p.laser_type == 2)
                            ? (float)p.power_raw / 10.0f
                            : (float)p.power_raw * 100.0f / 255.0f;
        apply_laser((uint8_t)(pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct)));
    });
    more_view_register_callbacks();

    /* apply_device_bar() 换页时按默认规则重置了浮层显隐，按当前视图再设一次 */
    ui_notice_stack_set_visible(!in_more_view());
}

void page_user_control_exit(void)
{
    g_active = false;
    ui_keyboard_hide();
    /* lv_timer 的生命周期不跟页面走，离页不撤会在别的页面上弹 toast */
    dev_clean_timeout_disarm();

    /* 这四个回调是**单槽**的、调试页也在用：不清则从本页切去调试页后，那边的传感器条、
     * 三轴位置、设备行、激光读数都不再刷新（page_user_debug_exit() 同样清这一组）。 */
    auto &proxy = ui::DebugMsgProxy::instance();
    proxy.register_device(nullptr);
    proxy.register_laser(nullptr);
    proxy.register_sensor(nullptr);
    proxy.register_axis(nullptr);

    close_pm_dlg();
    close_laser_dlg();

    /* 通知浮层的显隐**不用在这里还原**：PageManager::switch_to() 的顺序是
     * on_exit() → apply_device_bar() → on_enter()，紧随其后那句就按新页的规则重设了。
     * 在这儿写 set_visible(true) 只会被覆盖，还会让人以为本页要负责别页的浮层。 */
}
