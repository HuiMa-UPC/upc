#ifndef SERIAL_ENV_HPP
#define SERIAL_ENV_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float oxygen_percent;
    int16_t pressure_mbar;
    float temperature_c;
    float humidity_percent;
    float pm25_ug_m3;
    uint8_t powder_present;
    float substrate_temp_c;
    uint16_t hall_bitmap;
} serial_sensor_report_t;

typedef enum {
    SERIAL_SENSOR_ERR_NONE = 0,
    SERIAL_SENSOR_ERR_PARAM,
    SERIAL_SENSOR_ERR_TOO_SHORT,
    SERIAL_SENSOR_ERR_SOF,
    SERIAL_SENSOR_ERR_PORT_CMD,
    SERIAL_SENSOR_ERR_TLV,
    SERIAL_SENSOR_ERR_INCOMPLETE,
    SERIAL_SENSOR_ERR_CHECKSUM,
} serial_sensor_err_t;

int serial_frame_is_sensor_report(const uint8_t* data, size_t len);

int serial_parse_sensor_report_detail(const uint8_t* data, size_t len,
                                      serial_sensor_report_t* out,
                                      serial_sensor_err_t* err_out);

int serial_parse_sensor_report(const uint8_t* data, size_t len,
                               serial_sensor_report_t* out);

const char* serial_sensor_err_string(serial_sensor_err_t err);

#ifdef __cplusplus
}
#endif

#endif
