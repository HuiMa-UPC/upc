cmake_minimum_required(VERSION 3.16)

set(UPC_GLAVO_TARGET upc_glavo)

set(UPC_GLAVO_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/service_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo_parser_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu_link.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol_mcu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/print_job_state.cpp
)

add_library(${UPC_GLAVO_TARGET} STATIC ${UPC_GLAVO_SOURCES})
add_library(upc::glavo ALIAS ${UPC_GLAVO_TARGET})

target_compile_features(${UPC_GLAVO_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_GLAVO_TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common/ipc
)

target_link_libraries(${UPC_GLAVO_TARGET}
    PUBLIC
        upc::parser
        middleware::port
        upc::ipc
        upc::debug
)

target_compile_options(${UPC_GLAVO_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

set(UPC_GLAVO_TARGET ${UPC_GLAVO_TARGET} PARENT_SCOPE)
