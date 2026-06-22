#ifndef SERIAL_PRINT_HPP
#define SERIAL_PRINT_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERIAL_EVT_NONE = 0,
    SERIAL_EVT_PRINT_PREPARING,
    SERIAL_EVT_PRINT_CONDITION_OK,
    SERIAL_EVT_PRINT_READY,
} serial_event_t;

int serial_build_print_request(uint8_t* out, size_t out_size);
int serial_build_print_start(uint16_t layer_height_um, uint8_t* out, size_t out_size);
int serial_build_layer_done(uint16_t next_layer_height_um, uint8_t* out, size_t out_size);
int serial_build_print_finished(uint8_t* out, size_t out_size);
int serial_build_print_terminate(uint8_t* out, size_t out_size);

serial_event_t serial_parse_frame(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
