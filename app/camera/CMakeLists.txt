set(CAMERA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_camera
    main.cpp
    ${CAMERA_LIB_DIR}/camera_service.cpp
    ${CAMERA_LIB_DIR}/vi_capture.cpp
    ${CAMERA_LIB_DIR}/camera_dma_frame_source.cpp
)

target_compile_features(rongsu_camera PRIVATE cxx_std_17)

target_include_directories(rongsu_camera
    PRIVATE
        ${CAMERA_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(rongsu_camera
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
)

if(CMAKE_SYSROOT)
    find_library(UPC_RKAIQ_LIB NAMES rkaiq
        PATHS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
else()
    find_library(UPC_RKAIQ_LIB NAMES rkaiq)
endif()

if(UPC_RKAIQ_LIB)
    message(STATUS "camera: linking librkaiq ${UPC_RKAIQ_LIB}")
    target_link_libraries(rongsu_camera PRIVATE ${UPC_RKAIQ_LIB})
    target_include_directories(rongsu_camera
        PRIVATE
            ${CMAKE_SYSROOT}/usr/include
            ${CMAKE_SYSROOT}/usr/include/rkaiq
            ${CMAKE_SYSROOT}/usr/include/rkaiq/common
            ${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI2
            ${CMAKE_SYSROOT}/usr/include/rkaiq/algos
            ${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser
            ${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser_v2
            ${CMAKE_SYSROOT}/usr/include/rkaiq/xcore
    )
else()
    message(WARNING "camera: librkaiq not found — build may fail on target")
endif()

if(CMAKE_SYSROOT)
    find_library(UPC_CAMERA_RGA_LIB NAMES rga
        PATHS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
else()
    find_library(UPC_CAMERA_RGA_LIB NAMES rga)
endif()

if(UPC_CAMERA_RGA_LIB)
    message(STATUS "camera: linking librga ${UPC_CAMERA_RGA_LIB}")
    target_link_libraries(rongsu_camera PRIVATE ${UPC_CAMERA_RGA_LIB})
    target_compile_definitions(rongsu_camera PRIVATE UPC_CAMERA_HAS_RGA=1)
else()
    message(WARNING "camera: librga not found — raw thumbnail RGA path disabled")
endif()

if(CMAKE_SYSROOT)
    find_library(UPC_CAMERA_AVFORMAT NAMES avformat
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_CAMERA_AVCODEC NAMES avcodec
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_CAMERA_AVUTIL NAMES avutil
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_CAMERA_SWSCALE NAMES swscale
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
else()
    find_library(UPC_CAMERA_AVFORMAT NAMES avformat)
    find_library(UPC_CAMERA_AVCODEC NAMES avcodec)
    find_library(UPC_CAMERA_AVUTIL NAMES avutil)
    find_library(UPC_CAMERA_SWSCALE NAMES swscale)
endif()

if(UPC_CAMERA_AVFORMAT AND UPC_CAMERA_AVCODEC AND UPC_CAMERA_AVUTIL AND UPC_CAMERA_SWSCALE)
    message(STATUS "camera: linking ffmpeg libs for demo video source")
    target_link_libraries(rongsu_camera PRIVATE
        ${UPC_CAMERA_AVFORMAT}
        ${UPC_CAMERA_AVCODEC}
        ${UPC_CAMERA_AVUTIL}
        ${UPC_CAMERA_SWSCALE})
    target_compile_definitions(rongsu_camera PRIVATE UPC_CAMERA_HAS_LIBAV=1)
else()
    message(WARNING "camera: ffmpeg libs not found — demo video stream source disabled")
    target_compile_definitions(rongsu_camera PRIVATE UPC_CAMERA_HAS_LIBAV=0)
endif()

target_compile_options(rongsu_camera
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
