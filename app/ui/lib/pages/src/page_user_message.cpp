#include "page_user_message.hpp"
#include "ui_user_skeleton.hpp"
#include "ui_message_store.hpp"

void page_user_message_refresh(bool /*force*/)
{
    /* 进过一次这页就算全看过了：清未读、熄侧栏红点。
     * 页面本身还是骨架（等设计稿），但"看过"这个语义先立住，
     * 免得红点在设计稿到位之前一直挂着灭不掉。 */
    ui_message_mark_all_read();
}

lv_obj_t *page_user_message_create(lv_obj_t *parent)
{
    return ui_user_skeleton_create(parent, "Messages",
                                     "No messages\n(skeleton — pending design)");
}
