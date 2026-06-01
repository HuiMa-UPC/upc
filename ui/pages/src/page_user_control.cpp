#include "page_user_control.hpp"
#include "ui_user_skeleton.hpp"

void page_user_control_refresh(bool /*force*/)
{
}

lv_obj_t *page_user_control_create(lv_obj_t *parent)
{
    return ui_user_skeleton_create(parent, "Control",
                                     "User control panel\n(skeleton — pending design)");
}
