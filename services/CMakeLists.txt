cmake_minimum_required(VERSION 3.16)

set(SERVICES_TARGET services)
set(SERVICES_PARSERS_TARGET services_parsers)

file(GLOB SERVICES_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/parser/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/glavo/*.c*"
)

file(GLOB SERVICES_PARSER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/glavo/parsers/*/*.c*"
)

if(NOT SERVICES_CORE_SOURCES)
    message(FATAL_ERROR "services: no core source files found")
endif()

add_library(${SERVICES_TARGET} STATIC ${SERVICES_CORE_SOURCES})
add_library(services::port ALIAS ${SERVICES_TARGET})

target_compile_features(${SERVICES_TARGET} PUBLIC cxx_std_17)

target_include_directories(${SERVICES_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glavo>
)

target_link_libraries(${SERVICES_TARGET}
    PUBLIC
        middleware::port
)

# 各协议解析插件（独立静态库，链接时需 --whole-archive 以保证静态注册生效）
if(SERVICES_PARSER_SOURCES)
    add_library(${SERVICES_PARSERS_TARGET} STATIC ${SERVICES_PARSER_SOURCES})
    add_library(services::parsers ALIAS ${SERVICES_PARSERS_TARGET})

    target_compile_features(${SERVICES_PARSERS_TARGET} PUBLIC cxx_std_17)
    target_link_libraries(${SERVICES_PARSERS_TARGET} PUBLIC ${SERVICES_TARGET})

    file(GLOB PARSER_PLUGIN_DIRS LIST_DIRECTORIES true
        "${CMAKE_CURRENT_SOURCE_DIR}/glavo/parsers/*")
    foreach(_parser_dir IN LISTS PARSER_PLUGIN_DIRS)
        if(IS_DIRECTORY "${_parser_dir}")
            target_include_directories(${SERVICES_PARSERS_TARGET}
                PUBLIC
                    $<BUILD_INTERFACE:${_parser_dir}>)
        endif()
    endforeach()
else()
    message(STATUS "services: no parser plugins under glavo/parsers/")
endif()

# 调试日志总开关：cmake -DSERVICES_DEBUG=ON/OFF
option(SERVICES_DEBUG "Print services debug trace to stderr" OFF)

if(SERVICES_DEBUG)
    target_compile_definitions(${SERVICES_TARGET} PUBLIC SERVICES_DEBUG=1)
    message(STATUS "services: debug logging ENABLED")
else()
    target_compile_definitions(${SERVICES_TARGET} PUBLIC SERVICES_DEBUG=0)
    message(STATUS "services: debug logging DISABLED")
endif()

# TCP/串口链路调试开关：cmake -DTCP_DEBUG=ON/OFF
option(TCP_DEBUG "Print TCP sender and serial link debug trace to stderr" OFF)

if(TCP_DEBUG)
    target_compile_definitions(${SERVICES_TARGET} PUBLIC TCP_DEBUG=1)
    message(STATUS "services: TCP link debug logging ENABLED")
else()
    target_compile_definitions(${SERVICES_TARGET} PUBLIC TCP_DEBUG=0)
    message(STATUS "services: TCP link debug logging DISABLED")
endif()

foreach(_svc_tgt IN ITEMS ${SERVICES_TARGET} ${SERVICES_PARSERS_TARGET})
    if(TARGET ${_svc_tgt})
        target_compile_options(${_svc_tgt}
            PRIVATE
                -Wall -Wextra
                $<$<CONFIG:Debug>:-O0>
                $<$<CONFIG:Release>:-O2>
        )
    endif()
endforeach()

set(SERVICES_TARGET ${SERVICES_TARGET} PARENT_SCOPE)
set(SERVICES_PARSERS_TARGET ${SERVICES_PARSERS_TARGET} PARENT_SCOPE)

if(SERVICES_STANDALONE_BUILD)
    message(STATUS "services v1.0.0 (standalone)")
endif()
