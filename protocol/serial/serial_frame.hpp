#ifndef SERIAL_FRAME_HPP
#define SERIAL_FRAME_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_SOF0 0x5A
#define SERIAL_SOF1 0xA5
#define SERIAL_FT_SOC_TO_MCU 0xAA
#define SERIAL_FT_MCU_TO_SOC 0xBB

uint8_t serial_checksum(const uint8_t* data, size_t len);

/** MCU→SOC RX：5A A5 BB，总帧长 = 8 + Plen */
size_t serial_guess_rx_frame_len(const uint8_t* buf, size_t avail);

/** SOC→MCU 组帧（FrameType=AA） */
int serial_build_aa_frame(uint8_t port, uint8_t cmd,
                          uint8_t type, uint8_t tlen,
                          const uint8_t* value,
                          uint8_t* out, size_t out_size);

/** @return 0 校验通过，-1 长度不足，-2 校验失败 */
int serial_frame_checksum_check(const uint8_t* data, size_t len,
                                uint8_t* calc_out, uint8_t* rx_out);

#define serial_guess_frame_len serial_guess_rx_frame_len

#ifdef __cplusplus
}
#endif

#endif
