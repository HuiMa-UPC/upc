cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED UPC_DEBUG)
    set(UPC_DEBUG OFF)
endif()

if(NOT DEFINED UPC_LOG_ENABLED)
    if(UPC_DEBUG)
        set(UPC_LOG_ENABLED 1)
    else()
        set(UPC_LOG_ENABLED 0)
    endif()
endif()

set(UPC_DEBUG_TARGET upc_debug)

add_library(${UPC_DEBUG_TARGET} STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rs_log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/upc_sys_monitor.cpp
)

add_library(upc::debug ALIAS ${UPC_DEBUG_TARGET})

target_include_directories(${UPC_DEBUG_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

if(UPC_DEBUG)
    target_compile_definitions(${UPC_DEBUG_TARGET} PUBLIC UPC_DEBUG=1)
    message(STATUS "upc_debug: logging ENABLED (wall-clock stderr)")
else()
    target_compile_definitions(${UPC_DEBUG_TARGET} PUBLIC UPC_DEBUG=0)
    message(STATUS "upc_debug: logging DISABLED")
endif()

target_compile_definitions(${UPC_DEBUG_TARGET} PUBLIC UPC_LOG_ENABLED=${UPC_LOG_ENABLED})

set(UPC_DEBUG_TARGET ${UPC_DEBUG_TARGET} PARENT_SCOPE)
