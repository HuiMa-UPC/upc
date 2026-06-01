#ifndef PROTOCOL_MCU_HPP
#define PROTOCOL_MCU_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_PARAM_A_MAX_SCALE  100000000.0f

int build_motion_frame(const uint8_t* subpkts11, int n,
                       uint8_t* out, size_t out_size);

int build_heartbeat_frame(uint8_t* out, size_t out_size);

int build_reset_frame(uint8_t* out, size_t out_size);

int build_readparam_frame(uint8_t* out, size_t out_size);

typedef struct {
    uint16_t cmd_rate;
    uint16_t data_rate;
    uint8_t  flash_flag;
    uint16_t current_x;
    uint16_t current_y;
    uint16_t current_z;
    uint16_t laser_state;
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

serial_event_t serial_parse_frame(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
