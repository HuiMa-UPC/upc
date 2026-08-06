#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_device_bar.hpp"
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
    /* 5 个图标沿竖轴均匀分布；首尾再加内边距，使最上/最下留白略多于图标间距 */
    lv_obj_set_flex_align(menu_panel_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(menu_panel_, USER_PAD, 0);
    lv_obj_set_style_pad_bottom(menu_panel_, USER_PAD, 0);
    lv_obj_set_style_pad_hor(menu_panel_, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(menu_panel_, 0, 0);
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
     * 不显示该条的隐藏页会被收回 0，页面因此拿到整屏高度。 */
    lv_obj_set_style_pad_top(content_panel_, USER_TOPBAR_H, 0);
    lv_obj_set_style_border_width(content_panel_, 0, 0);
    lv_obj_set_style_radius(content_panel_, 0, 0);
    lv_obj_set_style_bg_opa(content_panel_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content_panel_, LV_OBJ_FLAG_SCROLLABLE);

    /* 常驻设备信息条建在 content_panel_ 之后，于是恒定压在所有页面之上——首页的
     * 摄像头整屏层也在它下面。它仍在 shell_ 内，所以打印流程弹窗那层全屏遮罩
     * （建在 screen_ 上的 g_overlay / lv_layer_top）会连它一起压暗，与页面表现一致。 */
    device_bar_ = ui_device_bar_create(shell_);

    /* 侧栏画在最后（浮于页面和设备信息条之上）。必须在 FLOATING 之后做——直接对
     * flex 子节点 move_foreground 会改变布局顺序，把侧栏挪到屏幕右侧。 */
    lv_obj_move_foreground(menu_panel_);
}

/* 创建左侧菜单按钮外壳（选中态主色背景、按压反馈），返回按钮对象 */
static lv_obj_t* make_menu_button(lv_obj_t* parent, void* page_raw,
                                  lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    /* 正方形按钮：白遮罩/主色高亮都是正方形（边长 = 侧栏可用宽度） */
    const int32_t nav_btn_sz = USER_SIDEBAR_W - 2 * USER_PAD_SM;
    lv_obj_set_size(btn, nav_btn_sz, nav_btn_sz);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, user_color_accent, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_color_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(btn, USER_NAV_ICON_PAD, 0);
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

void PageManager::register_page(PagePtr page, const lv_image_dsc_t* menu_icon_img) {
    PageEntry entry;
    entry.page = std::move(page);
    Page* raw = entry.page.get();

    lv_obj_t* btn = make_menu_button(menu_panel_, raw, menu_btn_event_cb);
    /* 选中态主色高亮与按压态白遮罩均由按钮自身整块绘制（见 make_menu_button），
     * 两者同形同位、大小一致。 */

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, menu_icon_img);
    lv_obj_center(img);
    /* 缩放到 USER_NAV_ICON_SZ（256 = 100%） */
    if (menu_icon_img && menu_icon_img->header.w > 0) {
        const int32_t zoom = 256 * USER_NAV_ICON_SZ / (int32_t)menu_icon_img->header.w;
        lv_image_set_scale(img, zoom);
    }
    /* A8 图标用 recolor 着色，默认未选灰 */
    lv_obj_set_style_image_recolor(img, user_color_icon_off, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    entry.menu_btn = btn;
    entry.menu_icon_img = img;
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
        /* 图片图标按选中态着色（符号图标已由按钮 text_color 处理） */
        if (entry.menu_icon_img) {
            lv_obj_set_style_image_recolor(entry.menu_icon_img,
                active ? user_color_accent : user_color_icon_off, 0);
        }
    }
}

void PageManager::apply_device_bar(const PageEntry& entry) {
    /* 有菜单按钮 = 侧栏那五个标签页 → 显示；隐藏页（选文件 / 调试 / 延时摄影）
     * 不显示。用这条规则而不是写死页名：以后新增隐藏页默认就不显示。
     * 延时摄影页另外还建在 lv_layer_top() 上整屏覆盖，本来也盖得住这条。 */
    const bool show = entry.menu_btn != nullptr;
    ui_device_bar_set_visible(show);
    lv_obj_set_style_pad_top(content_panel_, show ? USER_TOPBAR_H : 0, 0);
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
