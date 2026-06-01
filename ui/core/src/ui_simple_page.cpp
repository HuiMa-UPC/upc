#include "ui_simple_page.hpp"

namespace ui {

SimplePage::SimplePage(std::string name,
                       CreateFn create,
                       RefreshFn refresh,
                       ExitFn on_exit)
    : name_(std::move(name))
    , create_(create)
    , refresh_(refresh)
    , on_exit_(on_exit) {}

void SimplePage::on_create(lv_obj_t *parent)
{
    if(create_) {
        root_ = create_(parent);
    }
}

void SimplePage::on_enter()
{
    if(root_) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
    if(refresh_) {
        refresh_(true);
    }
}

void SimplePage::on_exit()
{
    if(root_) {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
    if(on_exit_) {
        on_exit_();
    }
}

} // namespace ui
