set(UPC_GLAVO_TARGET upc_glavo)
set(GLAVO_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

set(UPC_GLAVO_SOURCES
    ${GLAVO_LIB_DIR}/service_base.cpp
    ${GLAVO_LIB_DIR}/glavo_service.cpp
    ${GLAVO_LIB_DIR}/glavo_parser_bridge.cpp
    ${GLAVO_LIB_DIR}/mcu_link.cpp
    ${GLAVO_LIB_DIR}/print_job_state.cpp
    ${GLAVO_LIB_DIR}/print_video_notify.cpp
)

add_library(${UPC_GLAVO_TARGET} STATIC ${UPC_GLAVO_SOURCES})
add_library(upc::glavo ALIAS ${UPC_GLAVO_TARGET})

target_compile_features(${UPC_GLAVO_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_GLAVO_TARGET}
    PUBLIC
        ${GLAVO_LIB_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common/ipc
)

target_link_libraries(${UPC_GLAVO_TARGET}
    PUBLIC
        upc::protocol
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

# ---------- Glavo 进程可执行文件 ----------
add_executable(rongsu_glavo main.cpp)

target_compile_features(rongsu_glavo PRIVATE cxx_std_17)

target_include_directories(rongsu_glavo
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(rongsu_glavo
    PRIVATE
        upc::process
        upc::ipc
        upc::debug
        -Wl,--whole-archive
        upc::parser_gcode
        upc::parser_ovf
        -Wl,--no-whole-archive
        upc::glavo
        upc::parser
)
