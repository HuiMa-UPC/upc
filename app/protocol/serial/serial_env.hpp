#ifndef SERIAL_ENV_HPP
#define SERIAL_ENV_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/** 1ST 布局：0=旧固件 Len13/21；1=V4+ Len15 重排 */
#define SERIAL_SENSOR_LAYOUT_LEGACY  0u
#define SERIAL_SENSOR_LAYOUT_V4      1u

typedef struct {
    float oxygen_percent;
    int16_t pressure1_x10_kpa;   /**< 舱内1气压（旧）或未用（V4）；单位 0.1kPa */
    float temperature_c;         /**< 舱温（旧）/ 无效时由 SOC AHT20 填 */
    float humidity_percent;
    float pm25_ug_m3;            /**< 旧 1ST 单路 PM；V4 由 SOC UART 填 */
    float pm1_ug_m3;
    float pm10_ug_m3;
    int16_t filter_pressure_x10_kpa; /**< 滤芯气压 SC0（新旧均有） */
    int16_t pressure2_x10_kpa;       /**< 舱内2气压（旧 Len21） */
    uint8_t powder_present;  /**< 粉充足：旧=bit0/1==11；V4=左右匣均非缺粉 */
    uint8_t powder_bits;     /**< 旧：value[10] 低 2 位；V4：bit0=左匣有粉 bit1=右匣有粉（OUTL/OUTR 取反） */
    uint8_t outer_door_open; /**< 1=门开：旧 bit7；V4=!(舱门关到位 bit2) */
    float substrate_temp_c;
    uint16_t hall_bitmap;
    uint8_t inner_door_open;     /**< 1=全开 0=非全开：bit1 UP_SWPD/PD2；非关到位 */

    /* ---- V4 1ST 扩展字段（layout==V4 时有效） ---- */
    uint8_t layout;              /**< SERIAL_SENSOR_LAYOUT_* */
    float env_ntc_c;             /**< 环境 NTC 0.1℃ */
    float filter_ntc_c;          /**< 滤芯 NTC_LX */
    uint8_t local_switch_bits;   /**< 本地接近开关：bit1 内门全开、bit2 舱门关到位等 */
    uint16_t adapter_input_bits; /**< 转接板：粉瓶微动 + 红外镜像 */
    uint8_t powder_ir_bits;      /**< bit0..7 = A1/A2/A3/B1/B2/B3/OUTL/OUTR；1=无粉 */
    uint8_t validity_bits;       /**< value[14] 有效位图 */
    uint8_t powder_left_low;     /**< 1=左匣缺粉（OUTL 无粉） */
    uint8_t powder_right_low;    /**< 1=右匣缺粉（OUTR 无粉） */
} serial_sensor_report_t;

/** TLV 0x06 EXT：电气量 + 风机（仅 V4 布局有意义；Len 与旧版相同须靠版本切换） */
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
 * @brief 解析 TLV 0x02 1ST。
 * Tlv-Len==15 固定按 V4 布局；Len 13/21 走旧布局（二者字段不兼容）。
 */
int serial_parse_sensor_report_detail(const uint8_t* data, size_t len,
                                      serial_sensor_report_t* out,
                                      serial_sensor_err_t* err_out);

int serial_parse_sensor_report(const uint8_t* data, size_t len,
                               serial_sensor_report_t* out);

/** 解析 TLV 0x06 EXT（须调用方确认 MCU≥V4，因 Len 与旧 EXT 相同） */
int serial_parse_sensor_ext_detail(const uint8_t* data, size_t len,
                                   serial_sensor_ext_t* out,
                                   serial_sensor_err_t* err_out);

const char* serial_sensor_err_string(serial_sensor_err_t err);

#ifdef __cplusplus
}
#endif

#endif
