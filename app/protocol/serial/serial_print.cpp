#include "serial_print.hpp"
#include "serial_frame.hpp"

int serial_build_print_request(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(0xA2, 0x40, 0x01, 0, nullptr, out, out_size);
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
