#ifndef SERIAL_ENV_HPP
#define SERIAL_ENV_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float oxygen_percent;        /**< 滤芯氧 %；无效 -1。舱内氧由 SOC IIO 填 */
    int16_t filter_pressure_x10_kpa; /**< 滤芯气压 SC0，单位 0.1kPa */
    float substrate_temp_c;
    uint8_t powder_present;      /**< 1=左右匣均非缺粉 */
    uint8_t powder_bits;         /**< bit0=左匣有粉 bit1=右匣有粉（OUTL/OUTR 取反） */
    uint8_t outer_door_open;     /**< 1=门开：!(舱门关到位 bit2) */
    float env_ntc_c;             /**< 环境 NTC 0.1℃ */
    float filter_ntc_c;          /**< 滤芯 NTC_LX */
    uint8_t local_switch_bits;   /**< 本地接近开关：bit2 舱门关到位等 */
    uint16_t adapter_input_bits; /**< 转接板：粉瓶微动 + 红外镜像 */
    uint8_t powder_ir_bits;      /**< bit0..7 = A1/A2/A3/B1/B2/B3/OUTL/OUTR；1=无粉 */
    uint8_t validity_bits;       /**< value[14] 有效位图 */
    uint8_t powder_left_low;     /**< 1=左匣缺粉（OUTL 无粉） */
    uint8_t powder_right_low;    /**< 1=右匣缺粉（OUTR 无粉） */
} serial_sensor_report_t;

/** TLV 0x06 EXT：电气量 + 风机 */
typedef struct {
    uint16_t sieve_current_ma;
    uint16_t door_coil_ma;
    uint16_t inner_door_step_ma;
    uint16_t ptc_heat_ma;
    uint16_t drop_powder_ma;
    uint16_t soc_supply_ma;
    uint16_t cool_fan_ma;
    uint16_t soc_24v_mv;
    uint8_t  elec_valid_bits;
    uint16_t fan_rpm;
    uint16_t fan_bus_v_x10;   /**< 0.1V */
    uint16_t fan_bus_i_ma;
    uint8_t  fan_fault;
    uint8_t  fan_state;
    uint8_t  fan_status_bits; /**< bit7=链路正常 */
} serial_sensor_ext_t;

typedef enum {
    SERIAL_SENSOR_ERR_NONE = 0,
    SERIAL_SENSOR_ERR_PARAM,
    SERIAL_SENSOR_ERR_TOO_SHORT,
    SERIAL_SENSOR_ERR_SOF,
    SERIAL_SENSOR_ERR_PORT_CMD,
    SERIAL_SENSOR_ERR_TLV,
    SERIAL_SENSOR_ERR_INCOMPLETE,
    SERIAL_SENSOR_ERR_CHECKSUM,
} serial_sensor_err_t;

int serial_frame_is_sensor_report(const uint8_t* data, size_t len);

/**
 * @brief 解析 TLV 0x02 1ST。只接受 Tlv-Len==15。
 */
int serial_parse_sensor_report_detail(const uint8_t* data, size_t len,
                                      serial_sensor_report_t* out,
                                      serial_sensor_err_t* err_out);

int serial_parse_sensor_report(const uint8_t* data, size_t len,
                               serial_sensor_report_t* out);

/** 解析 TLV 0x06 EXT（电气量 + 风机） */
int serial_parse_sensor_ext_detail(const uint8_t* data, size_t len,
                                   serial_sensor_ext_t* out,
                                   serial_sensor_err_t* err_out);

const char* serial_sensor_err_string(serial_sensor_err_t err);

#ifdef __cplusplus
}
#endif

#endif
