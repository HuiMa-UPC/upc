set(VISUAL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

find_package(OpenCV 4.5.4 EXACT REQUIRED COMPONENTS core imgproc imgcodecs)

add_executable(rongsu_visual
    main.cpp
    ${VISUAL_LIB_DIR}/first_layer_detector.cpp
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
            ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(rknn_first_layer_smoke
        PRIVATE
            ${OpenCV_LIBS}
            ${CMAKE_DL_LIBS}
    )
    target_compile_options(rknn_first_layer_smoke PRIVATE -Wall -Wextra)
endif()
