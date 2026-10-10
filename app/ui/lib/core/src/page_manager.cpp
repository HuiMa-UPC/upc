#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_user_widgets.hpp"
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

    lv_obj_set_style_bg_grad_dir(shell_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(shell_, LV_OPA_COVER, 0);
    apply_shell_style();   /* 底色两档都在这里给，见 set_shell_light */

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
    lv_obj_set_style_radius(menu_panel_, 0, 0);
    lv_obj_set_scrollbar_mode(menu_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_SCROLLABLE);
    apply_sidebar_style();   /* 底色/描边/投影两档都在这里给，见 set_sidebar_video_mode */

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

/* 侧栏两档外观。浅色那档照稿 6073:72801：白 4% 的底几乎看不见，侧栏与页面的分界
 * 全靠右侧那道 #0C1C61 @4% / offset(2,0) / blur 6 的外投影划出来，所以投影不能省。
 * 深色那档是整屏视频态的退路，值沿用改版前的：bg_opa 90% 是实测出来的——叠上摄像头层
 * 那条 LV_OPA_70 底衬后，全白帧下图标底色约 #212121，与其余暗色页面的 #131212 接近；
 * 想让画面多透一点可降到 LV_OPA_80（约 #262626），再低未选中图标就要糊。 */
void PageManager::apply_sidebar_style() {
    if (!menu_panel_) {
        return;
    }
    if (sidebar_video_) {
        lv_obj_set_style_bg_color(menu_panel_, user_color_panel, 0);
        lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_90, 0);
        lv_obj_set_style_border_width(menu_panel_, 1, 0);
        lv_obj_set_style_border_color(menu_panel_, user_color_divider, 0);
        lv_obj_set_style_border_opa(menu_panel_, LV_OPA_50, 0);
        lv_obj_set_style_border_side(menu_panel_, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_shadow_width(menu_panel_, 8, 0);
        lv_obj_set_style_shadow_ofs_x(menu_panel_, 0, 0);
        lv_obj_set_style_shadow_color(menu_panel_, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(menu_panel_, LV_OPA_10, 0);
    } else {
        lv_obj_set_style_bg_color(menu_panel_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(menu_panel_, 10, 0);     /* 稿 4%，255 的 4% ≈ 10 */
        lv_obj_set_style_border_width(menu_panel_, 0, 0);
        lv_obj_set_style_shadow_width(menu_panel_, 6, 0);
        lv_obj_set_style_shadow_ofs_x(menu_panel_, 2, 0);
        lv_obj_set_style_shadow_color(menu_panel_, user_light_shadow, 0);
        lv_obj_set_style_shadow_opa(menu_panel_, 10, 0); /* 稿 4% */
    }
    /* 图标色跟着档位走，重新按当前高亮刷一遍 */
    update_menu_highlight(current_ ? current_->name() : std::string());
}

void PageManager::raise_sidebar() {
    if (menu_panel_) {
        /* menu_panel_ 早已是 FLOATING，这里只改 z 序不会动到 shell_ 的 flex 布局顺序
         * ——顺序反了的后果见 init() 里那段说明。 */
        lv_obj_move_foreground(menu_panel_);
    }
}

void PageManager::set_sidebar_video_mode(bool on) {
    if (sidebar_video_ == on) {
        return;
    }
    sidebar_video_ = on;
    apply_sidebar_style();
}

/* 整屏底色两档。页面根节点与 content_panel_ 都是透明的，底全由这一层给。
 *
 * 之所以是**一个开关而不是直接换成浅色**：浅色稿是一页一页改过来的，改完的页面
 * （首页 / 历史页 / 消息页 / 设置页）要浅底，控制页与隐藏页仍是深色内容——翻成浅底
 * 那些页的白字会当场看不见。名单在 apply_device_bar()。 */
void PageManager::apply_shell_style() {
    if (!shell_) {
        return;
    }
    lv_obj_set_style_bg_color(shell_, shell_light_ ? user_light_bg : user_color_bg, 0);
    /* Toast 早先也在这里跟着一起切深浅档；新稿的顶部 toast 只剩一档白卡（全屏视频页上
     * 同样清楚），不再跟壳的底色走。它只关心内容区左缘，见 apply_device_bar()。 */
}

void PageManager::set_shell_light(bool on) {
    if (shell_light_ == on) {
        return;
    }
    shell_light_ = on;
    apply_shell_style();
}

/* 创建左侧菜单按钮外壳，返回按钮对象。
 *
 * 背景分四档，靠 LVGL 的状态选择器叠出来：
 *   默认                          → 透明
 *   CHECKED                       → #333145 实色底板（稿 6073:72715 的 Nav_Home_Active）
 *   PRESSED                       → 白 20%，按的是未选中的标签
 *   CHECKED | PRESSED             → 主色蓝 20%，按的是已选中的标签
 * 组合态一定压过单态：候选样式要求状态位是对象当前状态的子集，再取状态值最大的那条，
 * 而 LV_STATE_CHECKED(1<<2) | LV_STATE_PRESSED(1<<7) = 132 > PRESSED 的 128。
 *
 * **CHECKED 那档必须显式给 bg_color + bg_opa，不能只是「不设」**：lv_btn_create 会带上
 * 默认主题，而主题给每个普通按钮的 checked 态都挂了 bg_color_secondary（副色 +
 * LV_OPA_COVER，见 lv_theme_default.c 的 apply 分支），副色由 lv_display.c 自动初始化
 * 主题时传的 LV_PALETTE_RED——不压住的话选中态会是一块不透明红底（已实机踩过）。
 * 早先是靠把 bg_opa 写成 LV_OPA_TRANSP 来压制的（那时稿上选中态没有底板），现在稿
 * 自带底板，局部给一个不透明的 #333145 同样盖得住，绕法可以退掉了。
 *
 * 圆角取 USER_NAV_ITEM_SZ/3（1280 档 96/3 = 32，正是稿值），跟着项边长走而不是借用
 * USER_RADIUS——后者是通用卡片圆角（1280 档才 14），和导航项没有关系。 */
static lv_obj_t* make_menu_button(lv_obj_t* parent, void* page_raw,
                                  lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    /* 正方形按钮：按压遮罩是正方形，边长直接取稿上导航项的边长（96@1280）；
     * 侧栏两侧那段内边距反过来由 USER_SIDEBAR_W 与它的差算出，见 PageManager::init。 */
    const int32_t nav_btn_sz = USER_NAV_ITEM_SZ;
    lv_obj_set_size(btn, nav_btn_sz, nav_btn_sz);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, user_light_nav_on, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_color_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, user_color_accent,
                              LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, USER_NAV_ITEM_SZ / 3, 0);
    lv_obj_set_style_pad_all(btn, USER_NAV_ICON_PAD, 0);
    /* 选中底板的外投影，**只给 LV_STATE_CHECKED**。稿导出的 SVG 是 feOffset dy=4 +
     * feGaussianBlur stdDeviation=6，即视觉模糊约 12（filter 区域也正好在 96 框外留了
     * 12 的边）。
     *
     * **别图省事挂在默认态上**：LVGL 决定画不画影子只看 shadow_width / shadow_opa，
     * 压根不查 bg_opa（见 lv_draw_rect.c 里 has_shadow 那段，bg_opa 走的是另一条独立的
     * has_fill）。挂默认态的话五枚全带影子。而且未选中那四枚底是透明的、bg_cover 为
     * false，lv_draw_sw_box_shadow.c 里那句 `simple = dsc->bg_cover` 就不成立，影子连
     * 按钮正中间也画出来——屏上是每枚未选中项底下压着一团完整的深色柔光，看着像浮起来。
     *
     * 五枚导出里只有 Nav_Home_Active 带这个 filter，其余四枚没有：那是导出差异不是设计
     * 意图，投影挂在「选中」那个变体上，而任一时刻只有一项选中。 */
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 12, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_ofs_y(btn, 4, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_color(btn, user_light_nav_on, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, page_raw);
    return btn;
}

void PageManager::register_page(PagePtr page, const char* menu_icon) {
    PageEntry entry;
    entry.page = std::move(page);
    Page* raw = entry.page.get();

    lv_obj_t* btn = make_menu_button(menu_panel_, raw, menu_btn_event_cb);
    lv_obj_set_style_text_font(btn, user_font_icon(), 0);
    /* 符号图标这条路只剩调试入口在用，不跟侧栏档位切换：未选中取浅色稿的灰，
     * 选中态在 #333145 底板上取白（与图片图标那条一致）。 */
    lv_obj_set_style_text_color(btn, user_light_icon_off, 0);
    lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, menu_icon ? menu_icon : raw->name().c_str());
    lv_obj_center(lbl);

    entry.menu_btn = btn;
    entries_.push_back(std::move(entry));
}

/* 浅色稿 6073:72801 的选中态是「#333145 实色底板 + 白色字形」，比早先那版简单得多：
 * 那时稿上选中态没有底板、只有一枚蓝渐变字形，而 A8 图标只存 alpha、recolor 只能上
 * 单色，于是每个按钮里建了两层互斥的图标——未选中露 A8 灰图标，选中露一块铺了渐变的
 * 方块、用同一份 A8 当 bitmap_mask_src 裁成字形。那套连带三个 LVGL 的坑（蒙版按原
 * 尺寸居中贴不缩放、transform_pivot 默认在左上角、窄侧栏档位要放开 OVERFLOW_VISIBLE）
 * 已随稿一并退役，现在一层图标 + 换 recolor 颜色就够。 */
void PageManager::register_page(PagePtr page, const lv_image_dsc_t* const* menu_icon_set,
                                const lv_image_dsc_t* const* badge_icon_set) {
    PageEntry entry;
    entry.page = std::move(page);
    Page* raw = entry.page.get();

    lv_obj_t* btn = make_menu_button(menu_panel_, raw, menu_btn_event_cb);
    /* 底板与按压反馈都由按钮自身整块绘制，见 make_menu_button。 */

    /* 按当前档位挑等宽的那份，正常情况下 src_sz == USER_NAV_ICON_SZ、下面那段缩放
     * 整个跳过。留着缩放是兜底：主题改了尺寸却没同步改生成脚本的 NAV_SIZES 时，
     * 形不崩、只是糊。 */
    const lv_image_dsc_t* src = ui_user_pick_icon(menu_icon_set, USER_NAV_ICON_SZ);
    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, src);
    lv_obj_center(img);
    if (src && src->header.w > 0) {
        const int32_t src_sz = (int32_t)src->header.w;
        if (src_sz != USER_NAV_ICON_SZ) {
            /* pivot 默认在左上角，不挪到中心会朝左上缩 */
            lv_image_set_pivot(img, src_sz / 2, src_sz / 2);
            lv_image_set_scale(img, 256 * USER_NAV_ICON_SZ / src_sz);
        }
    }
    /* 具体颜色随选中态与侧栏档位变，统一在 update_menu_highlight 里给。 */
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    entry.menu_btn = btn;
    entry.menu_icon_img = img;
    entry.menu_icon_set = menu_icon_set;
    entry.menu_icon_badge_set = badge_icon_set;
    entries_.push_back(std::move(entry));
}

void PageManager::register_hidden_page(PagePtr page, std::string owner_menu) {
    PageEntry entry;
    entry.page = std::move(page);
    entry.owner_menu = std::move(owner_menu);
    /* 无菜单按钮：menu_btn / menu_icon_img 保持 nullptr */
    entries_.push_back(std::move(entry));
}

void PageManager::set_hidden_page_owner(const std::string& page_name,
                                        std::string owner_menu) {
    PageEntry* e = find_entry(page_name);
    if (!e || e->menu_btn) {
        return;
    }
    e->owner_menu = std::move(owner_menu);
    if (current_ == e->page.get()) {
        update_menu_highlight(page_name);
    }
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
    /* 有动态目标解析器则切到解析结果（如首页 / 监控页图标→该分区当前视图），否则切到本页 */
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
        /* 图片图标只有一层，靠 recolor 换色。选中态恒为白（底板是深色 #333145，
         * 两档都一样）；未选中态跟着侧栏档位走：浅色档 #9E9E9E（稿值），整屏视频档
         * 退回 #555555——那一档底是深的，稿值那个亮灰反而会和白色选中态糊在一起。
         * （符号图标那条路仍由按钮的 text_color 处理，见另一个 register_page 重载） */
        if (entry.menu_icon_img) {
            const lv_color_t c = active ? lv_color_white()
                                        : (sidebar_video_ ? user_color_icon_off
                                                          : user_light_icon_off);
            lv_obj_set_style_image_recolor(entry.menu_icon_img, c, 0);
        }
    }
}

void PageManager::apply_device_bar(const PageEntry& entry) {
    const std::string& page = entry.page->name();
    /* 有菜单按钮 = 侧栏那五个标签页 → 显示；隐藏页（选文件 / 调试 / 延时摄影）
     * 不显示。用这条规则而不是写死页名：以后新增隐藏页默认就不显示。
     * 延时摄影页另外还建在 lv_layer_top() 上整屏覆盖，本来也盖得住这条。
     *
     * **历史页 / 消息页 / 设置页是例外**：三页的浅色稿（6122 一族 / 6230:89616 一族 /
     * 6403·6457 一族）顶部都没画这条，页面从屏顶起就是自己的内容（各自给顶内距）。
     * 消息页的稿里那条设备条其实还躺在图层树上（6101:14137），但被不透明的顶栏整条
     * 盖住了，等价于不画。代价是在这三页看不到设备名与联机状态，这是照稿的取舍。
     * 不显示时下面那句会把 pad_top 收回 0，整屏高度还给页面。
     *
     * 设置页**必须**排除，不是可选的美化：左栏 8 项（解锁 Debug 后）高
     * 48 + 8×72 + 7×10 = 694，被 USER_CONTENT_TOP 下推 112 之后会顶到屏底；
     * 右栏标题行盒顶也是按屏顶 y=56 摆的。
     *
     * 监控页同样排除：稿 6338:27240 一族里摄像头画面从 y=0 起、右栏 Tab 行顶在
     * y=32，整屏没有给这条带子留位置。 */
    const bool show = entry.menu_btn != nullptr &&
                      page != "user_history" && page != "user_message" &&
                      page != "user_settings" && page != "user_monitor";
    ui_device_bar_set_visible(show);
    lv_obj_set_style_pad_top(content_panel_, show ? USER_CONTENT_TOP : 0, 0);
    /* 通知三态（toast / 铃铛 / 面板）**只在首页显示**。
     *
     * 早先的规则是「侧栏那五个标签页显示，另外排除消息页、历史页、设置页」，实际落到
     * 首页 + 控制页两处。浅色稿把右上角这块做成了首页布局的一部分（铃铛在稿上就画在
     * 首页那一帧里），所以收窄到首页；控制页的新消息与其余页面一样，靠侧栏消息图标那
     * 颗红点提示。
     *
     * 被排除的几页各有各的理由，收窄之后一并覆盖：消息页自己（消息都摊在页面上了，
     * 再在右上角浮两张重复的没意义）、历史页与设置页（右上角已经有 Storage 容量条和
     * Wi-Fi 的 On/Off 开关，浮层压上去会连触摸一起吃掉）。 */
    ui_notice_stack_set_visible(show && page == "user_home");
    /* 让位量是打印态/监控态首页专属的（要让开右上角那块环境区），换页一律清零；
     * 首页自己会在 on_enter 里按当前状态再让一次。 */
    ui_notice_stack_set_top_offset(0);
    /* 侧栏的深色档同理，只在首页那两个整屏视频态用。换页一律回浅色——打印在后台继续
     * 跑时用户可能切到别的页，那些页是浅底/暗底但都不是视频，深色侧栏没有必要。
     * 首页在 on_enter 里按当前状态再置一次。 */
    set_sidebar_video_mode(false);
    /* 整屏底色：**已按浅色稿改过的页面**才给浅底。控制页还是深色内容，翻成浅底
     * 白字会当场看不见；那一页改稿时把页名加进来即可。
     * 隐藏页里延时摄影 / 相册全屏仍走深色；调试页已翻浅。 */
    set_shell_light(page == "user_home" || page == "user_monitor" ||
                    page == "user_history" || page == "user_message" ||
                    page == "user_settings" || page == "user_files" ||
                    page == "user_debug");
    /* 顶部 toast 在内容区里水平居中（稿 6357:39342：1280 档中心 x=704，即侧栏右侧那一段
     * 的中线）。延时摄影与相册大图建在 lv_layer_top() 上整屏覆盖、看不到侧栏，按整屏居中。 */
    const bool full_screen = (page == "user_timelapse" || page == "user_photo");
    ui_toast_set_content_left(full_screen ? 0 : USER_SIDEBAR_W);
}

void PageManager::set_menu_badge(const std::string& page_name, bool on) {
    PageEntry* entry = find_entry(page_name);
    if (!entry || !entry->menu_btn) {
        return;
    }
    if (on && !entry->menu_badge) {
        /* 稿里红点是烤进图标 SVG 的，那样没法按未读数开关，所以另叠一个小圆。
         * FLOATING：菜单按钮里图标是 center 摆的，红点不能占布局。
         *
         * 稿 6255:95797 给的是确值：96 的导航项里 circle(60,36) r=4，即直径 8、
         * 圆心从项心 (48,48) 往右上各让 12。颜色 #FE6868 与首页铃铛那颗同值
         * （那边早就是稿值了，这边原先填的是深色档的 ui_color_danger #F85149）。 */
        const int32_t sz  = USER_NAV_ITEM_SZ * 8 / 96;
        const int32_t off = USER_NAV_ITEM_SZ * 12 / 96;
        lv_obj_t* dot = lv_obj_create(entry->menu_btn);
        lv_obj_remove_style_all(dot);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(dot, sz, sz);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, user_light_notice_dot, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_CENTER, off, -off);
        entry->menu_badge = dot;
    }
    if (entry->menu_badge) {
        if (on) {
            lv_obj_remove_flag(entry->menu_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(entry->menu_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* 有带红点变体的话连图标一起换：那一版把右上角咬掉了一块给红点让位，
     * 不换的话红点会压在原轮廓上（同 ui_notice_stack.cpp 的 build_bell）。 */
    if (entry->menu_icon_img && entry->menu_icon_badge_set) {
        const lv_image_dsc_t* const* set =
            on ? entry->menu_icon_badge_set : entry->menu_icon_set;
        lv_image_set_src(entry->menu_icon_img,
                         ui_user_pick_icon(set, USER_NAV_ICON_SZ));
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
        ui_toast_show("Printing in progress", UI_TOAST_INFORM, 2000);
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
