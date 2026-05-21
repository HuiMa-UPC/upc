cmake_minimum_required(VERSION 3.16)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/toolchain-aarch64-buildroot.cmake"
        CACHE FILEPATH "Cross toolchain (optional)")
endif()

project(upc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(APP_TARGET demo CACHE STRING "Application executable name")

add_subdirectory(3rdparty/lv_port_linux-release-v9.5)

set(APP_SOURCES
    src/main.cpp
    src/platform.cpp
    src/ui_theme.cpp
    src/ui_shell.cpp
    ui/common/page_common.cpp
    ui/home/page_home.cpp
    ui/control/page_control.cpp
    ui/files/page_files.cpp
    ui/settings/page_settings.cpp
    ui/serial/page_serial.cpp
    ui/tcpclient/page_tcpclient.cpp
)

add_executable(${APP_TARGET} ${APP_SOURCES})

target_include_directories(${APP_TARGET} PRIVATE 
                            ${CMAKE_SOURCE_DIR}/inc
                            ${CMAKE_SOURCE_DIR}/ui/common
                            ${CMAKE_SOURCE_DIR}/ui/control
                            ${CMAKE_SOURCE_DIR}/ui/files
                            ${CMAKE_SOURCE_DIR}/ui/home
                            ${CMAKE_SOURCE_DIR}/ui/settings
                            ${CMAKE_SOURCE_DIR}/ui/serial
                            ${CMAKE_SOURCE_DIR}/ui/tcpclient
                          )

target_compile_definitions(${APP_TARGET} PRIVATE _POSIX_C_SOURCE=200809L)

target_compile_options(${APP_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -O3>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -O3>
)

target_link_libraries(${APP_TARGET} PRIVATE lvgl::port)

find_package(Threads REQUIRED)
target_link_libraries(${APP_TARGET} PRIVATE Threads::Threads)
