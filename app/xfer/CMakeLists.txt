set(XFR_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_xfr
    main.cpp
    ${XFR_LIB_DIR}/xfr_service.cpp
    ${XFR_LIB_DIR}/xfr_config.cpp
    ${XFR_LIB_DIR}/http_server.cpp
    ${XFR_LIB_DIR}/upload_manager.cpp
    ${XFR_LIB_DIR}/auth.cpp
    ${XFR_LIB_DIR}/mdns_publisher.cpp
)

target_compile_features(rongsu_xfr PRIVATE cxx_std_17)

target_include_directories(rongsu_xfr
    PRIVATE
        ${XFR_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
)

find_package(OpenSSL REQUIRED)

target_link_libraries(rongsu_xfr
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
        OpenSSL::Crypto
)

target_compile_options(rongsu_xfr
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
