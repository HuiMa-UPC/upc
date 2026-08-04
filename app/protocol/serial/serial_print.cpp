#include "serial_print.hpp"
#include "serial_frame.hpp"

int serial_build_print_request(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(0xA2, 0x40, 0x01, 0, nullptr, out, out_size);
}

int serial_build_plate_prepare(uint16_t thickness_um, uint8_t* out, size_t out_size)
{
    uint8_t v[2] = { static_cast<uint8_t>(thickness_um & 0xFFu),
                     static_cast<uint8_t>(thickness_um >> 8u) };
    return serial_build_aa_frame(0xA2, 0x40, 0x12, 2, v, out, out_size);
}

int serial_build_print_start(uint16_t layer_height_um, uint8_t* out, size_t out_size)
{
    uint8_t v[2] = { (uint8_t)(layer_height_um & 0xFF),
                     (uint8_t)(layer_height_um >> 8) };
    return serial_build_aa_frame(0xA4, 0x01, 0x03, 2, v, out, out_size);
}

int serial_build_layer_done(uint16_t next_layer_height_um, uint8_t* out, size_t out_size)
{
    uint8_t v[2] = { (uint8_t)(next_layer_height_um & 0xFF),
                     (uint8_t)(next_layer_height_um >> 8) };
    return serial_build_aa_frame(0xA4, 0x01, 0x05, 2, v, out, out_size);
}

int serial_build_print_finished(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(0xA4, 0x01, 0x06, 0, nullptr, out, out_size);
}

int serial_build_print_terminate(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(0xA2, 0x40, 0x0B, 0, nullptr, out, out_size);
}

int serial_build_door_open_request(uint8_t* out, size_t out_size)
{
    /* UART2 打印流程：PORT_GENERAL_TASK(0xA2) + CMD_REQUEST_PRINTING(0x40)
     * + TLV_REQ_OPEN_DOOR(0x10)。 */
    return serial_build_aa_frame(0xA2, 0x40, 0x10, 0, nullptr, out, out_size);
}

int serial_build_transport_request(uint8_t* out, size_t out_size)
{
    /* 自动转运流程请求：PORT_GENERAL_TASK(0xA2) + CMD_REQUEST_TRANSPORT(0x41)
     * + TLV_REQ_TRANSPORT(0x02)，无 Value。示例帧 5A A5 AA A2 41 01 02 02 00 … */
    return serial_build_aa_frame(0xA2, 0x41, 0x02, 0, nullptr, out, out_size);
}

static int serial_print_verify_rx(const uint8_t* data, size_t len)
{
    if(!data || len < 10) {
        return -1;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -1;
    }

    const uint8_t payload_len = data[6];
    if(len < (size_t)(8 + payload_len)) {
        return -1;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -1;
    }
    return 0;
}

serial_event_t serial_parse_frame(const uint8_t* data, size_t len)
{
    if(serial_print_verify_rx(data, len) != 0) {
        return SERIAL_EVT_NONE;
    }

    const uint8_t port = data[3];
    const uint8_t cmd  = data[4];
    const uint8_t type = data[7];

    if(port == 0xA5 && cmd == 0x9B) {
        return SERIAL_EVT_NONE;
    }

    if(port == 0xA2 && cmd == 0x40 && type == 0x02) {
        return SERIAL_EVT_PRINT_PREPARING;
    }
    if(port == 0xA4 && cmd == 0x01 && type == 0x02) {
        return SERIAL_EVT_PRINT_CONDITION_OK;
    }
    if(port == 0xA4 && cmd == 0x01 && type == 0x04) {
        return SERIAL_EVT_PRINT_READY;
    }
    return SERIAL_EVT_NONE;
}

int serial_parse_plate_prepare_result(const uint8_t* data, size_t len, uint8_t* result_out)
{
    if(serial_print_verify_rx(data, len) != 0) {
        return -1;
    }

    /* UART2 基板准备结果：
     * PORT_GENERAL_TASK(0xA2) + CMD_REQUEST_PRINTING(0x40) + TLV_REQ_PLATE_RESULT(0x13)
     * Value: result 0x00=成功，0x01=失败。 */
    const uint8_t port = data[3];
    const uint8_t cmd  = data[4];
    const uint8_t payload_len = data[6];
    const uint8_t type = data[7];
    const uint8_t tlen = data[8];
    if(port != 0xA2 || cmd != 0x40 || type != 0x13) {
        return -2;
    }
    if(payload_len != 0x03 || tlen != 0x01) {
        return -3;
    }
    if(result_out) {
        *result_out = data[9];
    }
    return 0;
}

int serial_parse_door_open_result(const uint8_t* data, size_t len, uint8_t* door_state_out)
{
    if(serial_print_verify_rx(data, len) != 0) {
        return -1;
    }

    /* UART2 打印流程门控结果：
     * PORT_GENERAL_TASK(0xA2) + CMD_REQUEST_PRINTING(0x40) + TLV_REQ_DOOR_OPEN_RESULT(0x11)
     * Value: door_state 0x00=门已开，0x01=拒绝/失败。 */
    const uint8_t port = data[3];
    const uint8_t cmd  = data[4];
    const uint8_t payload_len = data[6];
    const uint8_t type = data[7];
    const uint8_t tlen = data[8];
    if(port != 0xA2 || cmd != 0x40 || type != 0x11) {
        return -2;
    }
    if(payload_len != 0x03 || tlen != 0x01) {
        return -3;
    }
    if(door_state_out) {
        *door_state_out = data[9];
    }
    return 0;
}

int serial_parse_clean_test_result(const uint8_t* data, size_t len, uint8_t* result_out)
{
    if(serial_print_verify_rx(data, len) != 0) {
        return -1;
    }

    /* 清粉测试完成结果：
     * PORT_PRINTING_TASK(0xA4) + CMD_PRINTING_CTRL(0x01) + TLV(0x22)
     * Value: result_code，0=成功，非0=失败。 */
    const uint8_t port = data[3];
    const uint8_t cmd  = data[4];
    const uint8_t payload_len = data[6];
    const uint8_t type = data[7];
    const uint8_t tlen = data[8];
    if(port != 0xA4 || cmd != 0x01 || type != 0x22) {
        return -2;
    }
    if(payload_len != 0x03 || tlen != 0x01) {
        return -3;
    }
    if(result_out) {
        *result_out = data[9];
    }
    return 0;
}

int serial_parse_transport_result(const uint8_t* data, size_t len, uint8_t* result_out)
{
    if(serial_print_verify_rx(data, len) != 0) {
        return -1;
    }

    /* 自动转运流程结果：
     * PORT_GENERAL_TASK(0xA2) + CMD_REQUEST_TRANSPORT(0x41) + TLV_REQ_TRANSPORT_RESULT(0x04)
     * Value: 0=完成，1=拒绝或失败，2=已中止。 */
    const uint8_t port = data[3];
    const uint8_t cmd  = data[4];
    const uint8_t payload_len = data[6];
    const uint8_t type = data[7];
    const uint8_t tlen = data[8];
    if(port != 0xA2 || cmd != 0x41 || type != 0x04) {
        return -2;
    }
    if(payload_len != 0x03 || tlen != 0x01) {
        return -3;
    }
    if(result_out) {
        *result_out = data[9];
    }
    return 0;
}
