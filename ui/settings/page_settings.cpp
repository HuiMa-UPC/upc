#include "page_settings.hpp"
#include "page_common.hpp"

lv_obj_t *page_settings_create(lv_obj_t *parent)
{
    return page_common_create_placeholder(parent,
                                          "Settings",
                                          "page_settings.cpp\nSystem settings\nContent TBD");
}
