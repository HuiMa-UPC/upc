#include "page_home.hpp"
#include "page_common.hpp"

lv_obj_t *page_home_create(lv_obj_t *parent)
{
    return page_common_create_placeholder(parent,
                                          "Home",
                                          "page_home.cpp\nAssignee: ___\nContent TBD");
}
