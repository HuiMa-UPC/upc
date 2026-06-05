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
)

if(TARGET services_parsers)
    target_link_libraries(rongsu_glavo
        PRIVATE
            -Wl,--whole-archive
            services
            services_parsers
            -Wl,--no-whole-archive
    )
elseif(TARGET services)
    target_link_libraries(rongsu_glavo
        PRIVATE
            -Wl,--whole-archive
            services
            -Wl,--no-whole-archive
    )
endif()
