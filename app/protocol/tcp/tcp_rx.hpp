#ifndef TCP_RX_HPP
#define TCP_RX_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/** MCU→SOC 常态应答 byte[8] 通用标志（单次脉冲） */
enum tcp_status_event_flag : uint8_t {
    TCP_EVT_NONE            = 0x00,
    TCP_EVT_FLASH_SAVED     = 0x0D,
    TCP_EVT_OTA_READY       = 0x03,
    TCP_EVT_OTA_CHUNK_DONE  = 0x23,
    TCP_EVT_OTA_ALL_DONE    = 0x13,
    TCP_EVT_OTA_FAILED      = 0xE3,
};

typedef struct {
    uint16_t cmd_rate;
    uint16_t data_rate;
    uint8_t  event_flag;
    uint8_t  ota_progress;
} tcp_response_t;

/** Length=0x10, ID=0x01 运动/心跳/OTA 应答 */
int tcp_parse_status_response(const uint8_t* data, size_t len, tcp_response_t* resp);

#define parse_tcp_response tcp_parse_status_response

#ifdef __cplusplus
}
#endif

#endif
