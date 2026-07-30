set(MEDIA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_media
    main.cpp
    ${MEDIA_LIB_DIR}/media_service.cpp
    ${MEDIA_LIB_DIR}/print_video_recorder.cpp
    ${MEDIA_LIB_DIR}/print_h264_recorder.cpp
    ${MEDIA_LIB_DIR}/clean_video_recorder.cpp
)

target_compile_features(rongsu_media PRIVATE cxx_std_17)

target_include_directories(rongsu_media
    PRIVATE
        ${MEDIA_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/app/camera/lib
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

target_compile_options(rongsu_media
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
