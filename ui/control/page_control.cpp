#include "page_control.hpp"
#include "page_common.hpp"

lv_obj_t *page_control_create(lv_obj_t *parent)
{
    return page_common_create_placeholder(parent,
                                          "Control",
                                          "page_control.cpp\nPrinter control UI\nContent TBD");
}
