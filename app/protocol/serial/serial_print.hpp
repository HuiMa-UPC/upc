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
/* TLV_DUST_CLEAN_FINISHED 的 result 取值 */
#define SERIAL_CLEAN_RESULT_DONE     0x00  /**< 吹粉动作结束（三轮升降完、风机已关）；之后 Z 仍会定位到 -10cm 并筛粉反转，不代表设备已空闲 */
#define SERIAL_CLEAN_RESULT_FAILED   0x01  /**< 启动被拒 / Z 运动失败 / Z 超时 */
#define SERIAL_CLEAN_RESULT_ABORTED  0x02  /**< 被 TLV_DUST_CLEAN_STOP 中止 */

/**
 * 清粉结果上报（MCU→SOC，每次清粉会话一次）：
 * 5A A5 BB A2 42 01 03 03 01 <result> Sum，Seq 固定 0x01；result 见 SERIAL_CLEAN_RESULT_*。
 */
int serial_parse_clean_test_result(const uint8_t* data, size_t len, uint8_t* result_out);
/** 自动转运流程结果：BB A2/0x41 + TLV 0x04，Value 1B（0=完成 1=拒绝/失败 2=已中止） */
int serial_parse_transport_result(const uint8_t* data, size_t len, uint8_t* result_out);

#ifdef __cplusplus
}
#endif

#endif
