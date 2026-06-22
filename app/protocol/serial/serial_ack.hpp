#ifndef SERIAL_ACK_HPP
#define SERIAL_ACK_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_FT_SOC_ACK 0xAB
#define SERIAL_FT_MCU_ACK 0xBA
#define SERIAL_ACK_FRAME_LEN 10u

typedef struct {
    uint8_t port;
    uint8_t cmd;
    uint8_t type;
} serial_ack_key_t;

int serial_is_mcu_ack(const uint8_t* data, size_t len);

int serial_is_mcu_data(const uint8_t* data, size_t len);

int serial_is_soc_data(const uint8_t* data, size_t len);

/** 从 AA/BB/AB/BA 帧提取匹配键（port+cmd+type，不使用 seq） */
int serial_extract_ack_key(const uint8_t* data, size_t len, serial_ack_key_t* out);

int serial_ack_keys_match(const serial_ack_key_t* a, const serial_ack_key_t* b);

/** MCU→SOC BB 数据帧 → SOC→MCU AB 应答 */
int serial_build_soc_ack(const uint8_t* bb_frame, size_t bb_len,
                         uint8_t* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
