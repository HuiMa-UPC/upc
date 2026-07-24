#ifndef SERIAL_OTA_HPP
#define SERIAL_OTA_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SERIAL_OTA_PORT_GENERAL = 0xA2,
    SERIAL_OTA_PORT_TASK    = 0xA6,

    SERIAL_OTA_CMD_REQUEST    = 0x30,
    SERIAL_OTA_CMD_PROCESSING = 0x31,

    SERIAL_OTA_TLV_REQ          = 0x01,
    SERIAL_OTA_TLV_REQ_RESULT   = 0x02,
    SERIAL_OTA_TLV_PACKS_SEQ    = 0x01,
    SERIAL_OTA_TLV_JUDGMENT_SEQ = 0x02,
    SERIAL_OTA_TLV_PACKS_ACK    = 0x03,
    SERIAL_OTA_TLV_FINISHED     = 0x04,
    SERIAL_OTA_TLV_FINISHED_ACK = 0x05,

    SERIAL_OTA_CHUNK_SIZE = 128,
};

typedef struct serial_ota_reply {
    uint8_t port;
    uint8_t cmd;
    uint8_t seq;
    uint8_t tlv_type;
    uint8_t tlv_len;
    uint8_t value0;
} serial_ota_reply_t;

int serial_ota_build_request(uint8_t seq, uint8_t* out, size_t out_size);
int serial_ota_build_data(uint8_t seq,
                          const uint8_t* chunk,
                          uint8_t chunk_len,
                          uint8_t* out,
                          size_t out_size);
int serial_ota_build_finished(uint8_t seq, uint8_t* out, size_t out_size);

int serial_ota_parse_reply(const uint8_t* frame,
                           size_t len,
                           serial_ota_reply_t* out);

#ifdef __cplusplus
}
#endif

#endif
