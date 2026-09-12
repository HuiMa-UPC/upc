set(VISUAL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

if(TOOLCHAIN STREQUAL "phx-buildroot-toolchain")
    # phx sysroot currently provides OpenCV 4.9.x; keep a minimum floor for ABI safety.
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
    if(OpenCV_VERSION VERSION_LESS "4.5.4")
        message(FATAL_ERROR
            "OpenCV >= 4.5.4 required for visual module on phx (found ${OpenCV_VERSION})")
    endif()
else()
    # Keep existing strict requirement for non-phx toolchains (including k1mini).
    find_package(OpenCV 4.5.4 EXACT REQUIRED COMPONENTS core imgproc imgcodecs)
endif()

add_executable(rongsu_visual
    main.cpp
    ${VISUAL_LIB_DIR}/first_layer_detector.cpp
    ${VISUAL_LIB_DIR}/red_dot_detector.cpp
    ${VISUAL_LIB_DIR}/visual_service.cpp
)

target_compile_features(rongsu_visual PRIVATE cxx_std_17)

target_include_directories(rongsu_visual
    PRIVATE
        ${VISUAL_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
        ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(rongsu_visual
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
        ${OpenCV_LIBS}
        ${CMAKE_DL_LIBS}
)

target_compile_options(rongsu_visual
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

option(UPC_BUILD_FIRST_LAYER_SMOKE "Build the offline RKNN first-layer smoke tool" ON)
if(UPC_BUILD_FIRST_LAYER_SMOKE)
    add_executable(rknn_first_layer_smoke
        first_layer_smoke.cpp
        ${VISUAL_LIB_DIR}/first_layer_detector.cpp
    )
    target_compile_features(rknn_first_layer_smoke PRIVATE cxx_std_17)
    target_include_directories(rknn_first_layer_smoke
        PRIVATE
            ${VISUAL_LIB_DIR}
            ${CMAKE_SOURCE_DIR}/common/fs_manifest
            ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(rknn_first_layer_smoke
        PRIVATE
            ${OpenCV_LIBS}
            ${CMAKE_DL_LIBS}
    )
    target_compile_options(rknn_first_layer_smoke PRIVATE -Wall -Wextra)
endif()
