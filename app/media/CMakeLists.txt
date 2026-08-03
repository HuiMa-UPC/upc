set(MEDIA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_media
    main.cpp
    ${MEDIA_LIB_DIR}/media_service.cpp
    ${MEDIA_LIB_DIR}/print_video_recorder.cpp
    ${MEDIA_LIB_DIR}/print_h264_recorder.cpp
    ${MEDIA_LIB_DIR}/clean_video_recorder.cpp
    ${MEDIA_LIB_DIR}/timelapse_util.cpp
)

target_compile_features(rongsu_media PRIVATE cxx_std_17)

target_include_directories(rongsu_media
    PRIVATE
        ${MEDIA_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(rongsu_media
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
)

if(CMAKE_SYSROOT)
    find_library(UPC_MEDIA_RGA_LIB NAMES rga
        PATHS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
else()
    find_library(UPC_MEDIA_RGA_LIB NAMES rga)
endif()

if(UPC_MEDIA_RGA_LIB)
    message(STATUS "media: linking librga ${UPC_MEDIA_RGA_LIB}")
    target_link_libraries(rongsu_media PRIVATE ${UPC_MEDIA_RGA_LIB})
    target_compile_definitions(rongsu_media PRIVATE UPC_CAMERA_HAS_RGA=1)
else()
    message(WARNING "media: librga not found — raw thumbnail RGA path disabled")
endif()

if(CMAKE_SYSROOT)
    find_path(UPC_MEDIA_MPP_INCLUDE_DIR NAMES rk_mpi.h
        PATHS
            ${CMAKE_SYSROOT}/usr/include
            ${CMAKE_SYSROOT}/usr/include/rockchip
            ${CMAKE_SYSROOT}/usr/include/mpp
        NO_DEFAULT_PATH)
    find_library(UPC_MEDIA_MPP_LIB NAMES rockchip_mpp mpp
        PATHS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
else()
    find_path(UPC_MEDIA_MPP_INCLUDE_DIR NAMES rk_mpi.h
        PATHS
            /usr/include
            /usr/include/rockchip
            /usr/include/mpp)
    find_library(UPC_MEDIA_MPP_LIB NAMES rockchip_mpp mpp)
endif()

if(UPC_MEDIA_MPP_INCLUDE_DIR AND UPC_MEDIA_MPP_LIB)
    message(STATUS "media: enabling RKMPP include=${UPC_MEDIA_MPP_INCLUDE_DIR} lib=${UPC_MEDIA_MPP_LIB}")
    target_include_directories(rongsu_media PRIVATE ${UPC_MEDIA_MPP_INCLUDE_DIR})
    target_link_libraries(rongsu_media PRIVATE ${UPC_MEDIA_MPP_LIB})
    target_compile_definitions(rongsu_media PRIVATE UPC_MEDIA_HAS_MPP=1)
else()
    message(WARNING "media: RKMPP not found — print_h264 recorder disabled")
endif()

target_compile_options(rongsu_media
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
