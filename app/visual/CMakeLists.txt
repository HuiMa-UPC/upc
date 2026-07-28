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

if(CMAKE_SYSROOT)
    set(_UPC_OPENCV_HINTS
        "${CMAKE_SYSROOT}/usr/lib/cmake/opencv4"
        "${CMAKE_SYSROOT}/lib/cmake/opencv4")
endif()

find_package(OpenCV QUIET COMPONENTS core imgproc HINTS ${_UPC_OPENCV_HINTS})
if(OpenCV_FOUND)
    message(STATUS "visual: linking OpenCV ${OpenCV_VERSION}")
    target_include_directories(rongsu_visual PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(rongsu_visual PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(rongsu_visual PRIVATE UPC_VISUAL_HAS_OPENCV=1)
else()
    message(WARNING "visual: OpenCV not found in current toolchain sysroot")
    target_compile_definitions(rongsu_visual PRIVATE UPC_VISUAL_HAS_OPENCV=0)
endif()

target_compile_options(rongsu_visual
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
