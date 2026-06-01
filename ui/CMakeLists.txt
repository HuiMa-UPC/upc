set(UI_TARGET ui)

file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/pages/src/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/src/*.c*"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/src/*.c*"
)
if(NOT UI_SOURCES)
    message(FATAL_ERROR "ui: no source files found under core/, pages/, components/, assets/")
endif()

add_library(${UI_TARGET} STATIC ${UI_SOURCES})
add_library(ui::port ALIAS ${UI_TARGET})

target_compile_features(${UI_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UI_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pages/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/components/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/assets/inc>
        $<INSTALL_INTERFACE:include/ui>
        $<INSTALL_INTERFACE:include/ui/core>
        $<INSTALL_INTERFACE:include/ui/pages>
        $<INSTALL_INTERFACE:include/ui/components>
        $<INSTALL_INTERFACE:include/ui/assets>
)

target_link_libraries(${UI_TARGET}
    PUBLIC
        middleware::port
        lvgl::port
)

target_compile_options(${UI_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

set(UI_TARGET ${UI_TARGET} PARENT_SCOPE)

if(UI_STANDALONE)
    message(STATUS "ui v1.0.0 (standalone)")
endif()
