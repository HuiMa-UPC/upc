#include "page_user_history.hpp"
#include "ui_user_skeleton.hpp"

void page_user_history_refresh(bool /*force*/)
{
}

lv_obj_t *page_user_history_create(lv_obj_t *parent)
{
    return ui_user_skeleton_create(parent, "History",
                                     "No history yet\n(skeleton — pending design)");
}
