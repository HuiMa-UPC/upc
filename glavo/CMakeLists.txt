add_subdirectory(lib)

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
