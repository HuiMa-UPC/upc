#ifndef TCP_FRAME_HPP
#define TCP_FRAME_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SOF0 0xBE
#define TCP_SOF1 0xEF

/** MCU→SOC RX：byte[2]=Length，总帧长 = 3 + Length（见 doc/SOC-_MCU TCP报文.xlsx） */
size_t tcp_guess_rx_frame_len(const uint8_t* buf, size_t avail);

#ifdef __cplusplus
}
#endif

#endif
