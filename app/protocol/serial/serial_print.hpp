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
int serial_build_door_open_request(uint8_t* out, size_t out_size);
/** 自动转运流程请求：A2/0x41 + TLV_REQ_TRANSPORT(0x02)，无 Value */
int serial_build_transport_request(uint8_t* out, size_t out_size);

serial_event_t serial_parse_frame(const uint8_t* data, size_t len);
int serial_parse_door_open_result(const uint8_t* data, size_t len, uint8_t* door_state_out);
int serial_parse_clean_test_result(const uint8_t* data, size_t len, uint8_t* result_out);
/** 自动转运流程结果：BB A2/0x41 + TLV 0x04，Value 1B（0=完成 1=拒绝/失败 2=已中止） */
int serial_parse_transport_result(const uint8_t* data, size_t len, uint8_t* result_out);

#ifdef __cplusplus
}
#endif

#endif
