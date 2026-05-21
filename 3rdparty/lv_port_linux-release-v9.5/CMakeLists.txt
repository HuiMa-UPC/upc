cmake_minimum_required(VERSION 3.16)

# 作为 upc 子工程嵌入；也可在本目录单独 cmake 调试
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lvgl_port LANGUAGES C CXX)
    set(LVGL_STANDALONE_BUILD ON CACHE BOOL "Building lvgl port standalone")
endif()

set(LVGL_PORT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "LVGL v9.5 port root")

set(LVGL_PORT_CONFIG "rongsu_ui" CACHE STRING "Config under configs/*.defaults")
set(LV_CONF_DEFAULTS_PATH "${LVGL_PORT_DIR}/configs/${LVGL_PORT_CONFIG}.defaults")
if(NOT EXISTS "${LV_CONF_DEFAULTS_PATH}")
    message(FATAL_ERROR "LVGL port config not found: ${LV_CONF_DEFAULTS_PATH}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LV_CONF_DEFAULTS_PATH}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 生成 lv_conf.h（与官方 port 相同脚本）
set(LVGL_CONF_GEN_DIR "${CMAKE_BINARY_DIR}/lvgl-port-conf")
file(MAKE_DIRECTORY "${LVGL_CONF_GEN_DIR}")
set(LVGL_CONF_H "${LVGL_CONF_GEN_DIR}/lv_conf.h")

set(_LVGL_TEMPLATE "${LVGL_PORT_DIR}/lvgl/lv_conf_template.h")
set(_LVGL_GEN_SCRIPT "${LVGL_PORT_DIR}/lvgl/scripts/generate_lv_conf.py")
if(NOT EXISTS "${_LVGL_TEMPLATE}")
    message(FATAL_ERROR "lvgl submodule missing. Run: git submodule update --init --recursive")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_LVGL_GEN_SCRIPT}"
        --template "${_LVGL_TEMPLATE}"
        --defaults "${LV_CONF_DEFAULTS_PATH}"
        --config "${LVGL_CONF_H}"
    RESULT_VARIABLE _lv_conf_rc
    OUTPUT_VARIABLE _lv_conf_out
    ERROR_VARIABLE _lv_conf_err
)
if(NOT _lv_conf_rc EQUAL 0)
    message(FATAL_ERROR "generate lv_conf.h failed:\n${_lv_conf_err}")
endif()
message(STATUS "LVGL port: generated ${LVGL_CONF_H}")

# lvgl 子目录（构建目录名与 v8.2 保持一致）
set(LV_BUILD_CONF_PATH "${LVGL_CONF_H}" CACHE PATH "Generated lv_conf.h" FORCE)
# 必须为 OFF：否则会创建 .venv 并安装 pcpp，需要 python3-venv（Ubuntu 常未装）
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "Skip pcpp preprocess" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(lvgl ${CMAKE_BINARY_DIR}/lvgl-core)

if(TARGET lvgl)
    # 交叉编译 Buildroot：clock_gettime / timespec（lv_linux_drm.c）
    target_compile_definitions(lvgl PRIVATE
        _POSIX_C_SOURCE=200809L
        LV_USE_GLTF=0
        LV_USE_OPENGLES=0
        LV_USE_DEMO_GLTF=0
        LV_USE_3DTEXTURE=0
    )
    target_compile_options(lvgl PRIVATE -Wno-psabi)

    # DRM UI 不用 GLTF：该目录 .c 含 C++ 原始字符串 R"(...)"，用 C 编译会刷屏告警
    get_target_property(_lvgl_sources lvgl SOURCES)
    set(_lvgl_sources_keep "")
    foreach(_src IN LISTS _lvgl_sources)
        if(_src MATCHES "[/\\\\]libs[/\\\\]gltf[/\\\\]" OR _src MATCHES "[/\\\\]opengles[/\\\\]")
            continue()
        endif()
        list(APPEND _lvgl_sources_keep "${_src}")
    endforeach()
    set_target_properties(lvgl PROPERTIES SOURCES "${_lvgl_sources_keep}")
    list(LENGTH _lvgl_sources _n_before)
    list(LENGTH _lvgl_sources_keep _n_after)
    math(EXPR _n_removed "${_n_before} - ${_n_after}")
    if(_n_removed GREATER 0)
        message(STATUS "LVGL port: excluded ${_n_removed} GLTF/OpenGLES sources from lvgl")
    endif()
endif()

# DRM / evdev（RK3588 板端需要）
set(_LVGL_PORT_LIBS lvgl::lvgl m pthread rt dl)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBDRM QUIET libdrm)
    pkg_check_modules(LIBEVDEV QUIET libevdev)
endif()

if(LIBDRM_FOUND)
    list(APPEND _LVGL_PORT_LIBS ${LIBDRM_LIBRARIES})
else()
    find_library(_DRM_LIB drm)
    if(_DRM_LIB)
        list(APPEND _LVGL_PORT_LIBS ${_DRM_LIB})
    else()
        message(WARNING "libdrm not found; DRM driver may fail at link/run time")
    endif()
endif()

if(LIBEVDEV_FOUND)
    list(APPEND _LVGL_PORT_LIBS ${LIBEVDEV_LIBRARIES})
else()
    find_library(_EVDEV_LIB evdev)
    if(_EVDEV_LIB)
        list(APPEND _LVGL_PORT_LIBS ${_EVDEV_LIB})
    else()
        message(WARNING "libevdev not found; touch input may fail")
    endif()

endif()

add_library(lvgl_port INTERFACE)
add_library(lvgl::port ALIAS lvgl_port)

target_link_libraries(lvgl_port INTERFACE ${_LVGL_PORT_LIBS})

target_include_directories(lvgl_port INTERFACE
    "${LVGL_PORT_DIR}/lvgl"
    "${LVGL_CONF_GEN_DIR}"
)

if(LIBDRM_FOUND)
    target_include_directories(lvgl_port INTERFACE ${LIBDRM_INCLUDE_DIRS})
endif()
if(LIBEVDEV_FOUND)
    target_include_directories(lvgl_port INTERFACE ${LIBEVDEV_INCLUDE_DIRS})
endif()

if(LVGL_STANDALONE_BUILD)
    message(STATUS "LVGL port v9.5: lvgl + DRM (${LVGL_PORT_CONFIG})")
endif()

set(LVGL_PORT_DIR "${LVGL_PORT_DIR}" PARENT_SCOPE)
set(LVGL_CONF_DIR "${LVGL_CONF_GEN_DIR}" PARENT_SCOPE)
