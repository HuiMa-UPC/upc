#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_device_bar.hpp"
#include "ui_notice_stack.hpp"
#include "control.hpp"
#include <algorithm>

namespace ui {

PageManager& PageManager::instance() {
    static PageManager inst;
    return inst;
}

void PageManager::init(lv_obj_t* screen) {
    screen_ = screen;
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_set_style_border_width(screen_, 0, 0);
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    shell_ = lv_obj_create(screen_);
    lv_obj_set_size(shell_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(shell_, 0, 0);
    lv_obj_set_style_border_width(shell_, 0, 0);
    lv_obj_set_style_radius(shell_, 0, 0);
    lv_obj_clear_flag(shell_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(shell_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shell_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_set_style_bg_color(shell_, user_color_bg, 0);
    lv_obj_set_style_bg_grad_dir(shell_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(shell_, LV_OPA_COVER, 0);

    /* 侧栏是浮层而不是 flex 的第一格：首页的摄像头层要延伸到屏幕左边缘（整屏预览），
     * 侧栏浮在画面之上。content_panel_ 用 pad_left 补回侧栏宽度，所以其余页面的
     * 内容区几何与改动前逐像素一致，无需逐页调整。 */
    menu_panel_ = lv_obj_create(shell_);
    lv_obj_add_flag(menu_panel_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(menu_panel_, 0, 0);
    lv_obj_set_width(menu_panel_, USER_SIDEBAR_W);
    lv_obj_set_height(menu_panel_, lv_pct(100));
    lv_obj_set_flex_flow(menu_panel_, LV_FLEX_FLOW_COLUMN);
    /* 稿 5339:27264 的 Home bar 是「竖向居中 + 固定 USER_NAV_GAP 间距」，不是按屏高
     * 均分：1280 档 5 枚 96 的项加 4 道 16 的缝共 544，居中落在 800 高的屏上。
     * pad_top 与 pad_bottom 取同一个值，所以 CENTER 在内容区里居中等价于在整条侧栏里
     * 居中，这两个 padding 只作最小留白（项撑不满侧栏时不起作用）。 */
    lv_obj_set_flex_align(menu_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(menu_panel_, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(menu_panel_, USER_PAD, 0);
    /* 左右内边距由「侧栏宽 − 项边长」反推：稿上这段是 16@1280（128 = 16+96+16）。 */
    lv_obj_set_style_pad_hor(menu_panel_, (USER_SIDEBAR_W - USER_NAV_ITEM_SZ) / 2, 0);
    lv_obj_set_style_pad_row(menu_panel_, USER_NAV_GAP, 0);
    lv_obj_set_style_bg_color(menu_panel_, user_color_panel, 0);
    /* 摄像头整屏时侧栏背后是视频，图标底色由当前帧决定：亮帧下合成色会盖过
     * user_color_icon_off（#555555），未选中图标糊掉。面板自己吃掉大部分不透明度，
     * 暗色就只落在侧栏这 USER_SIDEBAR_W 内，不额外压暗画面。
     * 其余页面背后是 user_color_bg（#0a0909，近纯黑），提高不透明度肉眼无差别。
     * 取值依据：叠上摄像头层那条 LV_OPA_70 底衬后，全白帧下图标底色约 #212121，
     * 与其余页面的 #131212 接近。想让画面多透一点可降到 LV_OPA_80（约 #262626）。 */
    lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(menu_panel_, 1, 0);
    lv_obj_set_style_border_color(menu_panel_, user_color_divider, 0);
    lv_obj_set_style_border_opa(menu_panel_, LV_OPA_50, 0);
    lv_obj_set_style_border_side(menu_panel_, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(menu_panel_, 0, 0);
    /* 侧栏保留轻微层次，避免与内容区之间出现明显黑色竖缝。 */
    lv_obj_set_style_shadow_width(menu_panel_, 8, 0);
    lv_obj_set_style_shadow_color(menu_panel_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(menu_panel_, LV_OPA_10, 0);
    lv_obj_set_scrollbar_mode(menu_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_SCROLLABLE);

    content_panel_ = lv_obj_create(shell_);
    lv_obj_set_width(content_panel_, 0);
    lv_obj_set_height(content_panel_, lv_pct(100));
    lv_obj_set_flex_grow(content_panel_, 1);
    lv_obj_set_style_pad_all(content_panel_, 0, 0);
    /* 侧栏已改浮层，content_panel_ 会铺满整屏；用 pad_left 让出侧栏，
     * 页面按 LV_PCT(100) 建出来的内容区因此与改动前一致。想做整屏出血的页面
     * （目前只有首页的摄像头层）自行用负偏移跨过这段 padding。 */
    lv_obj_set_style_pad_left(content_panel_, USER_SIDEBAR_W, 0);
    /* 顶部同理让出常驻设备信息条；这一格由 apply_device_bar() 按页开关，
     * 不显示该条的隐藏页会被收回 0，页面因此拿到整屏高度。
     * 用 USER_CONTENT_TOP 而不是设备条高：稿上内容顶比条高小一截（112 对 128），
     * 两者已拆成独立字段，见 ui_theme.cpp 头部说明。 */
    lv_obj_set_style_pad_top(content_panel_, USER_CONTENT_TOP, 0);
    lv_obj_set_style_border_width(content_panel_, 0, 0);
    lv_obj_set_style_radius(content_panel_, 0, 0);
    lv_obj_set_style_bg_opa(content_panel_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content_panel_, LV_OBJ_FLAG_SCROLLABLE);

    /* 常驻设备信息条建在 content_panel_ 之后，于是恒定压在所有页面之上——首页的
     * 摄像头整屏层也在它下面。它仍在 shell_ 内，所以打印流程弹窗那层全屏遮罩
     * （建在 screen_ 上的 g_overlay / lv_layer_top）会连它一起压暗，与页面表现一致。 */
    device_bar_ = ui_device_bar_create(shell_);

    /* 左下通知浮层，与设备信息条同级：压在所有页面之上，又在弹窗那层
     * （lv_layer_top）之下。显隐规则见 apply_device_bar()。 */
    notice_stack_ = ui_notice_stack_create(shell_);

    /* 侧栏画在最后（浮于页面和设备信息条之上）。必须在 FLOATING 之后做——直接对
     * flex 子节点 move_foreground 会改变布局顺序，把侧栏挪到屏幕右侧。 */
    lv_obj_move_foreground(menu_panel_);
}

/* 创建左侧菜单按钮外壳（仅按压反馈，无常显底板），返回按钮对象。
 *
 * 背景分四档，靠 LVGL 的状态选择器叠出来：
 *   默认                          → 透明（稿 2235:8697 的选中态只有字形，没有底板）
 *   CHECKED                       → 透明，见下
 *   PRESSED                       → 白 20%，按的是未选中的标签
 *   CHECKED | PRESSED             → 主色蓝 20%，按的是已选中的标签
 * 组合态一定压过单态：候选样式要求状态位是对象当前状态的子集，再取状态值最大的那条，
 * 而 LV_STATE_CHECKED(1<<2) | LV_STATE_PRESSED(1<<7) = 132 > PRESSED 的 128。
 *
 * **CHECKED 那档必须显式写成 LV_OPA_TRANSP，不能只是「不设」**：lv_btn_create 会带上
 * 默认主题，而主题给每个普通按钮的 checked 态都挂了 bg_color_secondary（副色 +
 * LV_OPA_COVER，见 lv_theme_default.c 的 apply 分支），副色由 lv_display.c 自动初始化
 * 主题时传的 LV_PALETTE_RED——不压住的话选中态会是一块不透明红底（已实机踩过）。
 * 局部样式优先于主题样式，所以只要局部把 bg_opa 设成透明就盖住了；bg_color 不用管，
 * 底不画出来颜色无所谓。反过来也别给它设可见的背景色，那就成了稿上没有的常显底板。 */
static lv_obj_t* make_menu_button(lv_obj_t* parent, void* page_raw,
                                  lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    /* 正方形按钮：按压遮罩是正方形，边长直接取稿上导航项的边长（96@1280）；
     * 侧栏两侧那段内边距反过来由 USER_SIDEBAR_W 与它的差算出，见 PageManager::init。 */
    const int32_t nav_btn_sz = USER_NAV_ITEM_SZ;
    lv_obj_set_size(btn, nav_btn_sz, nav_btn_sz);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_color_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, user_color_accent,
                              LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(btn, USER_NAV_ICON_PAD, 0);
    /* 选中态那层渐变按蒙版原尺寸（64×64）建，在窄侧栏的档位上比按钮还大（1024 档
     * 按钮 58），不放开溢出会先被裁掉再缩放，字形四角就缺了。1280 档按钮已加到 96、
     * 不再溢出，但这个标志要留着给其余档位。见 register_page 的说明。 */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, page_raw);
    return btn;
}

void PageManager::register_page(PagePtr page, const char* menu_icon) {
    PageEntry entry;
    entry.page = std::move(page);
    Page* raw = entry.page.get();

    lv_obj_t* btn = make_menu_button(menu_panel_, raw, menu_btn_event_cb);
    lv_obj_set_style_text_font(btn, user_font_icon(), 0);
    lv_obj_set_style_text_color(btn, user_color_icon_off, 0);
    lv_obj_set_style_text_color(btn, user_color_accent, LV_STATE_CHECKED);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, menu_icon ? menu_icon : raw->name().c_str());
    lv_obj_center(lbl);

    entry.menu_btn = btn;
    entries_.push_back(std::move(entry));
}

/* 选中态字形的蓝色对角渐变。方向取左上→右下，与 ui_user_make_primary_pill_btn /
 * ui_slide_unlock 近似稿上 139° 的做法一致。
 * bg_grad 只存指针，故渐变描述符必须持久——五个图标共用这一份。
 *
 * **色值比稿宽**：稿 2235:8699 是 #4054FD → #4D80EE，两色只差绿通道 44（红 +13、蓝 −15），
 * 亮度差约 28/255，在 54px 的字形上肉眼就是一片纯蓝、看不出深浅，所以起点压暗、终点提亮
 * 到 #3348E8 → #6699F5（亮度差约 64）。同一处理见 ui_slide_unlock 的红条：注释里的稿值
 * 是 #FF2424 → #FF4B4B，实取 #D00A0A → #FF8484。**别照稿值改回去**，那等于关掉渐变。
 *
 * 停靠点 {45, 198} 而不是 {0, 255}：渐变铺满整个 64×64 方块，而字形只占 x/y 的 6..57，
 * 沿对角线是整条色带的 9%~89%，用 {0,255} 会把两端各浪费掉一截。稿上的 11%/86% 本就是
 * 相对字形自身算的（渐变挂在字形节点上，不是带留白的外框），换算到本方块即这两个值。 */
static const lv_grad_dsc_t& nav_active_grad() {
    static lv_grad_dsc_t s_grad;
    static bool s_ready = false;
    if (!s_ready) {
        const lv_color_t cols[2] = { lv_color_hex(0x3348E8), lv_color_hex(0x6699F5) };
        const lv_opa_t   opa[2]  = { LV_OPA_COVER, LV_OPA_COVER };
        const uint8_t    frac[2] = { 45, 198 };
        lv_grad_init_stops(&s_grad, cols, opa, frac, 2);
        lv_grad_linear_init(&s_grad, LV_GRAD_LEFT, LV_GRAD_TOP,
                            LV_GRAD_RIGHT, LV_GRAD_BOTTOM, LV_GRAD_EXTEND_PAD);
        s_ready = true;
    }
    return s_grad;
}

void PageManager::register_page(PagePtr page, const lv_image_dsc_t* menu_icon_img) {
    PageEntry entry;
    entry.page = std::move(page);
    Page* raw = entry.page.get();

    lv_obj_t* btn = make_menu_button(menu_panel_, raw, menu_btn_event_cb);
    /* 按压反馈（未选中白 20% / 已选中主色 20%）由按钮自身整块绘制，见 make_menu_button。 */

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, menu_icon_img);
    lv_obj_center(img);
    /* 缩放到 USER_NAV_ICON_SZ（256 = 100%） */
    if (menu_icon_img && menu_icon_img->header.w > 0) {
        const int32_t zoom = 256 * USER_NAV_ICON_SZ / (int32_t)menu_icon_img->header.w;
        lv_image_set_scale(img, zoom);
    }
    /* A8 图标用 recolor 着色，恒为未选灰——选中态改由下面那层渐变负责，
     * 因为 recolor 只能上单色，画不出稿里的渐变。 */
    lv_obj_set_style_image_recolor(img, user_color_icon_off, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    /* 选中态：一块铺了蓝渐变的方块，再用同一份 A8 图标当位图蒙版裁成字形。
     * 两个尺寸约束都来自 LVGL 的实现（lv_draw_sw_img.c 的 apply_mask）：
     *   - 蒙版按原尺寸居中贴、**不缩放**，所以方块必须与图头等大（这里 64×64），
     *     否则字形会被裁或四周留空；
     *   - 于是缩到 USER_NAV_ICON_SZ 只能靠 transform_scale，且 pivot 默认在左上角，
     *     必须挪到中心，不然会朝左上角缩。
     * 与 img 互斥显示，切换见 update_menu_highlight。 */
    lv_obj_t* grad = nullptr;
    if (menu_icon_img && menu_icon_img->header.w > 0) {
        const int32_t mask_sz = (int32_t)menu_icon_img->header.w;
        grad = lv_obj_create(btn);
        lv_obj_remove_style_all(grad);
        lv_obj_set_size(grad, mask_sz, (int32_t)menu_icon_img->header.h);
        lv_obj_center(grad);
        lv_obj_set_style_bg_color(grad, lv_color_hex(0x3348E8), 0);   /* 渐变起点，兜底用 */
        lv_obj_set_style_bg_grad(grad, &nav_active_grad(), 0);
        lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
        lv_obj_set_style_bitmap_mask_src(grad, menu_icon_img, 0);
        lv_obj_set_style_transform_pivot_x(grad, mask_sz / 2, 0);
        lv_obj_set_style_transform_pivot_y(grad, mask_sz / 2, 0);
        lv_obj_set_style_transform_scale(grad, 256 * USER_NAV_ICON_SZ / mask_sz, 0);
        lv_obj_remove_flag(grad, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(grad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(grad, LV_OBJ_FLAG_HIDDEN);
    }

    entry.menu_btn = btn;
    entry.menu_icon_img = img;
    entry.menu_icon_grad = grad;
    entries_.push_back(std::move(entry));
}

void PageManager::register_hidden_page(PagePtr page, std::string owner_menu) {
    PageEntry entry;
    entry.page = std::move(page);
    entry.owner_menu = std::move(owner_menu);
    /* 无菜单按钮：menu_btn / menu_icon_img 保持 nullptr */
    entries_.push_back(std::move(entry));
}

void PageManager::set_menu_target_resolver(const std::string& page_name,
                                           std::function<std::string()> resolver) {
    if (PageEntry* e = find_entry(page_name)) {
        e->target_resolver = std::move(resolver);
    }
}

void PageManager::menu_btn_event_cb(lv_event_t* e) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(e));
    if (!page) {
        return;
    }
    PageManager& mgr = PageManager::instance();
    /* 有动态目标解析器则切到解析结果（如首页图标→首页区当前视图），否则切到本页 */
    PageEntry* entry = mgr.find_entry(page->name());
    const std::string target = (entry && entry->target_resolver)
                               ? entry->target_resolver() : page->name();
    mgr.switch_to(target);
}

void PageManager::update_menu_highlight(const std::string& active_name) {
    /* 激活页若是无按钮的隐藏页且声明了归属菜单页，则改为高亮归属页的按钮 */
    std::string highlight = active_name;
    if (PageEntry* act = find_entry(active_name)) {
        if (!act->menu_btn && !act->owner_menu.empty()) {
            highlight = act->owner_menu;
        }
    }
    for (auto& entry : entries_) {
        if (!entry.menu_btn) {
            continue;
        }
        const bool active = entry.page->name() == highlight;
        if (active) {
            lv_obj_add_state(entry.menu_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(entry.menu_btn, LV_STATE_CHECKED);
        }
        /* 图片图标：未选中露 A8 灰图标，选中露渐变层，两者互斥。
         * （符号图标那条路仍由按钮的 text_color 处理，见另一个 register_page 重载） */
        if (entry.menu_icon_grad) {
            if (active) {
                lv_obj_remove_flag(entry.menu_icon_grad, LV_OBJ_FLAG_HIDDEN);
                if (entry.menu_icon_img) lv_obj_add_flag(entry.menu_icon_img, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(entry.menu_icon_grad, LV_OBJ_FLAG_HIDDEN);
                if (entry.menu_icon_img) lv_obj_remove_flag(entry.menu_icon_img, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void PageManager::apply_device_bar(const PageEntry& entry) {
    /* 有菜单按钮 = 侧栏那五个标签页 → 显示；隐藏页（选文件 / 调试 / 延时摄影）
     * 不显示。用这条规则而不是写死页名：以后新增隐藏页默认就不显示。
     * 延时摄影页另外还建在 lv_layer_top() 上整屏覆盖，本来也盖得住这条。 */
    const bool show = entry.menu_btn != nullptr;
    ui_device_bar_set_visible(show);
    lv_obj_set_style_pad_top(content_panel_, show ? USER_CONTENT_TOP : 0, 0);
    /* 通知浮层同一条规则，另外排除三页：
     *   - 消息页自己：消息都摊在页面上了，再在右上角浮两张重复的没意义；
     *   - 历史页 / 设置页：这两页的右上角已经有常驻内容（Storage 容量条、Wi-Fi 的
     *     On/Off 开关），浮层压上去会挡住它们——开关还可点，连触摸一起吃掉。这两页的
     *     新消息只靠侧栏消息图标的红点提示。
     * 试过降级成单行细条，但细条底是半透明的，与 Storage 条叠印两层字都读不清；往下
     * 移又会压到 Manage 按钮和那个开关，所以直接不显示。 */
    const std::string& page = entry.page->name();
    ui_notice_stack_set_visible(show && page != "user_message"
                                     && page != "user_history"
                                     && page != "user_settings");
    /* 让位量是打印态/监控态首页专属的（要让开右上角那块环境区），换页一律清零；
     * 首页自己会在 on_enter 里按当前状态再让一次。 */
    ui_notice_stack_set_top_offset(0);
}

void PageManager::set_menu_badge(const std::string& page_name, bool on) {
    PageEntry* entry = find_entry(page_name);
    if (!entry || !entry->menu_btn) {
        return;
    }
    if (on && !entry->menu_badge) {
        /* 稿里红点是烤进图标 SVG 的，那样没法按未读数开关，所以另叠一个小圆。
         * FLOATING：菜单按钮里图标是 center 摆的，红点不能占布局。 */
        const int32_t sz = USER_STATUS_DOT_SZ;
        lv_obj_t* dot = lv_obj_create(entry->menu_btn);
        lv_obj_remove_style_all(dot);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(dot, sz, sz);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, ui_color_danger, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 0);
        entry->menu_badge = dot;
    }
    if (entry->menu_badge) {
        if (on) {
            lv_obj_remove_flag(entry->menu_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(entry->menu_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

PageManager::PageEntry* PageManager::find_entry(const std::string& name) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [&name](const PageEntry& e) { return e.page->name() == name; });
    return it != entries_.end() ? &(*it) : nullptr;
}

void PageManager::switch_to(const std::string& name) {
    PageEntry* entry = find_entry(name);
    if (!entry) {
        return;
    }

    /* 打印中放开导航（可切 history/control/settings/message/home），
     * 仅禁止再次进入"选文件"页，避免打印中重复触发开始打印流程。
     * （control_start() 另有 g_state!=IDLE 防重入作为双保险。） */
    if (control_is_printing() && name == "user_files") {
        ui_toast_show("Printing in progress", user_color_accent, 2000);
        return;
    }

    if (current_ && current_->name() == name) {
        return;
    }

    if (current_ && current_ != entry->page.get()) {
        current_->on_exit();
    }

    /* 必须在 on_create 之前：页面首帧布局要看到最终的 pad_top，
     * 首页更是按它算根节点的纵向负偏移。 */
    apply_device_bar(*entry);

    if (!entry->created) {
        entry->page->on_create(content_panel_);
        entry->created = true;
    }

    current_ = entry->page.get();
    current_->on_enter();
    update_menu_highlight(name);
}

} // namespace ui
