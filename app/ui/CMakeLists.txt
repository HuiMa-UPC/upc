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

target_compile_options(${UI_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

# ---------- UI 进程可执行文件 ----------
add_executable(rongsu_ui
    main.cpp
    platform.cpp
)

target_compile_features(rongsu_ui PRIVATE cxx_std_17)

target_include_directories(rongsu_ui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
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
