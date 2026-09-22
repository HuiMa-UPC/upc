set(UI_TARGET ui)

file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/core/src/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/pages/src/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/components/src/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/assets/src/*.c*"
)
if(NOT UI_SOURCES)
    message(FATAL_ERROR "ui: no sources under lib/")
endif()

add_library(${UI_TARGET} STATIC ${UI_SOURCES})
add_library(ui::port ALIAS ${UI_TARGET})

target_compile_definitions(${UI_TARGET} PRIVATE
    "UPC_SW_VERSION=\"${UPC_PRODUCT_VERSION}\""
)

target_compile_features(${UI_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UI_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/core/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/pages/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/components/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/assets/inc>
)

target_link_libraries(${UI_TARGET}
    PUBLIC
        middleware::port
        upc::ipc
        lvgl::port
)

if(CMAKE_SYSROOT)
    find_library(UPC_RGA_LIB NAMES rga
        PATHS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
else()
    find_library(UPC_RGA_LIB NAMES rga)
endif()

if(UPC_RGA_LIB)
    message(STATUS "ui: linking librga ${UPC_RGA_LIB}")
    target_link_libraries(${UI_TARGET} PRIVATE ${UPC_RGA_LIB})
    target_compile_definitions(${UI_TARGET} PRIVATE UPC_UI_HAS_RGA=1)
else()
    message(WARNING "ui: librga not found — camera preview RGA path disabled")
endif()

# FFmpeg（Route B）：video_player.cpp 直接使用 libav* 软解 MP4
if(CMAKE_SYSROOT)
    find_library(UPC_UI_AVFORMAT NAMES avformat
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_UI_AVCODEC NAMES avcodec
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_UI_AVUTIL NAMES avutil
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_UI_SWSCALE NAMES swscale
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_UI_SWRESAMPLE NAMES swresample
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
else()
    find_library(UPC_UI_AVFORMAT NAMES avformat)
    find_library(UPC_UI_AVCODEC NAMES avcodec)
    find_library(UPC_UI_AVUTIL NAMES avutil)
    find_library(UPC_UI_SWSCALE NAMES swscale)
    find_library(UPC_UI_SWRESAMPLE NAMES swresample)
endif()

if(UPC_UI_AVFORMAT AND UPC_UI_AVCODEC AND UPC_UI_AVUTIL AND UPC_UI_SWSCALE)
    message(STATUS "ui: linking ffmpeg libs for video_player")
    target_link_libraries(${UI_TARGET} PRIVATE
        ${UPC_UI_AVFORMAT}
        ${UPC_UI_AVCODEC}
        ${UPC_UI_AVUTIL}
        ${UPC_UI_SWSCALE}
        ${UPC_UI_SWRESAMPLE})
    target_compile_definitions(${UI_TARGET} PRIVATE UPC_UI_HAS_LIBAV=1)
else()
    message(WARNING "ui: ffmpeg libs not found — mp4 playback fallback to PNG sequence")
    target_compile_definitions(${UI_TARGET} PRIVATE UPC_UI_HAS_LIBAV=0)
endif()

# PNG 拖动预览异步解码（png_scrub_preview）
if(CMAKE_SYSROOT)
    find_library(UPC_UI_PNG NAMES png
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
else()
    find_library(UPC_UI_PNG NAMES png)
endif()
if(UPC_UI_PNG)
    message(STATUS "ui: linking libpng for png scrub preview ${UPC_UI_PNG}")
    target_link_libraries(${UI_TARGET} PRIVATE ${UPC_UI_PNG})
    target_compile_definitions(${UI_TARGET} PRIVATE UPC_UI_HAS_LIBPNG=1)
else()
    message(WARNING "ui: libpng not found — PNG scrub falls back to throttled sync decode")
    target_compile_definitions(${UI_TARGET} PRIVATE UPC_UI_HAS_LIBPNG=0)
endif()

target_compile_options(${UI_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

# ---------- UI 进程可执行文件 ----------
set(UPC_UI_PLATFORM_VARIANT "1.0")

if(DEFINED TOOLCHAIN)
    string(TOLOWER "${TOOLCHAIN}" _upc_toolchain_lc)
elseif(DEFINED CMAKE_TOOLCHAIN_FILE)
    get_filename_component(_upc_toolchain_name "${CMAKE_TOOLCHAIN_FILE}" NAME_WE)
    string(TOLOWER "${_upc_toolchain_name}" _upc_toolchain_lc)
endif()

if(DEFINED _upc_toolchain_lc AND _upc_toolchain_lc MATCHES "phx")
    set(UPC_UI_PLATFORM_VARIANT "2.0")
endif()

set(UPC_UI_RGA_STREAMING 0)
if(DEFINED _upc_toolchain_lc AND _upc_toolchain_lc MATCHES "phx")
    set(UPC_UI_RGA_STREAMING 1)
endif()
target_compile_definitions(${UI_TARGET} PRIVATE
    UPC_UI_RGA_STREAMING=${UPC_UI_RGA_STREAMING}
)

set(UPC_UI_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${UPC_UI_PLATFORM_VARIANT}")
set(UPC_UI_PLATFORM_CPP "${UPC_UI_PLATFORM_DIR}/platform.cpp")

if(NOT EXISTS "${UPC_UI_PLATFORM_CPP}")
    message(FATAL_ERROR "ui: platform source not found: ${UPC_UI_PLATFORM_CPP}")
endif()

message(STATUS
    "ui: platform variant=${UPC_UI_PLATFORM_VARIANT} (toolchain=${_upc_toolchain_lc}) "
    "rga_streaming=${UPC_UI_RGA_STREAMING}")

add_executable(rongsu_ui
    main.cpp
    ${UPC_UI_PLATFORM_CPP}
)

target_compile_features(rongsu_ui PRIVATE cxx_std_17)

target_include_directories(rongsu_ui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${UPC_UI_PLATFORM_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/core/inc
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(rongsu_ui
    PRIVATE
        ui::port
        upc::process
        upc::ipc
        upc::debug
)

set(UI_TARGET ${UI_TARGET} PARENT_SCOPE)
