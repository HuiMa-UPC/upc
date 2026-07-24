#include "serial_ota.hpp"
#include "serial_frame.hpp"

#include <cstring>

namespace {

int build_aa_tlv_frame(uint8_t port,
                       uint8_t cmd,
                       uint8_t seq,
                       uint8_t tlv_type,
                       uint8_t tlv_len,
                       const uint8_t* value,
                       uint8_t* out,
                       size_t out_size)
{
    const size_t frame_len = 10u + static_cast<size_t>(tlv_len);
    if(!out || out_size < frame_len) {
        return -1;
    }

    out[0] = SERIAL_SOF0;
    out[1] = SERIAL_SOF1;
    out[2] = SERIAL_FT_SOC_TO_MCU;
    out[3] = port;
    out[4] = cmd;
    out[5] = seq;
    out[6] = static_cast<uint8_t>(2u + tlv_len);
    out[7] = tlv_type;
    out[8] = tlv_len;
    if(tlv_len > 0 && value) {
        std::memcpy(&out[9], value, tlv_len);
    }
    out[9u + tlv_len] = serial_checksum(out, 9u + tlv_len);
    return static_cast<int>(frame_len);
}

} // namespace

int serial_ota_build_request(uint8_t seq, uint8_t* out, size_t out_size)
{
    return build_aa_tlv_frame(
        SERIAL_OTA_PORT_GENERAL,
        SERIAL_OTA_CMD_REQUEST,
        seq,
        SERIAL_OTA_TLV_REQ,
        0,
        nullptr,
        out,
        out_size);
}

int serial_ota_build_data(uint8_t seq,
                          const uint8_t* chunk,
                          uint8_t chunk_len,
                          uint8_t* out,
                          size_t out_size)
{
    if(!chunk || chunk_len == 0 || chunk_len > SERIAL_OTA_CHUNK_SIZE) {
        return -1;
    }
    return build_aa_tlv_frame(
        SERIAL_OTA_PORT_TASK,
        SERIAL_OTA_CMD_PROCESSING,
        seq,
        SERIAL_OTA_TLV_PACKS_SEQ,
        chunk_len,
        chunk,
        out,
        out_size);
}

int serial_ota_build_finished(uint8_t seq, uint8_t* out, size_t out_size)
{
    return build_aa_tlv_frame(
        SERIAL_OTA_PORT_TASK,
        SERIAL_OTA_CMD_PROCESSING,
        seq,
        SERIAL_OTA_TLV_FINISHED,
        0,
        nullptr,
        out,
        out_size);
}

int serial_ota_parse_reply(const uint8_t* frame,
                           size_t len,
                           serial_ota_reply_t* out)
{
    if(!frame || !out || len < 10) {
        return -1;
    }
    if(frame[0] != SERIAL_SOF0 || frame[1] != SERIAL_SOF1 ||
       frame[2] != SERIAL_FT_MCU_TO_SOC) {
        return -2;
    }
    const uint8_t payload_len = frame[6];
    const size_t frame_len = static_cast<size_t>(8u + payload_len);
    if(len < frame_len || payload_len < 2) {
        return -3;
    }
    if(serial_frame_checksum_check(frame, len, nullptr, nullptr) != 0) {
        return -4;
    }

    out->port = frame[3];
    out->cmd = frame[4];
    out->seq = frame[5];
    out->tlv_type = frame[7];
    out->tlv_len = frame[8];
    out->value0 = (out->tlv_len > 0 && payload_len >= 3) ? frame[9] : 0;
    return 0;
}
