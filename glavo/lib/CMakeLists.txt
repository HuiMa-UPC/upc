cmake_minimum_required(VERSION 3.16)

set(SERVICES_TARGET services)
set(SERVICES_PARSERS_TARGET services_parsers)

set(SERVICES_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/service_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/parser_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/parser_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/parser_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/glavo_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/tcp_sender.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/cal_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/protocol_mcu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/print_job_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/glavo/ring_buffer.cpp
)

set(SERVICES_PARSER_SOURCES
    #Gcode
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/gcode/gcode_worker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/gcode/gcode_register.cpp
    #OVF
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/ovf/ovf_worker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/ovf/ovf_register.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/ovf/build_processor_strategy.pb.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/ovf/open_vector_format.pb.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/ovf/ovf_lut.pb.cc
)

# 协议插件目录：parser/ 下各子目录（框架 .cpp 在 parser/ 根，不纳入此 GLOB）
file(GLOB PARSER_PLUGIN_DIRS LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/parser/*")

add_library(${SERVICES_TARGET} STATIC ${SERVICES_CORE_SOURCES})
add_library(services::port ALIAS ${SERVICES_TARGET})

message(STATUS "services core: glavo_service.cpp -> ${CMAKE_CURRENT_SOURCE_DIR}/glavo/glavo_service.cpp")

target_compile_features(${SERVICES_TARGET} PUBLIC cxx_std_17)

target_include_directories(${SERVICES_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glavo>
)

foreach(_parser_dir IN LISTS PARSER_PLUGIN_DIRS)
    if(IS_DIRECTORY "${_parser_dir}")
        target_include_directories(${SERVICES_TARGET}
            PUBLIC
                $<BUILD_INTERFACE:${_parser_dir}>)
    endif()
endforeach()

target_link_libraries(${SERVICES_TARGET}
    PUBLIC
        middleware::port
        upc::ipc
        upc::debug
)

# 各协议解析插件（独立静态库，链接时需 --whole-archive 以保证静态注册生效）
add_library(${SERVICES_PARSERS_TARGET} STATIC ${SERVICES_PARSER_SOURCES})
add_library(services::parsers ALIAS ${SERVICES_PARSERS_TARGET})

target_compile_features(${SERVICES_PARSERS_TARGET} PUBLIC cxx_std_17)
target_link_libraries(${SERVICES_PARSERS_TARGET} 
    PUBLIC 
        ${SERVICES_TARGET}
        protobuf
)

foreach(_parser_dir IN LISTS PARSER_PLUGIN_DIRS)
    if(IS_DIRECTORY "${_parser_dir}")
        target_include_directories(${SERVICES_PARSERS_TARGET}
            PUBLIC
                $<BUILD_INTERFACE:${_parser_dir}>)
    endif()
endforeach()

foreach(_svc_tgt IN ITEMS ${SERVICES_TARGET} ${SERVICES_PARSERS_TARGET})
    target_compile_options(${_svc_tgt}
        PRIVATE
            -Wall -Wextra
            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Release>:-O2>
    )
endforeach()

set(SERVICES_TARGET ${SERVICES_TARGET} PARENT_SCOPE)
set(SERVICES_PARSERS_TARGET ${SERVICES_PARSERS_TARGET} PARENT_SCOPE)

if(SERVICES_STANDALONE_BUILD)
    message(STATUS "services v1.0.0 (standalone)")
endif()
