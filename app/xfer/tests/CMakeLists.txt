cmake_minimum_required(VERSION 3.16)
project(upc_xfr_3mf_smoke LANGUAGES CXX)

find_package(ZLIB REQUIRED)
find_path(UPC_TEST_PNG_INCLUDE_DIR NAMES png.h REQUIRED)
find_library(UPC_TEST_PNG_LIBRARY NAMES png png16 REQUIRED)

add_executable(transfer_3mf_smoke
    transfer_3mf_smoke.cpp
    ../lib/transfer_3mf_importer.cpp
    ../lib/transfer_thumbnail_generator.cpp
    ../../../common/fs_manifest/gcode_manifest.cpp
    ../../../common/debug/rs_log.cpp
)

target_compile_features(transfer_3mf_smoke PRIVATE cxx_std_17)
target_include_directories(transfer_3mf_smoke PRIVATE
    ../lib
    ../../../common
    ../../../common/debug
    ../../../common/fs_manifest
    ${UPC_TEST_PNG_INCLUDE_DIR}
)
target_compile_definitions(transfer_3mf_smoke PRIVATE
    UPC_DEBUG=0
    UPC_LOCAL_GCODE_DIR="${CMAKE_CURRENT_BINARY_DIR}/local"
    UPC_LOCAL_GCODE_CONFIG="${CMAKE_CURRENT_BINARY_DIR}/local/config.json"
    UPC_LOCAL_GCODE_THUMB_DIR="${CMAKE_CURRENT_BINARY_DIR}/image/gcode"
    UPC_XFR_HAS_RGA=0
)
target_link_libraries(transfer_3mf_smoke PRIVATE ZLIB::ZLIB ${UPC_TEST_PNG_LIBRARY})
