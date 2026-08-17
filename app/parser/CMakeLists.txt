# =============================================================================
# app/parser/CMakeLists.txt — 切片解析库构建脚本
#
# 产出三个静态库目标（插件可选）：
#   upc_parser       (upc::parser)       — 框架核心：Manager / Registry / RingBuffer
#   upc_parser_gcode (upc::parser_gcode) — G-code 格式插件（静态注册）
#   upc_parser_ovf   (upc::parser_ovf)   — OVF 格式插件（依赖 Protobuf）
#
# 构建模式
# --------
# 1. 嵌入主工程：由 app/CMakeLists.txt add_subdirectory 引入，使用交叉工具链 + upc::debug；
#    固定构建全部格式插件（gcode + ovf），与 rongsu_glavo 默认链接一致。
# 2. 独立根目录：在 app/parser/ 下执行 cmake -S . -B build，使用本机编译器，无 common 依赖；
#    通过 -DUPC_PARSER_BUILD_GCODE / -DUPC_PARSER_BUILD_OVF 按需选择插件。
#
# 独立构建示例
# ------------
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake -S . -B build -DUPC_PARSER_BUILD_GCODE=ON  -DUPC_PARSER_BUILD_OVF=OFF
#   cmake -S . -B build -DUPC_PARSER_BUILD_OVF=ON      # 需本机 Protobuf 与 .pb 版本匹配
#
# 消费方链接插件时需 --whole-archive，否则链接器可能剥离 *_register.cpp：
#   target_link_libraries(my_proc PRIVATE
#       -Wl,--whole-archive upc::parser_gcode upc::parser -Wl,--no-whole-archive)
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# -----------------------------------------------------------------------------
# 构建模式检测
# CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR 表示本目录为 CMake 根目录（独立构建）
# -----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(UPC_PARSER_STANDALONE TRUE)
else()
    set(UPC_PARSER_STANDALONE FALSE)
endif()

# -----------------------------------------------------------------------------
# 独立构建引导：project / 编译选项；日志走 lib/log 桩（UPC_LOG_* 空操作，不链 common）
# 嵌入主工程时跳过此段，由顶层 CMakeLists 提供 project 与 upc::debug
# -----------------------------------------------------------------------------
if(UPC_PARSER_STANDALONE)
    project(upc_parser LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    add_compile_options(-Wall -Wextra)
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

# -----------------------------------------------------------------------------
# 插件开关（核心库 upc_parser 始终构建，插件依赖它）
# 嵌入主工程：固定启用全部格式插件；独立构建：通过 option 按需选择
# -----------------------------------------------------------------------------
if(UPC_PARSER_STANDALONE)
    option(UPC_PARSER_BUILD_GCODE "Build G-code parser plugin" ON)
    option(UPC_PARSER_BUILD_OVF "Build OVF parser plugin (requires Protobuf)" OFF)

    if(NOT UPC_PARSER_BUILD_GCODE AND NOT UPC_PARSER_BUILD_OVF)
        message(STATUS "upc_parser: plugins disabled, building core only")
    endif()
else()
    set(UPC_PARSER_BUILD_GCODE ON)
    set(UPC_PARSER_BUILD_OVF ON)
    message(STATUS "upc_parser: embedded build, enabling all format plugins")
endif()

# OVF 需要 Protobuf：独立构建用 find_package，嵌入构建由交叉 sysroot 提供 protobuf 目标
if(UPC_PARSER_BUILD_OVF)
    if(UPC_PARSER_STANDALONE)
        find_package(Protobuf REQUIRED)
        set(UPC_PARSER_PROTOBUF_LIB protobuf::libprotobuf)
    else()
        set(UPC_PARSER_PROTOBUF_LIB protobuf)
    endif()

    # OVF protobuf 代码版本选择：
    # - AUTO：根据 protobuf 头版本自动判定（支持多交叉工具链共存）
    # - 手动：可显式指定 V3.18 / V3.21.12
    set(UPC_PARSER_OVF_PB_VARIANT "AUTO" CACHE STRING
        "OVF protobuf source variant: AUTO|V3.18|V3.21.12")
    set_property(CACHE UPC_PARSER_OVF_PB_VARIANT PROPERTY STRINGS
        AUTO V3.18 V3.21.12)

    set(_upc_ovf_pb_variant "${UPC_PARSER_OVF_PB_VARIANT}")
    set(_upc_pb_header_ver "")
    if(_upc_ovf_pb_variant STREQUAL "AUTO")
        # 交叉构建优先从 sysroot 读取 protobuf 头版本
        if(DEFINED CMAKE_SYSROOT AND CMAKE_SYSROOT)
            set(_upc_pb_common_h
                "${CMAKE_SYSROOT}/usr/include/google/protobuf/stubs/common.h")
            if(EXISTS "${_upc_pb_common_h}")
                file(STRINGS "${_upc_pb_common_h}" _upc_pb_ver_line
                    REGEX "^#define[ \t]+GOOGLE_PROTOBUF_VERSION[ \t]+[0-9]+")
                if(_upc_pb_ver_line)
                    string(REGEX REPLACE
                        "^#define[ \t]+GOOGLE_PROTOBUF_VERSION[ \t]+([0-9]+).*"
                        "\\1"
                        _upc_pb_header_ver
                        "${_upc_pb_ver_line}")
                endif()
            endif()
        endif()

        # protobuf >= 3.21.x 选 V3.21.12；否则回落 V3.18
        if(_upc_pb_header_ver AND _upc_pb_header_ver GREATER_EQUAL 3021000)
            set(_upc_ovf_pb_variant "V3.21.12")
        else()
            set(_upc_ovf_pb_variant "V3.18")
        endif()
    endif()

    if(NOT _upc_ovf_pb_variant STREQUAL "V3.18" AND
       NOT _upc_ovf_pb_variant STREQUAL "V3.21.12")
        message(FATAL_ERROR
            "Invalid UPC_PARSER_OVF_PB_VARIANT=${UPC_PARSER_OVF_PB_VARIANT}, "
            "expected AUTO|V3.18|V3.21.12")
    endif()

    set(UPC_PARSER_OVF_PB_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/ovf/${_upc_ovf_pb_variant}")
    foreach(_pb_file
        build_processor_strategy.pb.cc
        open_vector_format.pb.cc
        ovf_lut.pb.cc
        build_processor_strategy.pb.h
        open_vector_format.pb.h
        ovf_lut.pb.h)
        if(NOT EXISTS "${UPC_PARSER_OVF_PB_DIR}/${_pb_file}")
            message(FATAL_ERROR
                "OVF protobuf source missing: ${UPC_PARSER_OVF_PB_DIR}/${_pb_file}")
        endif()
    endforeach()

    message(STATUS
        "upc_parser_ovf: pb variant=${_upc_ovf_pb_variant}"
        " (GOOGLE_PROTOBUF_VERSION=${_upc_pb_header_ver})")
endif()

# -----------------------------------------------------------------------------
# 目标名称常量（供 PARENT_SCOPE 导出及外部引用）
# -----------------------------------------------------------------------------
set(UPC_PARSER_TARGET upc_parser)
set(UPC_PARSER_GCODE_TARGET upc_parser_gcode)
set(UPC_PARSER_OVF_TARGET upc_parser_ovf)

# -----------------------------------------------------------------------------
# 核心静态库 upc_parser / upc::parser
# 源：RingBuffer、校准、Registry、Manager
# -----------------------------------------------------------------------------
set(UPC_PARSER_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/slice_ring_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/cal_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/parser_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/parser_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/parser_manager.cpp
)

add_library(${UPC_PARSER_TARGET} STATIC ${UPC_PARSER_CORE_SOURCES})
add_library(upc::parser ALIAS ${UPC_PARSER_TARGET})

target_compile_features(${UPC_PARSER_TARGET} PUBLIC cxx_std_17)

set(UPC_PARSER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/gcode
    ${CMAKE_CURRENT_SOURCE_DIR}/ovf
)

# 独立构建：lib/log 提供 rs_log.hpp 桩；嵌入主工程：链接 upc::debug 提供完整日志
if(UPC_PARSER_STANDALONE)
    list(PREPEND UPC_PARSER_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/lib/log)
    target_compile_definitions(${UPC_PARSER_TARGET} PUBLIC UPC_PARSER_STANDALONE=1)
    message(STATUS "upc_parser: standalone build (logging disabled via stub headers)")
else()
    target_link_libraries(${UPC_PARSER_TARGET} PUBLIC upc::debug)
endif()

target_include_directories(${UPC_PARSER_TARGET} PUBLIC ${UPC_PARSER_INCLUDE_DIRS})

target_compile_options(${UPC_PARSER_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

# -----------------------------------------------------------------------------
# G-code 插件 upc_parser_gcode / upc::parser_gcode
# 源：GcodeWorker 实现 + gcode_register 静态注册
# -----------------------------------------------------------------------------
if(UPC_PARSER_BUILD_GCODE)
    add_library(${UPC_PARSER_GCODE_TARGET} STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/gcode/gcode_worker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gcode/gcode_register.cpp
    )
    add_library(upc::parser_gcode ALIAS ${UPC_PARSER_GCODE_TARGET})

    target_compile_features(${UPC_PARSER_GCODE_TARGET} PUBLIC cxx_std_17)
    target_link_libraries(${UPC_PARSER_GCODE_TARGET} PUBLIC upc::parser)
    target_compile_options(${UPC_PARSER_GCODE_TARGET} PRIVATE -Wall -Wextra)
endif()

# -----------------------------------------------------------------------------
# OVF 插件 upc_parser_ovf / upc::parser_ovf
# 源：OvfWorker + 官方 LVF! I/O + 静态注册 + 已生成的 protobuf（.pb.cc，不构建时 protoc）
# -----------------------------------------------------------------------------
if(UPC_PARSER_BUILD_OVF)
    add_library(${UPC_PARSER_OVF_TARGET} STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ovf/ovf_worker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ovf/ovf_official_io.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ovf/ovf_register.cpp
        ${UPC_PARSER_OVF_PB_DIR}/build_processor_strategy.pb.cc
        ${UPC_PARSER_OVF_PB_DIR}/open_vector_format.pb.cc
        ${UPC_PARSER_OVF_PB_DIR}/ovf_lut.pb.cc
    )
    add_library(upc::parser_ovf ALIAS ${UPC_PARSER_OVF_TARGET})

    target_compile_features(${UPC_PARSER_OVF_TARGET} PUBLIC cxx_std_17)
    target_include_directories(${UPC_PARSER_OVF_TARGET}
        PRIVATE ${UPC_PARSER_OVF_PB_DIR})
    target_link_libraries(${UPC_PARSER_OVF_TARGET}
        PUBLIC upc::parser ${UPC_PARSER_PROTOBUF_LIB})
    target_compile_options(${UPC_PARSER_OVF_TARGET} PRIVATE -Wall -Wextra)
endif()

# 嵌入主工程时向父作用域导出目标名（独立构建无父目录，跳过）
if(NOT UPC_PARSER_STANDALONE)
    set(UPC_PARSER_TARGET ${UPC_PARSER_TARGET} PARENT_SCOPE)
    set(UPC_PARSER_GCODE_TARGET ${UPC_PARSER_GCODE_TARGET} PARENT_SCOPE)
    set(UPC_PARSER_OVF_TARGET ${UPC_PARSER_OVF_TARGET} PARENT_SCOPE)
endif()
