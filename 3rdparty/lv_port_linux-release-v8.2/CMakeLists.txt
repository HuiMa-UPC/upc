cmake_minimum_required(VERSION 3.16)

# 检测是否为顶层构建（standalone 模式）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lvgl_port LANGUAGES C)
    set(LVGL_STANDALONE_BUILD ON CACHE BOOL "Building lvgl port as top-level project")
endif()
# 定义 LVGL 移植根目录
set(LVGL_PORT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "LVGL port root (lv_conf.h / lv_drv_conf.h)")
# 配置 LVGL 的头文件查找路径
set(LV_CONF_PATH "${LVGL_PORT_DIR}/lv_conf.h" CACHE STRING "Path to lv_conf.h" FORCE)
# LV_CONF_INCLUDE_SIMPLE: 使用简单路径包含 lv_conf.h
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)
# LV_LVGL_H_INCLUDE_SIMPLE: 关闭简单路径包含 lvgl.h
# 这个变量控制 lvgl 内部头文件的包含方式
set(LV_LVGL_H_INCLUDE_SIMPLE OFF CACHE BOOL "" FORCE)

add_subdirectory(lvgl ${CMAKE_BINARY_DIR}/lvgl-core)
add_subdirectory(lv_drivers ${CMAKE_BINARY_DIR}/lv_drivers)

target_include_directories(lvgl PUBLIC ${LVGL_PORT_DIR})
target_include_directories(lv_drivers PUBLIC ${LVGL_PORT_DIR})

add_library(lvgl_port INTERFACE)
add_library(lvgl_port::port ALIAS lvgl_port)
target_link_libraries(lvgl_port INTERFACE lvgl::lvgl lvgl::drivers)

if(LVGL_STANDALONE_BUILD)
    message(STATUS "LVGL port: lvgl + lv_drivers")
endif()

set(LVGL_PORT_DIR ${LVGL_PORT_DIR} PARENT_SCOPE)
