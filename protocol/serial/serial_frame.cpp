#include "serial_frame.hpp"
#include "serial_ack.hpp"
#include <cstring>

uint8_t serial_checksum(const uint8_t* data, size_t len)
{
    uint8_t sum = 0;
    for(size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

size_t serial_guess_rx_frame_len(const uint8_t* buf, size_t avail)
{
    if(!buf || avail < 3) {
        return 0;
    }
    if(buf[0] != SERIAL_SOF0 || buf[1] != SERIAL_SOF1) {
        return 1;
    }

    const uint8_t frame_type = buf[2];
    if(frame_type == SERIAL_FT_MCU_ACK) {
        return avail < SERIAL_ACK_FRAME_LEN ? 0 : SERIAL_ACK_FRAME_LEN;
    }
    if(frame_type == SERIAL_FT_MCU_TO_SOC) {
        if(avail < 7) {
            return 0;
        }
        const uint8_t payload_len = buf[6];
        const size_t frame_len = 8u + (size_t)payload_len;
        return avail < frame_len ? 0 : frame_len;
    }
    return 1;
}

int serial_build_aa_frame(uint8_t port, uint8_t cmd,
                          uint8_t type, uint8_t tlen,
                          const uint8_t* value,
                          uint8_t* out, size_t out_size)
{
    const size_t frame_len = 10u + (size_t)tlen;
    if(!out || out_size < frame_len) {
        return -1;
    }

    out[0] = SERIAL_SOF0;
    out[1] = SERIAL_SOF1;
    out[2] = SERIAL_FT_SOC_TO_MCU;
    out[3] = port;
    out[4] = cmd;
    out[5] = 0x01;
    out[6] = (uint8_t)(2 + tlen);
    out[7] = type;
    out[8] = tlen;
    if(tlen > 0 && value) {
        std::memcpy(&out[9], value, tlen);
    }
    out[9 + tlen] = serial_checksum(out, 9 + tlen);
    return (int)frame_len;
}

int serial_frame_checksum_check(const uint8_t* data, size_t len,
                                uint8_t* calc_out, uint8_t* rx_out)
{
    if(!data || len < 8) {
        return -1;
    }
    const uint8_t payload_len = data[6];
    if(len < (size_t)(8 + payload_len)) {
        return -1;
    }
    const uint8_t calc = serial_checksum(data, 9 + payload_len - 2);
    const uint8_t rx   = data[9 + payload_len - 2];
    if(calc_out) {
        *calc_out = calc;
    }
    if(rx_out) {
        *rx_out = rx;
    }
    return calc == rx ? 0 : -2;
}
