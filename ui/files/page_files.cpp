#include "page_files.hpp"
#include "page_common.hpp"

lv_obj_t *page_files_create(lv_obj_t *parent)
{
    return page_common_create_placeholder(parent,
                                          "Files",
                                          "page_files.cpp\nFile browser / G-code\nContent TBD");
}
