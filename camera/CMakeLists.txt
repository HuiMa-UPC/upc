set(CAMERA_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_camera
    main.cpp
    ${CAMERA_LIB_DIR}/camera_service.cpp
    ${CAMERA_LIB_DIR}/vi_capture.cpp
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

target_compile_options(rongsu_camera
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
