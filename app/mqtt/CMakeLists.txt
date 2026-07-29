set(MQTT_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_executable(rongsu_mqtt
    main.cpp
    ${MQTT_LIB_DIR}/mqtt_service.cpp
    ${MQTT_LIB_DIR}/mqtt_config.cpp
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
)

target_compile_options(rongsu_mqtt
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
