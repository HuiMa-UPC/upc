#include "page_user_message.hpp"
#include "ui_user_skeleton.hpp"

void page_user_message_refresh(bool /*force*/)
{
}

lv_obj_t *page_user_message_create(lv_obj_t *parent)
{
    return ui_user_skeleton_create(parent, "Messages",
                                     "No messages\n(skeleton — pending design)");
}
