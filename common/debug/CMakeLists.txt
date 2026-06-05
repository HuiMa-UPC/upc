cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED UPC_DEBUG)
    set(UPC_DEBUG OFF)
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

set(UPC_DEBUG_TARGET ${UPC_DEBUG_TARGET} PARENT_SCOPE)
