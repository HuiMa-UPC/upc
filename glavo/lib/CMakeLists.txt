cmake_minimum_required(VERSION 3.16)

set(UPC_GLAVO_TARGET upc_glavo)

set(UPC_GLAVO_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/service_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/glavo_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/glavo_parser_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/tcp_sender.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/protocol_mcu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/print_job_state.cpp
)

add_library(${UPC_GLAVO_TARGET} STATIC ${UPC_GLAVO_SOURCES})
add_library(upc::glavo ALIAS ${UPC_GLAVO_TARGET})

target_compile_features(${UPC_GLAVO_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_GLAVO_TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/glavo
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
