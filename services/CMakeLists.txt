cmake_minimum_required(VERSION 3.16)

set(SERVICES_TARGET services)

file(GLOB_RECURSE SERVICES_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/glavo/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/mcu/*.c*"
)
if(NOT SERVICES_SOURCES)
    message(FATAL_ERROR "services: no source files found")
endif()

add_library(${SERVICES_TARGET} STATIC ${SERVICES_SOURCES})
add_library(services::port ALIAS ${SERVICES_TARGET})

target_compile_features(${SERVICES_TARGET} PUBLIC cxx_std_17)

target_include_directories(${SERVICES_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glavo>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mcu>
)

target_link_libraries(${SERVICES_TARGET}
    PUBLIC
        middleware::port
)

# 调试日志总开关：cmake -DSERVICES_DEBUG=ON/OFF
option(SERVICES_DEBUG "Print services debug trace to stderr" OFF)

if(SERVICES_DEBUG)
    target_compile_definitions(${SERVICES_TARGET} PUBLIC SERVICES_DEBUG=1)
    message(STATUS "services: debug logging ENABLED")
else()
    target_compile_definitions(${SERVICES_TARGET} PUBLIC SERVICES_DEBUG=0)
    message(STATUS "services: debug logging DISABLED")
endif()

target_compile_options(${SERVICES_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

set(SERVICES_TARGET ${SERVICES_TARGET} PARENT_SCOPE)

if(SERVICES_STANDALONE_BUILD)
    message(STATUS "services v1.0.0 (standalone)")
endif()
