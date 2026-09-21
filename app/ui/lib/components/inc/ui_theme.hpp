#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  显示分辨率 Profile —— 运行时自适应
 *  ----------------------------------------------------------------------------
 *  现在 profile 不再是编译期宏选择，而是在 ui_theme_init() 中根据 DRM 实际
 *  分辨率自动挑选（按最长边）：
 *    实际最长边 ≥ 1600 px  → UI_RES_1920 套尺寸
 *    1100 ≤ 最长边 < 1600  → UI_RES_1280 套尺寸
 *    实际最长边 <  1100 px → UI_RES_1024 套尺寸（1024×600 物理横屏）
 *
 *  一份固件同时支持三种屏：编译时不用 -D 任何参数，不用改 .defaults。
 *
 *  如果你确实想强制走某套 profile（比如测试 1280 套布局），可以：
 *    1) 在调用 ui_theme_init() 前 ui_theme_force_profile(UI_RES_1280)，或
 *    2) 设置环境变量 LVGL_UPC_PROFILE=1 / 2 / 3
 * ============================================================================ */
#define UI_RES_1920    1
#define UI_RES_1280    2
#define UI_RES_1024    3

/* ---- KlipperScreen 风格配色（与分辨率无关） ---- */
extern const lv_color_t ui_color_bg;
extern const lv_color_t ui_color_sidebar;
extern const lv_color_t ui_color_panel;
extern const lv_color_t ui_color_panel_alt;
extern const lv_color_t ui_color_accent;
extern const lv_color_t ui_color_accent_dim;
extern const lv_color_t ui_color_text;
extern const lv_color_t ui_color_text_dim;
extern const lv_color_t ui_color_border;
extern const lv_color_t ui_color_danger;

/* ---- 用户 UI（Snowpod 风格，深黑底 + 主色高光；user_color_accent 现为蓝 #4258ff） ---- */
extern const lv_color_t user_color_bg;
extern const lv_color_t user_color_panel;
extern const lv_color_t user_color_panel_alt;
extern const lv_color_t user_color_accent;
extern const lv_color_t user_color_accent_dim;
extern const lv_color_t user_color_text;
extern const lv_color_t user_color_text_dim;
extern const lv_color_t user_color_icon_off;
extern const lv_color_t user_color_divider;
extern const lv_color_t user_color_status_ok;
extern const lv_color_t user_color_status_err;

/* ---- 浅色稿（新首页待机态 + 侧栏 + 通知三态，稿 6039:65949 / 6064:71609 / 6064:71936）----
 *
 *  这一组与上面那套深色**并存**而不是替换：目前只有首页待机态、侧栏（整屏视频态除外）
 *  和通知三态走浅色，打印态/监控态与其余页面仍是深色。等这两块都翻完再考虑合并。
 *
 *  `user_light_shadow` 是稿上那一族投影的共同色（侧栏 4%、卡片 4%、浮层 10%），
 *  不透明度各处不同，由调用方给 `shadow_opa`，这里只出色相。 */
extern const lv_color_t user_light_bg;        /**< 页面底 #fafafa */
extern const lv_color_t user_light_card;      /**< 白卡 #ffffff */
/** 稿上那批「白玻璃」控件的底 #fcfcfc（首页左下相机方钮、右上铃铛、开门滑块轨道）。
 *  稿里它们都是 `#FCFCFC` 加一层 backdrop blur，LVGL 没有背景模糊，直接给不透明色；
 *  与纯白的 user_light_card 只差 3/255，但稿上确实是两个值。 */
extern const lv_color_t user_light_glass;
extern const lv_color_t user_light_text;      /**< 正文/标题 #333145（稿变量 Dark black） */
extern const lv_color_t user_light_value;     /**< 四宫格数值 #111111，比正文再深一档 */
extern const lv_color_t user_light_accent;    /**< 主色 #485dff（稿变量 SOC Theme blue） */
extern const lv_color_t user_light_icon_off;  /**< 侧栏未选中图标 #9e9e9e */
extern const lv_color_t user_light_nav_on;    /**< 侧栏选中项底板 #333145 */
extern const lv_color_t user_light_shadow;    /**< 投影色相 #0c1c61 */
extern const lv_color_t user_light_divider;   /**< 设备条竖分隔线 #d9d9d9 */
extern const lv_color_t user_light_row_bg;    /**< 面板条目底 #333145 @5% */
extern const lv_color_t user_light_more_bg;   /**< 面板「查看剩余」行底 #f5f5f6 */
/** 设备条状态字绿 #00c74e。**不是 user_color_status_ok**：那个 #52ff92 是给深底调的
 *  亮薄荷绿，压在 #f5f5f5 上几乎看不见。 */
extern const lv_color_t user_light_ok;
/** 设备条上的设备名 #1d1d24。比正文的 user_light_text(#333145) 再深一点，稿上这两处
 *  就是不同的值。 */
extern const lv_color_t user_light_dev_name;
/** 未读红点 #fe6868。两处共用：侧栏消息图标（稿 6255:95797）与首页右上铃铛
 *  （稿 6201:85994）。**不是 ui_color_danger(#f85149)**，那个是深色档的危险红。 */
extern const lv_color_t user_light_notice_dot;
/** 设置页左栏渐变底的中间色 #f4f4f4（稿 6240:92194 的 50% 色标；两端是
 *  user_light_bg 的 #fafafa）。只有这一处在用。 */
extern const lv_color_t user_light_rail_mid;

/* ============================================================================
 *  尺寸 / 字体集合
 *  ----------------------------------------------------------------------------
 *  把所有原本写成 #define 的尺寸宏统一收到一个只读结构体里。
 *  全局指针 g_ui 在 ui_theme_init() 中根据屏分辨率指向 s_theme_1920 或
 *  s_theme_1280（在 ui_theme.cpp 内）。
 *
 *  调用代码不需要任何改动：USER_PAD / USER_HOME_DEVICE_W 等宏会展开成
 *  g_ui-> 字段访问，运行时拿到当前 profile 的值。
 * ============================================================================ */
typedef struct {
    /* ---- 开发者 Shell 尺寸 ---- */
    int32_t ui_sidebar_w;
    int32_t ui_nav_item_h;
    int32_t ui_nav_icon_w;
    int32_t ui_pad;
    int32_t ui_pad_sm;
    int32_t ui_radius;
    int32_t ui_form_label_w;
    int32_t ui_form_label2_w;
    int32_t ui_gain_row_label_w;
    int32_t ui_gain_row_ta_w;
    int32_t ui_jog_btn_sz;
    int32_t ui_jog_home_sz;
    int32_t ui_jog_step_w;
    int32_t ui_jog_step_btn_h;
    int32_t ui_jog_block_min_h;
    int32_t ui_input_h;
    int32_t ui_ta_ip_w;
    int32_t ui_ta_short_w;
    int32_t ui_btn_conn_w;
    int32_t ui_bar_title_w;
    int32_t ui_bar_value_w;
    int32_t ui_bar_h;
    int32_t ui_indicator_sz;
    int32_t ui_switch_label_w;
    int32_t ui_switch_w;
    int32_t ui_switch_h;
    int32_t ui_settings_row_h;
    int32_t ui_settings_label_w;
    int32_t ui_settings_ta_w;
    int32_t ui_list_item_h;
    int32_t ui_btn_open_w;
    int32_t ui_btn_action_w;
    int32_t ui_btn_job_w;
    int32_t ui_confirm_box_w;
    int32_t ui_confirm_text_w;
    int32_t ui_confirm_btn_w;
    int32_t ui_confirm_btn_h;
    int32_t ui_page_anim_ms;
    int32_t ui_page_anim_slide;
    int32_t ui_btn_pad_hor;
    int32_t ui_btn_pad_ver;
    int32_t ui_nav_active_border;

    /* ---- 用户 Shell 尺寸 ---- */
    int32_t user_sidebar_w;
    /** 侧栏里每枚导航项（可点方块）的边长，稿 5339:27264 的 Home bar 是 96@1280。
     *  侧栏宽由「两侧内边距 + 项」凑成（128 = 16 + 96 + 16），所以那段内边距按
     *  (USER_SIDEBAR_W - USER_NAV_ITEM_SZ)/2 反推，不再借用 USER_PAD_SM——后者是
     *  通用小间距，跟侧栏几何没有关系，两者绑在一起改哪边都会牵连另一边。 */
    int32_t user_nav_item_sz;
    /** 侧栏内相邻两枚导航项的间距（稿 16@1280）。稿上整组是「padding 16 + gap 16
     *  + 竖向居中」，不是按屏高均分，故 menu_panel_ 用 FLEX_ALIGN_CENTER + pad_row。 */
    int32_t user_nav_gap;
    /** 注意：本字段与侧栏无关，是设置页那些列表行的行高（page_user_settings 用了 6 处）。
     *  稿 3361:18467（左栏项）与 3361:18548（右栏 Wi-Fi 行）结构相同、高度都是
     *  24(padding) + 32(图标) + 24 = 80，所以两边共用这一个值。 */
    int32_t user_nav_item_h;
    /* ---- 设置页两栏布局（稿 3238:14296）----
     * 内容区 1152 正好闭合：左内距 36 + 左栏 300 + 列间距 36 + 右栏 604 + 右留白 176。
     * 右栏走 flex_grow 自动得到 604，所以只需定死左栏宽、间距和右留白三个数。
     * 右留白不是白放的——翻页圆钮落在那一段里（稿 x=1184，距屏右 32）。
     * **0 = 沿用改动前的旧式**（左栏 LV_PCT(34)、内距 USER_PAD、列间距 USER_PAD_LG、
     * 右内距 USER_PAD），1024/1920 两档填 0 以保持渲染不变。 */
    int32_t user_settings_nav_w;
    /** Wi-Fi 开关分段控件的高（稿 3361:18541 是 72）。宽度不用另给：行高改成 80 后
     *  旧式的 USER_NAV_ITEM_H*16/5 正好算出稿上的 256。0 = 沿用旧式 *4/5。 */
    int32_t user_settings_seg_h;
    /** 侧栏导航图标的**画布边长**（1280 档取稿值 32）。资源按这三个数各出一份原生
     *  尺寸，运行时 1:1 不缩放，所以这个值就是上屏尺寸本身——**改它必须同步改
     *  tools/gen_home_metric_icons.py 的 NAV_SIZES**，否则 pick_icon 挑不到等宽的
     *  就退回缩放，2px 的细描边会被 LVGL 的 2 抽头点采样采成毛刺。
     *
     *  早先这个字段是另一个意思：那时只有一份 64×64 的资源、字形在里面占 52，它是
     *  「把整张 64 缩到多大」的目标值（1280 档 59 是按旧稿字形 48 反推的 48÷(52/64)）。
     *  换成按档位出原生尺寸之后那套反推已作废，别再按「字形 ÷ 占比」去算。
     *
     *  稿上字形占图标盒的 30/32；1024/1920 两档这一轮不对稿、只保持改版前的上屏
     *  字形大小（≈47 / 49），除以 0.9375 反推得 50 / 52。 */
    int32_t user_nav_icon_sz;
    int32_t user_nav_icon_pad;
    int32_t user_topbar_h;
    /** 页面内容区的上偏移（`content_panel_` 的 `pad_top`，稿 112@1280）。
     *  **不等于 user_topbar_h**：那个管设备条自身的高，决定设备名那行竖直居中到
     *  哪（稿上行心 y=64，故条高 128）；本字段管页面内容从哪开始（稿上 Tab 文字
     *  顶在 y=112）。稿里设备行底 80 到内容顶只隔 32，两者本就不是同一个数，
     *  共用一个字段会顾此失彼，故拆开。 */
    int32_t user_content_top;
    int32_t user_topbar_pad_hor;
    int32_t user_topbar_pad_ver;
    int32_t user_pad;
    int32_t user_pad_sm;
    int32_t user_pad_lg;
    int32_t user_radius;
    /** 稿里那套弹窗/详情面板按钮组件的圆角（8@1280，不是全圆胶囊）：
     *  确认弹窗的取消/危险钮、选文件页详情面板的 Confirm Print / Delete File。 */
    int32_t user_btn_radius;
    int32_t user_btn_primary_w;
    int32_t user_btn_primary_h;
    int32_t user_btn_door_sz;
    int32_t user_btn_door_icon_sz;
    int32_t user_value_label_w;
    int32_t user_status_dot_sz;
    int32_t user_home_device_w;
    int32_t user_home_device_h;
    /* ---- 首页待机布局的定位数（浅色稿 6039:65949 原生 1280×800，量出来的绝对值）----
     * 稿上这几处都是定宽/定位，`LV_PCT` 吃整数百分比碰不到，所以收进主题表。
     * 1024/1920 两档按 ×0.8 / ×1.5 折算，没有逐个核过。
     *
     * 早先这一族里还有三个字段，都随浅色稿退役了：
     *   user_home_action_row_w  左下那行入口胶囊的总宽。那一行现在拆成各自定宽的
     *                           一枚方钮 + 一枚 PRINT 胶囊，见 user_light_btn_sz /
     *                           user_home_print_btn_w。
     *   user_home_cabin_w / _lead_w / _pad_right
     *                           右侧 CABIN 那套「悬浮文本 + 引线 + 空心圆环」的几何。
     *                           现在是规规矩矩的 2×2 白卡网格，见 user_home_grid_w。 */
    /** HOME 空气质量卡宽（浅色稿 6039:65949 是 300；旧暗色稿是 354）。
     *  高度另见 user_home_card_h——浅色稿里这张卡与右侧四宫格等高并排，不再由内容撑。
     *  **改这个值必须同步改 tools/gen_home_bg.py 的 CARD_SIZES**：卡背景是预生成位图，
     *  尺寸对不上就会被 COVER 拉伸。 */
    int32_t user_home_card_w;
    /** HOME 空气质量卡顶距（浅色稿 275；旧暗色稿是 241）。原先写成
     *  USER_TOPBAR_H + USER_PAD_LG*2，间距梯度改成 32 之后那个式子离稿更远，故独立成字段。 */
    int32_t user_home_card_top;
    /** 那批 A8 指标图标的显示边长（稿 32@1280）：首页左下两枚按钮的图标、
     *  打印态/监控态各卡头的图标。浅色稿首页那批卡头图标（HOME 卡头的房子与切换钮、
     *  四宫格每格右上那枚）是 24，按本值 *3/4 折算——三档正好整除成 36 / 24 / 19，
     *  不必再开一个字段，见 page_user_home.cpp 的 light_icon_sz()。
     *  两套尺寸生成脚本里都有原生位图（gen_home_metric_icons.py 的 METRIC_SIZES 与
     *  METRIC_SM_SIZES），所以 pick_icon 都能精确命中、运行时不缩放。 */
    int32_t user_metric_icon_sz;
    int32_t user_back_btn_w;
    int32_t user_back_btn_h;
    /** 全屏媒体页浮层圆钮的直径（延时播放器关闭钮 + 相册浏览器的关闭/翻页钮）。
     *
     *  独立成字段而不是继续用 user_back_btn_h：后者还被弹窗关闭钮、打印态浮层胶囊
     *  （control.cpp 里 *3/2 与 *9/2）、调试页与设置页返回钮引用，为了两个全屏页把它
     *  调大会牵动一大片；而在无稿档位它还通过 scroll_btn_size() 影响翻页钮直径。
     *
     *  三档现均取 user_back_btn_h 的 1.5 倍（96/66/60），理由见 ui_theme.cpp 文件头
     *  「触摸尺寸的刻意偏稿」。0 = 沿用 user_back_btn_h。 */
    int32_t user_media_btn_h;
    /** 延时播放器中央播放/暂停钮的直径，与上面的关闭钮**分开给值**。
     *
     *  原先写成 user_media_btn_size() * 2，关闭钮放大后它会跟着涨到屏高的两成，
     *  过于抢眼；拆开后两者互不牵动。0 = 沿用旧的 user_media_btn_size() * 2。 */
    int32_t user_media_play_h;
    int32_t user_sensor_strip_h;   /**< 调试页传感器条高（原先借用 user_topbar_h） */
    /** 空态那枚图标圆底的直径（稿 4531:7206 是 80，内含 36 的图标）。
     *  0 = 沿用旧式 user_btn_primary_h * 10/9——那个式子是照 1920 档（72×10/9 = 80）
     *  写的，1280 档只算得 57，与稿差 23，故本档改为直接给值。 */
    int32_t user_empty_ring_sz;

    /* ---- 打印流程：弹窗 / 滑动 / 旋转环 / 进度 / 打印中底条 ---- */
    int32_t user_dlg_w;
    int32_t user_dlg_pad;
    int32_t user_dlg_pad_lg;
    int32_t user_dlg_title_pad_bottom;
    int32_t user_dlg_radius;
    int32_t user_dlg_body_min_h;
    int32_t user_dlg_footer_btn_w;
    /** 弹窗页脚按钮 / 满宽主按钮高：按稿 64@1280 等比折算（1024=51、1920=96）。
     *  共用方：确认弹窗页脚、门弹窗与放版弹窗的满宽钮、选文件页详情两枚按钮与厚度分段。 */
    int32_t user_dlg_footer_btn_h;
    /* ---- 浅色档弹窗的行盒补偿（**只在 light_theme 下用**）----
     * 稿上的文字行盒比 LVGL 预生成 Montserrat 的 line_height 高一截：稿标题用
     * PingFang SC（1.4 倍行距，28px → 39），正文用 Inter（1.21 倍，20px → 24），
     * 而 montserrat_28 / _20 的 line_height 只有 30 / 22。差额不补的话，24/16/32 那套
     * 稿间距摆出来的卡片会比稿的 231 矮 11px，卡片又是居中的，于是正文、页脚、卡片
     * 下缘会整体上移 5~6px——量过。
     * 两个字段就是这个差额，上下按稿基线分（上 = lead - lead/2，下 = lead/2）。
     * 1280 档是逐项算的稿值；1024/1920 两档按 ×0.8 / ×1.5 折算后减各自字号的
     * line_height，没有稿可核。 */
    /** 标题行盒补偿：稿行盒 59/39/31 − line_height 44/30/27。 */
    int32_t user_dlg_title_lead;
    /** 正文行盒补偿：稿行盒 36/24/19 − line_height 27/22/18。 */
    int32_t user_dlg_body_lead;
    int32_t user_file_dlg_w;
    int32_t user_file_dlg_preview_w;
    int32_t user_file_dlg_preview_h;
    int32_t user_file_sel_w;
    int32_t user_file_sel_h;
    int32_t user_file_card_w;
    int32_t user_file_card_h;
    int32_t user_file_thumb_h;
    int32_t user_slide_w;
    int32_t user_slide_h;
    int32_t user_slide_knob_sz;
    /* ---- 开门滑块（首页 / 监控页 / 放版弹窗共用的那条红色滑动条）----
     * 单独立三个字段而不是复用 user_slide_*：后者还被延时摄影页的拖动预览框
     * （USER_SLIDE_W * 3/5）和放版弹窗的滑条最小宽钳制用着，跟着改会牵连那两处。
     * 1280 档取稿 5339:27265 的 400×80、圆头 64（内缩 8）——与打印态取件胶囊
     * （USER_PRINT_PILL_W）和底部控件组「blob 80 装 64 圆钮」是同一套几何。
     * 1024/1920 两档填的是改动前那套 *4/3 派生式算出的值，渲染不变。 */
    int32_t user_door_slide_w;
    int32_t user_door_slide_h;
    int32_t user_door_slide_knob;
    /** 圆环加载圈直径与线宽。全部四个 ui_arc_spinner（门弹窗、准备弹窗、转运盒两步）
     *  都只从这两个值取尺寸，没有别处写死，改这里即全局生效。 */
    int32_t user_spinner_sz;
    int32_t user_spinner_stroke;
    int32_t user_progress_w;
    int32_t user_progress_h;
    int32_t user_print_bar_h;
    int32_t user_print_bar_pad_hor;
    int32_t user_print_bar_pad_ver;
    int32_t user_print_bar_gap;
    int32_t user_print_terminate_w;
    int32_t user_print_terminate_h;

    /* ---- 打印态浮层（稿 3588:8735 / 3589:9992 一套六态，按 1280 稿折算三档）----
     * 六个态（打印中/暂停/中断/完成/吹粉中/吹粉完成）共用一张 stats-card、一条
     * 提示条、一组底部复合控件，尺寸全部走这几个常量，不写死坐标。 */
    /** stats-card 宽。1280 档取稿值 416，按稿分解正好是
     *  16(内距) + 100(缩略图) + 16(列距) + 268(右列) + 16(内距)。
     *  1024/1920 两档仍是按稿宽补 10% 的旧值（366/686）——「Montserrat 比稿里的 Futura
     *  更占地方、按稿宽会把 CONSUMABLES 的 caption 截掉」这条只在那两档成立：两列是
     *  等分的，1024 档若用稿值等比（333）每列只剩 109.5，而 CONSUMABLES 在 Montserrat 12
     *  加 1 字距下实测 107.4，余量 2px 一碰就截。1280 档用 416 时每列 130、余量 22.6，安全。
     *  **本字段只给 stats-card**；右上通知栈另有 user_notice_card_w，稿值不同。 */
    int32_t user_print_card_w;
    /** 右上通知栈的卡片宽。稿 5339:27299/27310 量出来是 400@1280，与 stats-card 的 416
     *  不是一个数——早先两处共用 user_print_card_w，是因为通知栈还摆在左下、要与那张卡
     *  左右对齐；栈挪到右上之后这层对齐关系已经不存在了。
     *  1024/1920 两档沿用拆分前的值（366/686），渲染不变。 */
    int32_t user_notice_card_w;
    int32_t user_print_thumb_sz;  /**< stats-card 左侧缩略图边长（稿 100） */
    /* 下面两个尺寸与预生成的玻璃底衬图强耦合：底衬按 tools/gen_ctl_backdrop.py 的
     * PROFILES 三档出图、运行时不缩放，改了必须同步改脚本并重跑，否则会和按钮错位。
     * 底衬上那颗圆的直径 = ctl_btn×5/4、两块之间的缝 = ctl_btn/8（均取自稿）。 */
    int32_t user_print_ctl_btn;   /**< 底部复合控件里的圆钮直径（稿 64；右侧中部的拍照钮同尺寸） */
    int32_t user_print_pill_w;    /**< 底部复合控件的胶囊宽（稿 400，即滑动取件轨道宽） */
    int32_t user_print_badge_sz;  /**< 徽标行状态图标边长（稿 16） */
    int32_t user_print_prog_h;    /**< 卡片内进度条高（稿 8） */
    /** 右上环境条每列的定宽（稿 4972:27560 是 64@1280）。
     *  **0 = 按内容自适应**：只有 1280 档收得进稿的 64（caption 降到 caption_sm 后四列
     *  最宽 62.1px），1024/1920 两档装不下——1024 档折算宽只有 51，而 `Humidity` 的
     *  caption @12 就要 57.1、`101.3 kpa` 的数值行还要 68，所以那两档填 0 走自适应，
     *  渲染与改动前一致。 */
    int32_t user_env_col_w;

    /* ---- 打印历史页（浅色稿 6122 一族，按 1280 稿等比折算三档）---- */
    int32_t user_hist_tab_h;      /**< Tab 行高（稿 60@1280） */
    /** 旧版历史卡片高。**历史页自己已经不用了**（浅色稿改成了网格、卡高由卡宽推），
     *  但全屏照片浏览器底部那条缩略图带还按它的 2/5 取格高，见 page_user_photo 的
     *  `strip_cell_h()`，所以字段留着。 */
    int32_t user_hist_card_h;
    int32_t user_hist_check_sz;   /**< 多选态复选框边长（历史页在用；消息页新稿无多选） */
    /** Tab 行左内距（稿 48@1280：Tab 文字 x=176，侧栏占 128）。
     *  历史页与消息页的浅色稿里，Tab 行、日期标题、卡片三者左缘齐平，都走这一个值，
     *  不再像旧稿那样让列表比 Tab 行多缩 16（那个 user_hist_list_pad_l 已删）。
     *  两页的 Tab 项间距也是 48，同取本值。0 = 沿用旧式 USER_PAD。 */
    int32_t user_hist_pad_l;
    /** 网格的列间距与行间距（稿 32@1280，两者同值）。
     *  **卡宽不单开字段**，由「内容区宽 − 2×本值」除以 3 现算——稿上三列是等分撑满的，
     *  写死 331 会因为 3×331+2×32 = 1057 比内容区的 1056 多 1 而挤掉一列。 */
    int32_t user_hist_grid_gap;
    int32_t user_hist_card_radius;  /**< 网格卡片圆角（稿 16@1280） */
    int32_t user_hist_selbar_h;     /**< 多选态底部浮条高（稿 6251:94204 是 80@1280） */
    int32_t user_hist_del_btn_w;    /**< 底部浮条里 Delete 钮的宽 / 高（稿 140×64@1280） */
    int32_t user_hist_del_btn_h;

    /* ---- 浅色稿几何（稿 6039:65949 原生 1280×800）----
     * 只有 1280 档是量出来的稿值，1024/1920 两档按 ×0.8 / ×1.5 折算，没有逐个核过。
     * 稿上的圆角/内距族里，8 = USER_PAD_SM/2、24 = USER_PAD、32 = USER_PAD_LG 都能
     * 落回现有梯度，只有那个到处用的 24 圆角落不回（USER_RADIUS 本档是 14），故单开。 */
    /** 浅色稿的统一圆角（白卡、左下两枚按钮、开门滑块、铃铛、面板；稿 24@1280）。
     *  toast 的 8 走 USER_BTN_RADIUS，面板条目的 12 取本值的一半。 */
    int32_t user_light_radius;
    /** 右上铃铛的边长（稿 72@1280）。铃铛内图标 40 取本值 *5/9。
     *  **上一版浅色稿里左下方钮与开门滑块圆钮也是这个值，新稿把它们改成了 84**，
     *  那三处现在走 user_action_btn_sz。 */
    int32_t user_light_btn_sz;
    /** 首页底部那族 84 见方的按钮（稿 6201:85989/85981/85973）：左下相机方钮、PRINT
     *  胶囊的高、右下开门滑块的旋钮，稿上三处同值。钮内图标 40。 */
    int32_t user_action_btn_sz;
    /** HOME 空气质量卡的高，同时也是右侧四宫格的高（稿 300@1280，两者等高并排）。 */
    int32_t user_home_card_h;
    /** 四宫格总宽（稿 340@1280）。单卡尺寸由总宽/总高减去 USER_PAD_SM/2 的间距再折半
     *  推出来（1280 档正好是稿的 166×139），不再单开字段。 */
    int32_t user_home_grid_w;
    /** 问候语块顶距（稿 160@1280）。左缘走 USER_PAD_LG + 侧栏宽，与 HOME 卡同一条竖线。 */
    int32_t user_home_greet_top;
    /** 设备渲染图边长（稿 700@1280）。资源按档位出原生尺寸、运行时 1:1 不缩放，
     *  改这个值必须同步改 tools/gen_home_bg.py 的 DEVICE_SIZES。 */
    int32_t user_home_dev_img;
    /** 设备图左缘 / 顶距（稿 663 / 172@1280，整屏坐标系）。稿上这张图右下两侧都是
     *  出血的（663 + 700 = 1363 > 1280、172 + 700 = 872 > 800），超出部分由父节点裁掉，
     *  不是画错了。图本身的下半截也已经在生成时淡成全透明，见 gen_home_bg.py 的
     *  DEV_FADES。 */
    int32_t user_home_dev_img_x;
    int32_t user_home_dev_img_y;
    /** 左下 PRINT 胶囊宽（稿 202@1280）。同一行左边那枚方钮是正方形，走 user_action_btn_sz。 */
    int32_t user_home_print_btn_w;
    /** 首页待机右下那条开门滑块的轨道宽 / 高（稿 453 × 94@1280，旋钮上下各内缩 5）。
     *  **不与 user_door_slide_* 共用**：那一族是控制页与打印态那三条深色红渐变滑块，
     *  出自各自的稿、尺寸也不同（1280 档 400×80）。 */
    int32_t user_home_slide_w;
    int32_t user_home_slide_h;
    /** 首页底部那一行距屏边的退让量（稿 48@1280）：左下按钮行的底距、右下滑块的右距。
     *  **比横向的 USER_PAD_LG(32) 大**，稿上就是这么不对称的；也落不回现有的内距梯度
     *  （1024 档的 USER_PAD_LG 反而是 36，那一族不是线性折算的），故单开一个字段。
     *
     *  滑块自己的底距要再减掉旋钮的内缩：稿上是**旋钮**与左下按钮上下对齐的
     *  （两者顶都在 668、底都在 752），而轨道比旋钮高出上下各 5。 */
    int32_t user_home_edge_gap;
    /** 铃铛面板宽（稿 320@1280）。高度由条目数撑，不定值。 */
    int32_t user_panel_w;
    /** 铃铛面板里单条消息的行高（稿 48@1280）。左侧等级色竖条高取本值的 2/3（稿 32）。 */
    int32_t user_panel_row_h;

    /* ---- 字体（开发者 Shell） ---- */
    const lv_font_t *font_body;
    const lv_font_t *font_form;
    const lv_font_t *font_title;
    const lv_font_t *font_icon;

    /* ---- 字体（用户 Shell） ---- */
    const lv_font_t *font_user_value;
    const lv_font_t *font_user_text;
    /** 比 text 小一档（1280 档 16）。稿在 16 号上有一档需求而字体表原先缺：新版打印态
     *  稿 4972:27560 里通知卡的标题与副标题**同为 16**（只靠字重和 60% 透明度区分，而
     *  Montserrat 只有一个字重，同号是唯一能落地的部分），右上环境条的数值也是 16。
     *  1024/1920 两档填的就是各自 font_user_text 的字号，所以「标题」「环境条数值」这两
     *  处换用本字段后那两档渲染不变，只有 1280 档降到稿值。 */
    const lv_font_t *font_user_text_sm;
    /** 比 text 大一档（1280 档 20）。稿的「一级标题」样式：设置页左栏项标题、右栏
     *  Wi-Fi 行标题、右栏分组标题都是 20。1024/1920 两档填各自 font_user_text 的字号，
     *  换用本字段后那两档渲染不变。 */
    const lv_font_t *font_user_title;
    const lv_font_t *font_user_caption;
    /** 比 caption 再小一档，给窄容器里的说明字用（打印统计卡的 TIME/CONSUMABLES 两列）。
     *  1024/1280 两档落到 12 号——这个字号是为它才在 rongsu_ui.defaults 里打开的。 */
    const lv_font_t *font_user_caption_sm;
    /** 浅色稿的展示大字：HOME 卡的空气质量分级（稿 40@1280，为它在 lv_conf 里打开了
     *  LV_FONT_MONTSERRAT_40）。比 font_user_value 再大一档。 */
    const lv_font_t *font_user_display;
    /** 浅色稿的中号数值：四宫格读数与问候语标题（稿都是 32@1280）。 */
    const lv_font_t *font_user_metric;
    /** 浅色稿的正文（稿 24@1280）：问候副标题、HOME 卡读数、PRINT 与开门滑块的文案、
     *  顶栏设备名与状态（稿那处是 22，同取本档）。夹在 font_user_metric(32) 与
     *  font_user_title(20) 之间——user 系原先在 18 与 20 之间直接跳过了 24。 */
    const lv_font_t *font_user_body;
    /** 浅色档弹窗的标题（稿 6251:94239 是 28@1280）。
     *
     *  **只服务开了 light_theme 的弹窗**，深色那套仍走 font_user_value(32)——十五个弹窗
     *  里目前只有历史页的删除确认有浅色稿，动 value 会把另外十四个一起改了。
     *  单开一档是因为 user 系在 1280 档正好跳过 28：body 是 24、value 是 32，两边等距，
     *  拿哪个都会让卡片高度偏 9px 左右（实机量过）。
     *  三档取 40/28/24：稿 28 折算是 42 / 28 / 22.4，1920 与 1024 取最近的可用字号。 */
    const lv_font_t *font_user_dlg_title;
    /** 浅色档弹窗页脚按钮的文字（稿 6024:23245/23259 是 Poppins Medium 22@1280，
     *  为它在 rongsu_ui.defaults 里打开了 LV_FONT_MONTSERRAT_22）。
     *
     *  同样**只服务 light_theme**，深色那套仍走 font_user_text。原先借的是
     *  font_user_title(20)——它在 1280 档比稿小一档，量到按钮文字窄了约 9%。
     *  三档取 32/22/18：稿 22 折算是 33 / 22 / 17.6，1920 与 1024 取最近的可用字号。 */
    const lv_font_t *font_user_dlg_btn;
    const lv_font_t *font_user_icon;

    /* 标识本 profile 是哪一个（UI_RES_1920 / UI_RES_1280），便于运行时分支 */
    int8_t profile_id;
} ui_theme_t;

/** 当前生效的尺寸/字体集；ui_theme_init() 之前为默认 (1920)，之后跟随实际屏。 */
extern const ui_theme_t *g_ui;

/* ============================================================================
 *  原有大写宏的兼容转发（运行时变成 g_ui-> 字段访问）。
 *  调用代码继续写 USER_PAD / UI_RADIUS / USER_HOME_DEVICE_W ... 即可。
 * ============================================================================ */
#define UI_SIDEBAR_W                (g_ui->ui_sidebar_w)
#define UI_NAV_ITEM_H               (g_ui->ui_nav_item_h)
#define UI_NAV_ICON_W               (g_ui->ui_nav_icon_w)
#define UI_PAD                      (g_ui->ui_pad)
#define UI_PAD_SM                   (g_ui->ui_pad_sm)
#define UI_RADIUS                   (g_ui->ui_radius)
#define UI_FORM_LABEL_W             (g_ui->ui_form_label_w)
#define UI_FORM_LABEL2_W            (g_ui->ui_form_label2_w)
#define UI_GAIN_ROW_LABEL_W         (g_ui->ui_gain_row_label_w)
#define UI_GAIN_ROW_TA_W            (g_ui->ui_gain_row_ta_w)
#define UI_JOG_BTN_SZ               (g_ui->ui_jog_btn_sz)
#define UI_JOG_HOME_SZ              (g_ui->ui_jog_home_sz)
#define UI_JOG_STEP_W               (g_ui->ui_jog_step_w)
#define UI_JOG_STEP_BTN_H           (g_ui->ui_jog_step_btn_h)
#define UI_JOG_BLOCK_MIN_H          (g_ui->ui_jog_block_min_h)
#define UI_INPUT_H                  (g_ui->ui_input_h)
#define UI_TA_IP_W                  (g_ui->ui_ta_ip_w)
#define UI_TA_SHORT_W               (g_ui->ui_ta_short_w)
#define UI_BTN_CONN_W               (g_ui->ui_btn_conn_w)
#define UI_BAR_TITLE_W              (g_ui->ui_bar_title_w)
#define UI_BAR_VALUE_W              (g_ui->ui_bar_value_w)
#define UI_BAR_H                    (g_ui->ui_bar_h)
#define UI_INDICATOR_SZ             (g_ui->ui_indicator_sz)
#define UI_SWITCH_LABEL_W           (g_ui->ui_switch_label_w)
#define UI_SWITCH_W                 (g_ui->ui_switch_w)
#define UI_SWITCH_H                 (g_ui->ui_switch_h)
#define UI_SETTINGS_ROW_H           (g_ui->ui_settings_row_h)
#define UI_SETTINGS_LABEL_W         (g_ui->ui_settings_label_w)
#define UI_SETTINGS_TA_W            (g_ui->ui_settings_ta_w)
#define UI_LIST_ITEM_H              (g_ui->ui_list_item_h)
#define UI_BTN_OPEN_W               (g_ui->ui_btn_open_w)
#define UI_BTN_ACTION_W             (g_ui->ui_btn_action_w)
#define UI_BTN_JOB_W                (g_ui->ui_btn_job_w)
#define UI_CONFIRM_BOX_W            (g_ui->ui_confirm_box_w)
#define UI_CONFIRM_TEXT_W           (g_ui->ui_confirm_text_w)
#define UI_CONFIRM_BTN_W            (g_ui->ui_confirm_btn_w)
#define UI_CONFIRM_BTN_H            (g_ui->ui_confirm_btn_h)
#define UI_PAGE_ANIM_MS             (g_ui->ui_page_anim_ms)
#define UI_PAGE_ANIM_SLIDE          (g_ui->ui_page_anim_slide)
#define UI_BTN_PAD_HOR              (g_ui->ui_btn_pad_hor)
#define UI_BTN_PAD_VER              (g_ui->ui_btn_pad_ver)
#define UI_NAV_ACTIVE_BORDER        (g_ui->ui_nav_active_border)

#define USER_SIDEBAR_W              (g_ui->user_sidebar_w)
#define USER_NAV_ITEM_SZ            (g_ui->user_nav_item_sz)
#define USER_NAV_GAP                (g_ui->user_nav_gap)
#define USER_NAV_ITEM_H             (g_ui->user_nav_item_h)
#define USER_SETTINGS_NAV_W         (g_ui->user_settings_nav_w)
#define USER_SETTINGS_SEG_H         (g_ui->user_settings_seg_h)
#define USER_NAV_ICON_SZ            (g_ui->user_nav_icon_sz)
#define USER_NAV_ICON_PAD           (g_ui->user_nav_icon_pad)
#define USER_TOPBAR_H               (g_ui->user_topbar_h)
#define USER_CONTENT_TOP            (g_ui->user_content_top)
#define USER_TOPBAR_PAD_HOR         (g_ui->user_topbar_pad_hor)
#define USER_TOPBAR_PAD_VER         (g_ui->user_topbar_pad_ver)
#define USER_PAD                    (g_ui->user_pad)
#define USER_PAD_SM                 (g_ui->user_pad_sm)
#define USER_PAD_LG                 (g_ui->user_pad_lg)
#define USER_RADIUS                 (g_ui->user_radius)
#define USER_BTN_RADIUS             (g_ui->user_btn_radius)
#define USER_BTN_PRIMARY_W          (g_ui->user_btn_primary_w)
#define USER_BTN_PRIMARY_H          (g_ui->user_btn_primary_h)
#define USER_BTN_DOOR_SZ            (g_ui->user_btn_door_sz)
#define USER_BTN_DOOR_ICON_SZ       (g_ui->user_btn_door_icon_sz)
#define USER_VALUE_LABEL_W          (g_ui->user_value_label_w)
#define USER_STATUS_DOT_SZ          (g_ui->user_status_dot_sz)
#define USER_HOME_DEVICE_W          (g_ui->user_home_device_w)
#define USER_HOME_DEVICE_H          (g_ui->user_home_device_h)
#define USER_HOME_CARD_W            (g_ui->user_home_card_w)
#define USER_HOME_CARD_TOP          (g_ui->user_home_card_top)
#define USER_METRIC_ICON_SZ         (g_ui->user_metric_icon_sz)
#define USER_BACK_BTN_W             (g_ui->user_back_btn_w)
#define USER_BACK_BTN_H             (g_ui->user_back_btn_h)
#define USER_SENSOR_STRIP_H         (g_ui->user_sensor_strip_h)
/** 全屏媒体页关闭/翻页圆钮直径（已解析 0 = 沿用 user_back_btn_h 的回退） */
int32_t user_media_btn_size(void);
/** 延时播放器播放/暂停钮直径（已解析 0 = 沿用 user_media_btn_size()*2 的回退） */
int32_t user_media_play_size(void);

#define USER_DLG_W                  (g_ui->user_dlg_w)
#define USER_DLG_PAD                (g_ui->user_dlg_pad)
#define USER_DLG_PAD_LG             (g_ui->user_dlg_pad_lg)
#define USER_DLG_TITLE_PAD_BOTTOM   (g_ui->user_dlg_title_pad_bottom)
#define USER_DLG_RADIUS             (g_ui->user_dlg_radius)
#define USER_DLG_BACKDROP_OPA       LV_OPA_70   /* 跟分辨率无关，固定 */
#define USER_DLG_BODY_MIN_H         (g_ui->user_dlg_body_min_h)
#define USER_DLG_FOOTER_BTN_W       (g_ui->user_dlg_footer_btn_w)
#define USER_DLG_FOOTER_BTN_H       (g_ui->user_dlg_footer_btn_h)
#define USER_DLG_TITLE_LEAD         (g_ui->user_dlg_title_lead)
#define USER_DLG_BODY_LEAD          (g_ui->user_dlg_body_lead)
#define USER_FILE_DLG_W             (g_ui->user_file_dlg_w)
#define USER_FILE_DLG_PREVIEW_W     (g_ui->user_file_dlg_preview_w)
#define USER_FILE_DLG_PREVIEW_H     (g_ui->user_file_dlg_preview_h)
#define USER_FILE_SEL_W             (g_ui->user_file_sel_w)
#define USER_FILE_SEL_H             (g_ui->user_file_sel_h)
#define USER_FILE_CARD_W            (g_ui->user_file_card_w)
#define USER_FILE_CARD_H            (g_ui->user_file_card_h)
#define USER_FILE_THUMB_H           (g_ui->user_file_thumb_h)
#define USER_SLIDE_W                (g_ui->user_slide_w)
#define USER_SLIDE_H                (g_ui->user_slide_h)
#define USER_SLIDE_KNOB_SZ          (g_ui->user_slide_knob_sz)
#define USER_DOOR_SLIDE_W           (g_ui->user_door_slide_w)
#define USER_DOOR_SLIDE_H           (g_ui->user_door_slide_h)
#define USER_DOOR_SLIDE_KNOB        (g_ui->user_door_slide_knob)
#define USER_SPINNER_SZ             (g_ui->user_spinner_sz)
#define USER_SPINNER_STROKE         (g_ui->user_spinner_stroke)
#define USER_PROGRESS_W             (g_ui->user_progress_w)
#define USER_PROGRESS_H             (g_ui->user_progress_h)
#define USER_PRINT_BAR_H            (g_ui->user_print_bar_h)
#define USER_PRINT_BAR_PAD_HOR      (g_ui->user_print_bar_pad_hor)
#define USER_PRINT_BAR_PAD_VER      (g_ui->user_print_bar_pad_ver)
#define USER_PRINT_BAR_GAP          (g_ui->user_print_bar_gap)
#define USER_PRINT_TERMINATE_W      (g_ui->user_print_terminate_w)
#define USER_PRINT_TERMINATE_H      (g_ui->user_print_terminate_h)

#define USER_PRINT_CARD_W           (g_ui->user_print_card_w)
#define USER_NOTICE_CARD_W          (g_ui->user_notice_card_w)
#define USER_PRINT_THUMB_SZ         (g_ui->user_print_thumb_sz)
#define USER_PRINT_CTL_BTN          (g_ui->user_print_ctl_btn)
#define USER_PRINT_PILL_W           (g_ui->user_print_pill_w)
#define USER_PRINT_BADGE_SZ         (g_ui->user_print_badge_sz)
#define USER_PRINT_PROG_H           (g_ui->user_print_prog_h)
#define USER_ENV_COL_W              (g_ui->user_env_col_w)

#define USER_HIST_TAB_H             (g_ui->user_hist_tab_h)
#define USER_HIST_CARD_H            (g_ui->user_hist_card_h)
#define USER_HIST_CHECK_SZ          (g_ui->user_hist_check_sz)
#define USER_HIST_PAD_L             (g_ui->user_hist_pad_l)
#define USER_HIST_GRID_GAP          (g_ui->user_hist_grid_gap)
#define USER_HIST_CARD_RADIUS       (g_ui->user_hist_card_radius)
#define USER_HIST_SELBAR_H          (g_ui->user_hist_selbar_h)
#define USER_HIST_DEL_BTN_W         (g_ui->user_hist_del_btn_w)
#define USER_HIST_DEL_BTN_H         (g_ui->user_hist_del_btn_h)

#define USER_LIGHT_RADIUS           (g_ui->user_light_radius)
#define USER_LIGHT_BTN_SZ           (g_ui->user_light_btn_sz)
#define USER_ACTION_BTN_SZ          (g_ui->user_action_btn_sz)
#define USER_HOME_CARD_H            (g_ui->user_home_card_h)
#define USER_HOME_GRID_W            (g_ui->user_home_grid_w)
#define USER_HOME_GREET_TOP         (g_ui->user_home_greet_top)
#define USER_HOME_DEV_IMG           (g_ui->user_home_dev_img)
#define USER_HOME_DEV_IMG_X         (g_ui->user_home_dev_img_x)
#define USER_HOME_DEV_IMG_Y         (g_ui->user_home_dev_img_y)
#define USER_HOME_PRINT_BTN_W       (g_ui->user_home_print_btn_w)
#define USER_HOME_SLIDE_W           (g_ui->user_home_slide_w)
#define USER_HOME_SLIDE_H           (g_ui->user_home_slide_h)
#define USER_HOME_EDGE_GAP          (g_ui->user_home_edge_gap)
#define USER_PANEL_W                (g_ui->user_panel_w)
#define USER_PANEL_ROW_H            (g_ui->user_panel_row_h)
#define USER_EMPTY_RING_SZ          (g_ui->user_empty_ring_sz)

/**
 * 初始化主题：根据当前 lv_display 实际分辨率挑选 profile，再初始化所有 lv_style。
 * 必须在 platform_init() 创建 lv_display 之后调用。
 */
void ui_theme_init(void);

/**
 * 强制使用某套 profile（必须在 ui_theme_init() 之前调用）。
 * profile_id 取 UI_RES_1920 / UI_RES_1280。
 * 传 0 表示取消强制，按自动逻辑选择。
 */
void ui_theme_force_profile(int8_t profile_id);

/** 当前生效的 profile id（调试/日志用） */
int8_t ui_theme_get_profile(void);

/* ---- 字体接口（运行时返回当前 profile 的字体） ---- */
const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_title(void);
const lv_font_t *user_font_value(void);
const lv_font_t *user_font_text(void);
/** 比 text 小一档（1280 档 16）：通知卡标题/副标题、右上环境条数值。 */
const lv_font_t *user_font_text_sm(void);
/** 比 text 大一档（1280 档 20）：稿的「一级标题」——设置页各处标题。 */
const lv_font_t *user_font_title(void);
const lv_font_t *user_font_caption(void);
/** 比 caption 小一档，给窄容器里塞不下的说明字用。 */
const lv_font_t *user_font_caption_sm(void);
/** 比 value 大一档（1280 档 40）：浅色稿 HOME 卡的空气质量分级大字。 */
const lv_font_t *user_font_display(void);
/** 与 value 同档（1280 档 32）：浅色稿的四宫格读数与问候语标题。 */
const lv_font_t *user_font_metric(void);
/** 浅色稿的正文（1280 档 24）：问候副标题、HOME 卡读数、PRINT 与开门滑块文案。 */
const lv_font_t *user_font_body(void);
/** 浅色档弹窗的标题（1280 档 28）。深色档弹窗不用它，见字段处说明。 */
const lv_font_t *user_font_dlg_title(void);
/** 浅色档弹窗页脚按钮的文字（1280 档 22）。深色档弹窗不用它，见字段处说明。 */
const lv_font_t *user_font_dlg_btn(void);
const lv_font_t *user_font_icon(void);
/** 中文字体（子集，编译进固件）。用于需要中文标签的页面，如调试测试页。 */
const lv_font_t *user_font_cjk(void);

/* ---- 预置样式 ---- */
lv_style_t *user_style_btn_primary(void);
lv_style_t *user_style_btn_ghost(void);

#ifdef __cplusplus
}
#endif

#endif
