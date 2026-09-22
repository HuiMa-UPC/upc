set(MEDIA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_media
    main.cpp
    ${MEDIA_LIB_DIR}/media_service.cpp
    ${MEDIA_LIB_DIR}/print_video_recorder.cpp
    ${MEDIA_LIB_DIR}/print_h264_recorder.cpp
    ${MEDIA_LIB_DIR}/clean_video_recorder.cpp
    ${MEDIA_LIB_DIR}/h264_stream_encoder.cpp
    ${MEDIA_LIB_DIR}/live_rtsp_pusher.cpp
    ${MEDIA_LIB_DIR}/local_rtsp_server.cpp
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

if(TOOLCHAIN STREQUAL "phx-buildroot-toolchain")
    message(STATUS "media: enable PHX streaming raw-thumb path")
    target_compile_definitions(rongsu_media PRIVATE UPC_MEDIA_PHX_RTHM_STREAMING=1)
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

# 相册 / 延时层拍 / 清粉抽帧：CPU JPEG（libturbojpeg）。不走 MPP，避免和 H.264 抢 VPU。
if(CMAKE_SYSROOT)
    find_path(UPC_MEDIA_TURBOJPEG_INCLUDE NAMES turbojpeg.h
        PATHS ${CMAKE_SYSROOT}/usr/include NO_DEFAULT_PATH)
    find_library(UPC_MEDIA_TURBOJPEG_LIB NAMES turbojpeg
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
else()
    find_path(UPC_MEDIA_TURBOJPEG_INCLUDE NAMES turbojpeg.h)
    find_library(UPC_MEDIA_TURBOJPEG_LIB NAMES turbojpeg)
endif()
if(UPC_MEDIA_TURBOJPEG_INCLUDE AND UPC_MEDIA_TURBOJPEG_LIB)
    message(STATUS "media: linking libturbojpeg ${UPC_MEDIA_TURBOJPEG_LIB}")
    target_include_directories(rongsu_media PRIVATE ${UPC_MEDIA_TURBOJPEG_INCLUDE})
    target_link_libraries(rongsu_media PRIVATE ${UPC_MEDIA_TURBOJPEG_LIB})
    target_compile_definitions(rongsu_media PRIVATE UPC_MEDIA_HAS_TURBOJPEG=1)
else()
    message(FATAL_ERROR "media: libturbojpeg not found (headers + libturbojpeg.so required for still JPEG)")
endif()

# live555：本地局域网 RTSP Server。部分工具链的 sysroot 未打包该库，
# 找不到时整个 LocalRtspServer 退化为空实现（UPC_MEDIA_HAS_LIVE555 未定义）。
# 库名与探测用头文件一一对应（groupsock 目录下的头是 Groupsock.hh，首字母大写）
set(UPC_LIVE555_MODULES liveMedia groupsock BasicUsageEnvironment UsageEnvironment)
set(UPC_LIVE555_HEADERS liveMedia.hh Groupsock.hh BasicUsageEnvironment.hh UsageEnvironment.hh)
set(UPC_LIVE555_INCLUDE_DIRS "")
set(UPC_LIVE555_LIBS "")
set(UPC_LIVE555_OK TRUE)
list(LENGTH UPC_LIVE555_MODULES UPC_LIVE555_COUNT)
math(EXPR UPC_LIVE555_LAST "${UPC_LIVE555_COUNT} - 1")
foreach(idx RANGE ${UPC_LIVE555_LAST})
    list(GET UPC_LIVE555_MODULES ${idx} mod)
    list(GET UPC_LIVE555_HEADERS ${idx} hdr)
    if(CMAKE_SYSROOT)
        find_path(UPC_LIVE555_INC_${mod} NAMES ${hdr}
            PATHS ${CMAKE_SYSROOT}/usr/include/${mod}
            NO_DEFAULT_PATH)
        find_library(UPC_LIVE555_LIB_${mod} NAMES ${mod}
            PATHS
                ${CMAKE_SYSROOT}/usr/lib
                ${CMAKE_SYSROOT}/lib
            NO_DEFAULT_PATH)
    else()
        find_path(UPC_LIVE555_INC_${mod} NAMES ${hdr}
            PATHS /usr/include/${mod})
        find_library(UPC_LIVE555_LIB_${mod} NAMES ${mod})
    endif()
    if(UPC_LIVE555_INC_${mod} AND UPC_LIVE555_LIB_${mod})
        list(APPEND UPC_LIVE555_INCLUDE_DIRS ${UPC_LIVE555_INC_${mod}})
        list(APPEND UPC_LIVE555_LIBS ${UPC_LIVE555_LIB_${mod}})
    else()
        set(UPC_LIVE555_OK FALSE)
    endif()
endforeach()

if(UPC_LIVE555_OK)
    message(STATUS "media: enabling live555 libs=${UPC_LIVE555_LIBS}")
    target_include_directories(rongsu_media PRIVATE ${UPC_LIVE555_INCLUDE_DIRS})
    # 链接顺序有依赖：liveMedia → groupsock → BasicUsageEnvironment → UsageEnvironment
    target_link_libraries(rongsu_media PRIVATE ${UPC_LIVE555_LIBS})
    target_compile_definitions(rongsu_media PRIVATE UPC_MEDIA_HAS_LIVE555=1)
else()
    message(WARNING "media: live555 not found — local RTSP server disabled")
endif()

target_compile_options(rongsu_media
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
