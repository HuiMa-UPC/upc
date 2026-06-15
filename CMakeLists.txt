cmake_minimum_required(VERSION 3.16)

set(TOOLCHAIN k1mini-buildroot-toolchain)
# set(TOOLCHAIN atk-dlrk356x-toolchain)
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/${TOOLCHAIN}.cmake"
    CACHE FILEPATH "Cross toolchain (optional)")

project(rongsu_upc
        VERSION 1.0.0
        LANGUAGES C CXX
)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Toolchain: ${TOOLCHAIN}")
message(STATUS "Layout: common/ + parser/ + protocol/ + ui/ + glavo/")
message(STATUS "Binaries: rongsu_ui, rongsu_glavo test_camera")
message(STATUS "========================================")

add_compile_options(-Wall -Wextra -O3)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

option(UPC_DEBUG "Enable unified debug logging (middleware+services)" ON)

add_subdirectory(common)
add_subdirectory(parser)
add_subdirectory(protocol)
add_subdirectory(3rdparty/lv_port_linux-release-v9.5)
add_subdirectory(ui)
add_subdirectory(glavo)
add_subdirectory(test)

