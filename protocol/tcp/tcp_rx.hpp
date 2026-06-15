#ifndef TCP_RX_HPP
#define TCP_RX_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t cmd_rate;
    uint16_t data_rate;
} tcp_response_t;

/** Length=0x10, ID=0x01 运动/心跳应答 */
int tcp_parse_status_response(const uint8_t* data, size_t len, tcp_response_t* resp);

#define parse_tcp_response tcp_parse_status_response

#ifdef __cplusplus
}
#endif

#endif
