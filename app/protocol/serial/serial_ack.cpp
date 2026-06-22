#include "serial_ack.hpp"
#include "serial_frame.hpp"

static int serial_frame_has_sof(const uint8_t* data, size_t len)
{
    return data && len >= 2 &&
           data[0] == SERIAL_SOF0 && data[1] == SERIAL_SOF1;
}

int serial_is_mcu_ack(const uint8_t* data, size_t len)
{
    return serial_frame_has_sof(data, len) &&
           len >= SERIAL_ACK_FRAME_LEN &&
           data[2] == SERIAL_FT_MCU_ACK &&
           data[6] == 0x02 && data[8] == 0x00;
}

int serial_is_mcu_data(const uint8_t* data, size_t len)
{
    return serial_frame_has_sof(data, len) &&
           len >= 10 &&
           data[2] == SERIAL_FT_MCU_TO_SOC;
}

int serial_is_soc_data(const uint8_t* data, size_t len)
{
    return serial_frame_has_sof(data, len) &&
           len >= 10 &&
           data[2] == SERIAL_FT_SOC_TO_MCU;
}

int serial_extract_ack_key(const uint8_t* data, size_t len, serial_ack_key_t* out)
{
    if(!data || !out || len < 9) {
        return -1;
    }
    if(!serial_frame_has_sof(data, len)) {
        return -1;
    }
    const uint8_t ft = data[2];
    if(ft != SERIAL_FT_SOC_TO_MCU && ft != SERIAL_FT_MCU_TO_SOC &&
       ft != SERIAL_FT_SOC_ACK && ft != SERIAL_FT_MCU_ACK) {
        return -1;
    }
    out->port = data[3];
    out->cmd  = data[4];
    out->type = data[7];
    return 0;
}

int serial_ack_keys_match(const serial_ack_key_t* a, const serial_ack_key_t* b)
{
    if(!a || !b) {
        return 0;
    }
    return a->port == b->port && a->cmd == b->cmd && a->type == b->type;
}

int serial_build_soc_ack(const uint8_t* bb_frame, size_t bb_len,
                         uint8_t* out, size_t out_size)
{
    if(!bb_frame || bb_len < 9 || !out || out_size < SERIAL_ACK_FRAME_LEN) {
        return -1;
    }
    if(!serial_is_mcu_data(bb_frame, bb_len)) {
        return -1;
    }
    if(serial_frame_checksum_check(bb_frame, bb_len, nullptr, nullptr) != 0) {
        return -1;
    }

    out[0] = SERIAL_SOF0;
    out[1] = SERIAL_SOF1;
    out[2] = SERIAL_FT_SOC_ACK;
    out[3] = bb_frame[3];
    out[4] = bb_frame[4];
    out[5] = bb_frame[5];
    out[6] = 0x02;
    out[7] = bb_frame[7];
    out[8] = 0x00;
    out[9] = serial_checksum(out, 9);
    return (int)SERIAL_ACK_FRAME_LEN;
}
