#ifndef UI_SIMPLE_PAGE_HPP
#define UI_SIMPLE_PAGE_HPP

#include "page_manager.hpp"

namespace ui {

/**
 * @brief 将 C 风格 page_xxx_create / _refresh 接到 PageManager 的薄适配器。
 *
 * 用于替代独立的 user_pages.cpp：注册时直接 make_unique<SimplePage>(...)。
 */
class SimplePage : public Page {
public:
    using CreateFn  = lv_obj_t *(*)(lv_obj_t *parent);
    using RefreshFn = void (*)(bool force);
    using ExitFn    = void (*)();

    SimplePage(std::string name,
               CreateFn create,
               RefreshFn refresh = nullptr,
               ExitFn on_exit = nullptr);

    std::string name() const override { return name_; }
    void on_create(lv_obj_t *parent) override;
    void on_enter() override;
    void on_exit() override;

private:
    std::string name_;
    CreateFn    create_;
    RefreshFn   refresh_;
    ExitFn      on_exit_;
    lv_obj_t   *root_ = nullptr;
};

} // namespace ui

#endif
