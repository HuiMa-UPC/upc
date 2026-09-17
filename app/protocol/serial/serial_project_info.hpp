#ifndef SERIAL_PROJECT_INFO_HPP
#define SERIAL_PROJECT_INFO_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file serial_project_info.hpp
 * @brief 工程版本查询/上报（PORT_GENERAL_TASK 0xA2 + CMD_PROJECT_INFO 0x10）
 *
 * 查询（SOC→MCU）：5A A5 AA A2 10 01 02 01 00 5F
 *   TLV_GET_VERSION(0x01)，无 Value。
 * 上报（MCU→SOC）：5A A5 BB A2 10 01 09 02 07 <7B Value> <cks>
 *   TLV_REPORT_VERSION(0x02)，Value 7 字节：
 *     [0..2] major/minor/patch
 *     [3] laser_type  [4] z_motor_type
 *     [5] 运行槽（1=A, 2=B）  [6] OTA 状态（0 空闲 / 3 TESTING / 4 CONFIRMED / 5 FAILED）
 *
 * 末字节为校验和（`serial_checksum`），非帧尾；general_task_app 在任意机器状态下应答。
 */

#define SERIAL_PINFO_PORT_GENERAL   0xA2  /**< PORT_GENERAL_TASK */
#define SERIAL_PINFO_CMD            0x10  /**< CMD_PROJECT_INFO */
#define SERIAL_PINFO_TLV_GET        0x01  /**< TLV_GET_VERSION */
#define SERIAL_PINFO_TLV_REPORT     0x02  /**< TLV_REPORT_VERSION */
#define SERIAL_PINFO_REPORT_TLV_LEN 0x07  /**< 上报 Value 长度 */

typedef struct {
    uint8_t major;        /**< 大版本 0~255（MCU app_config.h FW_VERSION_MAJOR） */
    uint8_t minor;        /**< 子版本 0~255（FW_VERSION_MINOR） */
    uint8_t patch;        /**< 修订号 0~255（FW_VERSION_PATCH） */
    uint8_t laser_type;   /**< 激光器类别：JFSC_LASER=1，AL_LASER=2 */
    uint8_t z_motor_type; /**< Z 轴电机：闭环 BRIDGE=0，开环 OPEN_CONTROL=1 */
    uint8_t run_slot;     /**< 运行槽：1=A, 2=B（TLV value[5]） */
    uint8_t ota_state;    /**< OTA 状态（TLV value[6]）：0 空闲, 3 TESTING, 4 CONFIRMED, 5 FAILED */
} serial_project_info_t;

/**
 * @brief 组装工程版本查询帧（TLV_GET_VERSION，无 Value）
 * @return >0 帧长；<=0 组帧失败
 */
int serial_build_project_info_request(uint8_t* out, size_t out_size);

/**
 * @brief 解析工程版本上报帧（TLV_REPORT_VERSION）
 * @return 0 成功；-1 参数/长度不足；-2 SOF 或 Port/Cmd 不匹配；-3 TLV 不匹配；-4 校验失败
 */
int serial_parse_project_info_report(const uint8_t* data, size_t len,
                                     serial_project_info_t* out);

#ifdef __cplusplus
}
#endif

#endif
