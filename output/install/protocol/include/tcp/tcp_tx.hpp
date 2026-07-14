#ifndef TCP_TX_HPP
#define TCP_TX_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/** SOC→MCU：BE EF + pkg_num + N×11B 子包 */
int tcp_build_motion_frame(const uint8_t* subpkts11, int n,
                           uint8_t* out, size_t out_size);

int tcp_build_heartbeat_frame(uint8_t* out, size_t out_size);

int tcp_build_reset_frame(uint8_t* out, size_t out_size);

/** OTA FRAME 子包 Mode（CMD=0x03 时 byte[1]） */
#define TCP_OTA_MODE_BEGIN  0x01u  /**< OTA 前查询 / 进入接收 */
#define TCP_OTA_MODE_END    0x02u  /**< 全部 DATA 发完后的通知 */

/**
 * SOC→MCU：FRAME CMD=0x03 OTA 控制（11B 子包，pkg_num=1）
 * 子包：CMD(0x03) + Mode + 9B 保留
 */
int tcp_build_ota_ctrl_frame(uint8_t mode, uint8_t* out, size_t out_size);

/**
 * SOC→MCU：DATA 0x13 OTA 数据（pkg_num=1）
 * 子包：CMD(0x13) + LEN(u16 LE) + SEQ(u16 LE) + DATA[len]
 */
int tcp_build_ota_data_frame(uint16_t seq, const uint8_t* data, uint16_t len,
                             uint8_t* out, size_t out_size);

/* 兼容旧名 */
#define build_motion_frame   tcp_build_motion_frame
#define build_heartbeat_frame tcp_build_heartbeat_frame
#define build_reset_frame    tcp_build_reset_frame

#ifdef __cplusplus
}
#endif

#endif
