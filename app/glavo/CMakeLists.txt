set(UPC_GLAVO_TARGET upc_glavo)
set(GLAVO_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

set(UPC_GLAVO_SOURCES
    ${GLAVO_LIB_DIR}/galvo_motion_encode.cpp
    ${GLAVO_LIB_DIR}/glavo_service.cpp
    ${GLAVO_LIB_DIR}/optical_path_check.cpp
    ${GLAVO_LIB_DIR}/height_lut_2d.cpp
    ${GLAVO_LIB_DIR}/height_calibration.cpp
    ${GLAVO_LIB_DIR}/plate_height_measure.cpp
    ${GLAVO_LIB_DIR}/mcu_link.cpp
    ${GLAVO_LIB_DIR}/soc_sensors.cpp
    ${GLAVO_LIB_DIR}/spi_status_led.cpp
    ${GLAVO_LIB_DIR}/spi_pm25_lcd.cpp
    ${GLAVO_LIB_DIR}/mcu_ota_session.cpp
    ${GLAVO_LIB_DIR}/mcu_serial_ota_session.cpp
    ${GLAVO_LIB_DIR}/print_job_state.cpp
    ${GLAVO_LIB_DIR}/print_video_notify.cpp
    ${GLAVO_LIB_DIR}/wifi_manager.cpp
    ${GLAVO_LIB_DIR}/n2_ble_parse.cpp
    ${GLAVO_LIB_DIR}/n2_ble_transport.cpp
    ${GLAVO_LIB_DIR}/n2_ble_client.cpp
)

add_library(${UPC_GLAVO_TARGET} STATIC ${UPC_GLAVO_SOURCES})
add_library(upc::glavo ALIAS ${UPC_GLAVO_TARGET})

target_compile_features(${UPC_GLAVO_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_GLAVO_TARGET}
    PUBLIC
        ${GLAVO_LIB_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common/ipc
)

# ---------- wpa_supplicant 控制接口（WiFi：libwpa_client / wpa_ctrl.h） ----------
if(CMAKE_SYSROOT)
    find_library(UPC_WPA_CLIENT_LIB NAMES wpa_client
        PATHS
            ${CMAKE_SYSROOT}/usr/lib
            ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
else()
    find_library(UPC_WPA_CLIENT_LIB NAMES wpa_client)
endif()

if(NOT UPC_WPA_CLIENT_LIB)
    message(FATAL_ERROR "libwpa_client not found in sysroot (required by WifiManager)")
endif()

# ---------- BlueZ / libdbus-1（制氮机 BLE；没有则空跑） ----------
set(UPC_GLAVO_HAS_BLUEZ 0)
if(CMAKE_SYSROOT)
    find_path(UPC_DBUS_INCLUDE_DIR dbus/dbus.h
        PATHS ${CMAKE_SYSROOT}/usr/include/dbus-1.0
        NO_DEFAULT_PATH)
    find_path(UPC_DBUS_ARCH_INCLUDE_DIR dbus/dbus-arch-deps.h
        PATHS
            ${CMAKE_SYSROOT}/usr/lib/dbus-1.0/include
            ${CMAKE_SYSROOT}/usr/lib64/dbus-1.0/include
            ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/dbus-1.0/include
        NO_DEFAULT_PATH)
    find_library(UPC_DBUS_LIB NAMES dbus-1
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib
            ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu
        NO_DEFAULT_PATH)
else()
    find_path(UPC_DBUS_INCLUDE_DIR dbus/dbus.h
        PATHS /usr/include/dbus-1.0)
    find_path(UPC_DBUS_ARCH_INCLUDE_DIR dbus/dbus-arch-deps.h
        PATHS
            /usr/lib/dbus-1.0/include
            /usr/lib64/dbus-1.0/include
            /usr/lib/x86_64-linux-gnu/dbus-1.0/include
            /usr/lib/aarch64-linux-gnu/dbus-1.0/include)
    find_library(UPC_DBUS_LIB NAMES dbus-1)
endif()
if(UPC_DBUS_INCLUDE_DIR AND UPC_DBUS_ARCH_INCLUDE_DIR AND UPC_DBUS_LIB)
    set(UPC_GLAVO_HAS_BLUEZ 1)
    message(STATUS "Glavo N2 BLE: libdbus-1 enabled")
else()
    message(WARNING "Glavo N2 BLE: libdbus-1 not found; UPC_GLAVO_HAS_BLUEZ=0 stub")
endif()

target_link_libraries(${UPC_GLAVO_TARGET}
    PUBLIC
        upc::protocol
        upc::parser
        middleware::port
        upc::ipc
        upc::debug
    PRIVATE
        ${UPC_WPA_CLIENT_LIB}
)
if(UPC_GLAVO_HAS_BLUEZ)
    target_include_directories(${UPC_GLAVO_TARGET} PRIVATE
        ${UPC_DBUS_INCLUDE_DIR}
        ${UPC_DBUS_ARCH_INCLUDE_DIR})
    # STATIC 库的 PRIVATE 依赖不会传到 rongsu_glavo
    target_link_libraries(${UPC_GLAVO_TARGET} PUBLIC ${UPC_DBUS_LIB})
endif()
target_compile_definitions(${UPC_GLAVO_TARGET} PRIVATE
    UPC_GLAVO_HAS_BLUEZ=${UPC_GLAVO_HAS_BLUEZ})

target_compile_options(${UPC_GLAVO_TARGET}
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

# ---------- Glavo 进程可执行文件 ----------
add_executable(rongsu_glavo main.cpp)

target_compile_features(rongsu_glavo PRIVATE cxx_std_17)

target_include_directories(rongsu_glavo
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(rongsu_glavo
    PRIVATE
        upc::process
        upc::ipc
        upc::debug
        -Wl,--whole-archive
        upc::parser_gcode
        upc::parser_ovf
        -Wl,--no-whole-archive
        upc::glavo
        upc::parser
)

