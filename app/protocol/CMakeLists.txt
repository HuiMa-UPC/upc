cmake_minimum_required(VERSION 3.16)

set(UPC_PROTOCOL_TARGET upc_protocol)

set(UPC_PROTOCOL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/tcp_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/tcp_tx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/tcp_rx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_ack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_print.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_env.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_debug.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_ota.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial_project_info.cpp
)

add_library(${UPC_PROTOCOL_TARGET} STATIC ${UPC_PROTOCOL_SOURCES})
add_library(upc::protocol ALIAS ${UPC_PROTOCOL_TARGET})

target_compile_features(${UPC_PROTOCOL_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_PROTOCOL_TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tcp
        ${CMAKE_CURRENT_SOURCE_DIR}/serial
)

target_compile_options(${UPC_PROTOCOL_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

set(UPC_PROTOCOL_TARGET ${UPC_PROTOCOL_TARGET} PARENT_SCOPE)
