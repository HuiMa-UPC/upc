#include "page_manager.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
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

    menu_panel_ = lv_obj_create(shell_);
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
    lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_40, 0);
    lv_obj_set_style_border_width(menu_panel_, 1, 0);
    lv_obj_set_style_border_color(menu_panel_, user_color_divider, 0);
    lv_obj_set_style_border_opa(menu_panel_, LV_OPA_50, 0);
    lv_obj_set_style_border_side(menu_panel_, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(menu_panel_, 0, 0);
    lv_obj_set_style_shadow_width(menu_panel_, 24, 0);
    lv_obj_set_style_shadow_color(menu_panel_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(menu_panel_, LV_OPA_30, 0);
    lv_obj_set_scrollbar_mode(menu_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(menu_panel_, LV_OBJ_FLAG_SCROLLABLE);

    content_panel_ = lv_obj_create(shell_);
    lv_obj_set_width(content_panel_, 0);
    lv_obj_set_height(content_panel_, lv_pct(100));
    lv_obj_set_flex_grow(content_panel_, 1);
    lv_obj_set_style_pad_all(content_panel_, 0, 0);
    lv_obj_set_style_border_width(content_panel_, 0, 0);
    lv_obj_set_style_radius(content_panel_, 0, 0);
    lv_obj_set_style_bg_opa(content_panel_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(content_panel_, LV_OBJ_FLAG_SCROLLABLE);
}

/* 创建左侧菜单按钮外壳（选中态橙色背景、按压反馈），返回按钮对象 */
static lv_obj_t* make_menu_button(lv_obj_t* parent, void* page_raw,
                                  lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    /* 正方形按钮：白遮罩/橙高亮都是正方形（边长 = 侧栏可用宽度） */
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
    /* 选中态橙高亮与按压态白遮罩均由按钮自身整块绘制（见 make_menu_button），
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

    if (control_is_printing() && name != "user_home") {
        ui_toast_show("Printing in progress", user_color_accent, 2000);
        return;
    }

    if (current_ && current_->name() == name) {
        return;
    }

    if (current_ && current_ != entry->page.get()) {
        current_->on_exit();
    }

    if (!entry->created) {
        entry->page->on_create(content_panel_);
        entry->created = true;
    }

    current_ = entry->page.get();
    current_->on_enter();
    update_menu_highlight(name);
}

} // namespace ui
