#ifndef PROTOCOL_MCU_HPP
#define PROTOCOL_MCU_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

int build_motion_frame(const uint8_t* subpkts11, int n,
                       uint8_t* out, size_t out_size);

int build_heartbeat_frame(uint8_t* out, size_t out_size);

int build_reset_frame(uint8_t* out, size_t out_size);

typedef struct {
    uint16_t cmd_rate;
    uint16_t data_rate;
} tcp_response_t;

int parse_tcp_response(const uint8_t* data, size_t len, tcp_response_t* resp);

int serial_build_print_request(uint8_t* out, size_t out_size);
int serial_build_print_start(uint16_t layer_height_um, uint8_t* out, size_t out_size);
int serial_build_layer_done(uint16_t next_layer_height_um, uint8_t* out, size_t out_size);
int serial_build_print_finished(uint8_t* out, size_t out_size);
int serial_build_print_terminate(uint8_t* out, size_t out_size);

typedef enum {
    SERIAL_EVT_NONE = 0,
    SERIAL_EVT_PRINT_READY,
    SERIAL_EVT_PRINT_CONDITION_OK,
} serial_event_t;

/** MCU 主动上报：CMD_SNESOR_VALUE_REPORT (0x9B)，TLV_SENSOR_REPORT_1ST */
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

serial_event_t serial_parse_frame(const uint8_t* data, size_t len);

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

/** @return 1 帧头为环境上报 (5A A5 BB A5 9B)，0 否 */
int serial_frame_is_sensor_report(const uint8_t* data, size_t len);

/** @return 0 成功，-1 失败；err_out 可选 */
int serial_parse_sensor_report_detail(const uint8_t* data, size_t len,
                                      serial_sensor_report_t* out,
                                      serial_sensor_err_t* err_out);

/** @return 0 成功，-1 非传感器帧或校验失败 */
int serial_parse_sensor_report(const uint8_t* data, size_t len,
                               serial_sensor_report_t* out);

const char* serial_sensor_err_string(serial_sensor_err_t err);

/** @return 0 校验通过，-1 长度不足，-2 校验失败（可选输出 calc/rx） */
int serial_frame_checksum_check(const uint8_t* data, size_t len,
                                uint8_t* calc_out, uint8_t* rx_out);

#ifdef __cplusplus
}
#endif

#endif
