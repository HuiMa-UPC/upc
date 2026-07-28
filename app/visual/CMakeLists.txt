set(VISUAL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_visual
    main.cpp
    ${VISUAL_LIB_DIR}/visual_service.cpp
)

target_compile_features(rongsu_visual PRIVATE cxx_std_17)

target_include_directories(rongsu_visual
    PRIVATE
        ${VISUAL_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(rongsu_visual
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
)

target_compile_options(rongsu_visual
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
