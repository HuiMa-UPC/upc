# =============================================================================
# LVGL v9.5 RK3588 DRM 移植层 CMake 构建文件
# =============================================================================
# 功能：为 RK3588 平台提供基于 DRM + evdev 的 LVGL v9.5 移植
# 特性：
#   - 支持作为子工程嵌入，也支持独立 cmake 调试
#   - 通过 configs/*.defaults 选择不同硬件配置
#   - 自动生成 lv_conf.h（复用官方 generate_lv_conf.py）
#   - 过滤 GLTF/OpenGLES 源码，关闭 demos/examples 减少体积
#   - 自动查找 libdrm / libevdev，提供 lvgl::port 接口库
# =============================================================================

cmake_minimum_required(VERSION 3.16)


# -----------------------------------------------------------------------------
# 1. 工程定位：子模块 or 独立构建
# -----------------------------------------------------------------------------
# CMAKE_SOURCE_DIR: 顶层 CMakeLists.txt 所在目录
# CMAKE_CURRENT_SOURCE_DIR: 当前文件所在目录
# 两者相等时，说明用户直接在此目录执行 cmake，非被 add_subdirectory() 引入
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lvgl_port LANGUAGES C CXX)
    # 标记独立构建模式，后续可用于条件编译/安装等
    set(LVGL_STANDALONE_BUILD ON CACHE BOOL "Building lvgl port standalone")
endif()


# -----------------------------------------------------------------------------
# 2. 移植层根目录与配置选择
# -----------------------------------------------------------------------------
# 记录本移植层路径，供父工程引用（PARENT_SCOPE 在文件末尾）
set(LVGL_PORT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "LVGL v9.5 port root")

# 通过 configs/*.defaults 选择配置，默认使用 rongsu_ui 配置
# 可选值取决于 configs/ 目录下有哪些 .defaults 文件
set(LVGL_PORT_CONFIG "rongsu_ui" CACHE STRING "Config under configs/*.defaults")

set(LV_CONF_DEFAULTS_PATH "${LVGL_PORT_DIR}/configs/${LVGL_PORT_CONFIG}.defaults")
if(NOT EXISTS "${LV_CONF_DEFAULTS_PATH}")
    message(FATAL_ERROR "LVGL port config not found: ${LV_CONF_DEFAULTS_PATH}")
endif()

# 声明配置文件的 configure-time 依赖：文件修改时自动重新配置
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LV_CONF_DEFAULTS_PATH}")


# -----------------------------------------------------------------------------
# 3. 生成 lv_conf.h（复用官方移植脚本）
# -----------------------------------------------------------------------------
# 需要 Python3 解释器来运行 generate_lv_conf.py
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 生成的头文件统一放在构建目录下的独立子目录，避免污染
set(LVGL_CONF_GEN_DIR "${CMAKE_BINARY_DIR}/lvgl-port-conf")
file(MAKE_DIRECTORY "${LVGL_CONF_GEN_DIR}")
set(LVGL_CONF_H "${LVGL_CONF_GEN_DIR}/lv_conf.h")

# 官方模板与生成脚本路径
set(_LVGL_TEMPLATE    "${LVGL_PORT_DIR}/lvgl/lv_conf_template.h")
set(_LVGL_GEN_SCRIPT  "${LVGL_PORT_DIR}/lvgl/scripts/generate_lv_conf.py")

if(NOT EXISTS "${_LVGL_TEMPLATE}")
    message(FATAL_ERROR "lvgl submodule missing. Run: git submodule update --init --recursive")
endif()

# 执行 Python 脚本：模板 + defaults -> lv_conf.h
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_LVGL_GEN_SCRIPT}"
        --template   "${_LVGL_TEMPLATE}"
        --defaults   "${LV_CONF_DEFAULTS_PATH}"
        --config     "${LVGL_CONF_H}"
    RESULT_VARIABLE _lv_conf_rc
    OUTPUT_VARIABLE _lv_conf_out
    ERROR_VARIABLE  _lv_conf_err
)
if(NOT _lv_conf_rc EQUAL 0)
    message(FATAL_ERROR "generate lv_conf.h failed:\n${_lv_conf_err}")
endif()
message(STATUS "LVGL port: generated ${LVGL_CONF_H}")


# -----------------------------------------------------------------------------
# 4. LVGL 核心库编译选项（必须在 add_subdirectory 前设置 CACHE 变量）
# -----------------------------------------------------------------------------
# LV_BUILD_CONF_PATH: 告知 lvgl 的 CMake 使用我们生成的 lv_conf.h
set(LV_BUILD_CONF_PATH "${LVGL_CONF_H}" CACHE PATH "Generated lv_conf.h" FORCE)

# 必须为 OFF：否则会创建 .venv 并安装 pcpp，需要 python3-venv（Ubuntu 常未预装）
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "Skip pcpp preprocess" FORCE)

# 关闭 demos / examples / ThorVG / 共享库，减少编译体积和时间
set(CONFIG_LV_BUILD_DEMOS        OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES       OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL  OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS              OFF CACHE BOOL "" FORCE)


# -----------------------------------------------------------------------------
# 5. 添加 LVGL 子目录构建
# -----------------------------------------------------------------------------
# 构建目录指定为 ${CMAKE_BINARY_DIR}/lvgl-core，与旧版 v8.2 保持目录名一致
add_subdirectory(lvgl ${CMAKE_BINARY_DIR}/lvgl-core)


# -----------------------------------------------------------------------------
# 6. 源码过滤：移除 GLTF 相关源文件（RK3588 不需要 3D 模型加载）
# -----------------------------------------------------------------------------
if(TARGET lvgl)
    # 定义 POSIX 版本以启用 clock_gettime / timespec（lv_linux_drm.c 需要）
    target_compile_definitions(lvgl PRIVATE
        _POSIX_C_SOURCE=200809L
        LV_USE_GLTF=0
        LV_USE_OPENGLES=1
        LV_USE_DEMO_GLTF=0
        LV_USE_3DTEXTURE=0
    )
    # 抑制 ARM 交叉编译的 ABI 兼容性警告
    target_compile_options(lvgl PRIVATE -Wno-psabi)

    # 从 lvgl 目标中过滤掉 gltf 目录下的源文件，避免编译无用代码
    get_target_property(_lvgl_sources lvgl SOURCES)
    set(_lvgl_sources_keep "")
    foreach(_src IN LISTS _lvgl_sources)
        if(_src MATCHES "[/\\\\]libs[/\\\\]gltf[/\\\\]")
            continue()  # 跳过 GLTF 相关源文件
        endif()
        list(APPEND _lvgl_sources_keep "${_src}")
    endforeach()

    # 重写目标的源文件列表
    set_target_properties(lvgl PROPERTIES SOURCES "${_lvgl_sources_keep}")

    # 统计并报告过滤结果
    list(LENGTH _lvgl_sources       _n_before)
    list(LENGTH _lvgl_sources_keep  _n_after)
    math(EXPR _n_removed "${_n_before} - ${_n_after}")
    if(_n_removed GREATER 0)
        message(STATUS "LVGL port: excluded ${_n_removed} GLTF sources from lvgl")
    endif()
endif()


# -----------------------------------------------------------------------------
# 7. 平台依赖库查找：DRM + evdev（RK3588 板端必需）
# -----------------------------------------------------------------------------
# 基础链接库：lvgl 核心 + 数学库 + 线程/动态库 + GBM/EGL/GLES + Mali GPU 驱动
set(_LVGL_PORT_LIBS
    lvgl::lvgl
    m pthread rt dl
    -Wl,--no-as-needed gbm EGL GLESv2  mali
    -Wl,--as-needed
)

# 尝试通过 pkg-config 查找系统库（更可靠，能获取 include 路径）
find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBDRM   QUIET libdrm)
    pkg_check_modules(LIBEVDEV QUIET libevdev)
endif()

# libdrm：Direct Rendering Manager，用于 GPU 显示输出
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


# -----------------------------------------------------------------------------
# 8. 导出接口库：lvgl::port
# -----------------------------------------------------------------------------
# 使用 INTERFACE 库封装所有依赖，父工程只需 target_link_libraries(... lvgl::port)
add_library(lvgl_port INTERFACE)
add_library(lvgl::port ALIAS lvgl_port)

# 链接依赖
target_link_libraries(lvgl_port INTERFACE ${_LVGL_PORT_LIBS})

# 头文件包含路径：lvgl 源码目录 + 生成的 lv_conf.h 目录
target_include_directories(lvgl_port INTERFACE
    "${LVGL_PORT_DIR}/lvgl"
    "${LVGL_CONF_GEN_DIR}"
)

# 追加 pkg-config 找到的系统 include 路径
if(LIBDRM_FOUND)
    target_include_directories(lvgl_port INTERFACE ${LIBDRM_INCLUDE_DIRS})
endif()
if(LIBEVDEV_FOUND)
    target_include_directories(lvgl_port INTERFACE ${LIBEVDEV_INCLUDE_DIRS})
endif()


# -----------------------------------------------------------------------------
# 9. 独立构建时的状态报告 + 父工程导出
# -----------------------------------------------------------------------------
if(LVGL_STANDALONE_BUILD)
    message(STATUS "LVGL port v9.5: lvgl + DRM (${LVGL_PORT_CONFIG})")
endif()

# 向父工程导出关键路径变量（仅当本文件被 add_subdirectory() 引入时生效）
set(LVGL_PORT_DIR   "${LVGL_PORT_DIR}"   PARENT_SCOPE)
set(LVGL_CONF_DIR   "${LVGL_CONF_GEN_DIR}" PARENT_SCOPE)