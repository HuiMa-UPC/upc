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
    lv_obj_set_style_bg_grad_color(shell_, user_color_panel, 0);
    lv_obj_set_style_bg_grad_dir(shell_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(shell_, LV_OPA_COVER, 0);

    menu_panel_ = lv_obj_create(shell_);
    lv_obj_set_width(menu_panel_, USER_SIDEBAR_W);
    lv_obj_set_height(menu_panel_, lv_pct(100));
    lv_obj_set_flex_flow(menu_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(menu_panel_, USER_PAD_LG, 0);
    lv_obj_set_style_pad_bottom(menu_panel_, USER_PAD_LG, 0);
    lv_obj_set_style_pad_hor(menu_panel_, USER_PAD_SM, 0);
    lv_obj_set_style_pad_row(menu_panel_, USER_PAD_SM, 0);
    lv_obj_set_style_bg_color(menu_panel_, user_color_panel, 0);
    lv_obj_set_style_bg_opa(menu_panel_, LV_OPA_COVER, 0);
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
    lv_obj_set_style_bg_color(content_panel_, user_color_bg, 0);
    lv_obj_set_style_bg_opa(content_panel_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content_panel_, LV_OBJ_FLAG_SCROLLABLE);
}

void PageManager::register_page(PagePtr page, const char* menu_icon) {
    PageEntry entry;
    entry.page = std::move(page);
    Page* raw = entry.page.get();

    lv_obj_t* btn = lv_btn_create(menu_panel_);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, USER_NAV_ITEM_H);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, user_color_accent, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, user_color_text, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, USER_RADIUS, 0);
    lv_obj_set_style_pad_all(btn, USER_NAV_ICON_PAD, 0);
    lv_obj_set_style_text_font(btn, user_font_icon(), 0);
    lv_obj_set_style_text_color(btn, user_color_icon_off, 0);
    lv_obj_set_style_text_color(btn, user_color_accent, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn, menu_btn_event_cb, LV_EVENT_CLICKED, raw);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, menu_icon ? menu_icon : raw->name().c_str());
    lv_obj_center(lbl);

    entry.menu_btn = btn;
    entries_.push_back(std::move(entry));
}

void PageManager::menu_btn_event_cb(lv_event_t* e) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(e));
    if (page) {
        PageManager::instance().switch_to(page->name());
    }
}

void PageManager::update_menu_highlight(const std::string& active_name) {
    for (auto& entry : entries_) {
        if (!entry.menu_btn) {
            continue;
        }
        const bool active = entry.page->name() == active_name;
        if (active) {
            lv_obj_add_state(entry.menu_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(entry.menu_btn, LV_STATE_CHECKED);
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
