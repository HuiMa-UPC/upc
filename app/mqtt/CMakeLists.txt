set(MQTT_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_mqtt
    main.cpp
    ${MQTT_LIB_DIR}/mqtt_service.cpp
    ${MQTT_LIB_DIR}/mqtt_service_telemetry.cpp
    ${MQTT_LIB_DIR}/mqtt_service_command.cpp
    ${MQTT_LIB_DIR}/mqtt_service_ota.cpp
    ${MQTT_LIB_DIR}/mqtt_config.cpp
    ${MQTT_LIB_DIR}/device_activation.cpp
)

target_compile_features(rongsu_mqtt PRIVATE cxx_std_17)

target_include_directories(rongsu_mqtt
    PRIVATE
        ${MQTT_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
)

find_package(eclipse-paho-mqtt-c CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(rongsu_mqtt
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
        eclipse-paho-mqtt-c::paho-mqtt3cs
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_options(rongsu_mqtt
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

target_compile_definitions(rongsu_mqtt
    PRIVATE
        "UPC_SW_VERSION=\"${UPC_PRODUCT_VERSION}\""
)
