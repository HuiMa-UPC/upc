cmake_minimum_required(VERSION 3.16)

set(UPC_PARSER_TARGET upc_parser)
set(UPC_PARSER_GCODE_TARGET upc_parser_gcode)
set(UPC_PARSER_OVF_TARGET upc_parser_ovf)

set(UPC_PARSER_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/slice_ring_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cal_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser_manager.cpp
)

add_library(${UPC_PARSER_TARGET} STATIC ${UPC_PARSER_CORE_SOURCES})
add_library(upc::parser ALIAS ${UPC_PARSER_TARGET})

target_compile_features(${UPC_PARSER_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_PARSER_TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/gcode
        ${CMAKE_CURRENT_SOURCE_DIR}/ovf
)

target_link_libraries(${UPC_PARSER_TARGET}
    PUBLIC
        upc::debug
)

target_compile_options(${UPC_PARSER_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

add_library(${UPC_PARSER_GCODE_TARGET} STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/gcode/gcode_worker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gcode/gcode_register.cpp
)
add_library(upc::parser_gcode ALIAS ${UPC_PARSER_GCODE_TARGET})

target_compile_features(${UPC_PARSER_GCODE_TARGET} PUBLIC cxx_std_17)
target_link_libraries(${UPC_PARSER_GCODE_TARGET} PUBLIC upc::parser)
target_compile_options(${UPC_PARSER_GCODE_TARGET} PRIVATE -Wall -Wextra)

add_library(${UPC_PARSER_OVF_TARGET} STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ovf/ovf_worker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ovf/ovf_register.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ovf/build_processor_strategy.pb.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ovf/open_vector_format.pb.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/ovf/ovf_lut.pb.cc
)
add_library(upc::parser_ovf ALIAS ${UPC_PARSER_OVF_TARGET})

target_compile_features(${UPC_PARSER_OVF_TARGET} PUBLIC cxx_std_17)
target_link_libraries(${UPC_PARSER_OVF_TARGET} PUBLIC upc::parser protobuf)
target_compile_options(${UPC_PARSER_OVF_TARGET} PRIVATE -Wall -Wextra)

set(UPC_PARSER_TARGET ${UPC_PARSER_TARGET} PARENT_SCOPE)
set(UPC_PARSER_GCODE_TARGET ${UPC_PARSER_GCODE_TARGET} PARENT_SCOPE)
set(UPC_PARSER_OVF_TARGET ${UPC_PARSER_OVF_TARGET} PARENT_SCOPE)
