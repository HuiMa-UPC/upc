set(XFR_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_xfr
    main.cpp
    ${XFR_LIB_DIR}/xfr_service.cpp
    ${XFR_LIB_DIR}/xfr_config.cpp
    ${XFR_LIB_DIR}/http_server.cpp
    ${XFR_LIB_DIR}/upload_manager.cpp
    ${XFR_LIB_DIR}/transfer_3mf_importer.cpp
    ${XFR_LIB_DIR}/transfer_thumbnail_generator.cpp
    ${XFR_LIB_DIR}/auth.cpp
    ${XFR_LIB_DIR}/mdns_publisher.cpp
)

target_compile_features(rongsu_xfr PRIVATE cxx_std_17)

target_include_directories(rongsu_xfr
    PRIVATE
        ${XFR_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common
        ${CMAKE_SOURCE_DIR}/common/fs_manifest
        ${CMAKE_SOURCE_DIR}/common/process
)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

if(CMAKE_SYSROOT)
    find_path(UPC_XFR_PNG_INCLUDE_DIR NAMES png.h
        PATHS ${CMAKE_SYSROOT}/usr/include NO_DEFAULT_PATH)
    find_library(UPC_XFR_PNG_LIB NAMES png png16
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
    find_library(UPC_XFR_RGA_LIB NAMES rga
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib NO_DEFAULT_PATH)
else()
    find_path(UPC_XFR_PNG_INCLUDE_DIR NAMES png.h)
    find_library(UPC_XFR_PNG_LIB NAMES png png16)
    find_library(UPC_XFR_RGA_LIB NAMES rga)
endif()
if(NOT UPC_XFR_PNG_INCLUDE_DIR OR NOT UPC_XFR_PNG_LIB)
    message(FATAL_ERROR "xfr: libpng headers/library not found (required for 3MF thumbnails)")
endif()
target_include_directories(rongsu_xfr PRIVATE ${UPC_XFR_PNG_INCLUDE_DIR})

target_link_libraries(rongsu_xfr
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
        -Wl,--whole-archive
        upc::parser_ovf
        -Wl,--no-whole-archive
        upc::parser
        OpenSSL::Crypto
        ZLIB::ZLIB
        ${UPC_XFR_PNG_LIB}
)

if(UPC_XFR_RGA_LIB)
    message(STATUS "xfr: linking librga for 3MF thumbnails ${UPC_XFR_RGA_LIB}")
    target_link_libraries(rongsu_xfr PRIVATE ${UPC_XFR_RGA_LIB})
    target_compile_definitions(rongsu_xfr PRIVATE UPC_XFR_HAS_RGA=1)
else()
    message(WARNING "xfr: librga not found — 3MF thumbnails use CPU conversion")
    target_compile_definitions(rongsu_xfr PRIVATE UPC_XFR_HAS_RGA=0)
endif()

target_compile_options(rongsu_xfr
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
