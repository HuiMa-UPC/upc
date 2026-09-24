#include "serial_env.hpp"
#include "serial_frame.hpp"

static int16_t serial_read_le16(const uint8_t* p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint16_t serial_read_le16u(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

const char* serial_sensor_err_string(serial_sensor_err_t err)
{
    switch(err) {
        case SERIAL_SENSOR_ERR_NONE:        return "OK";
        case SERIAL_SENSOR_ERR_PARAM:       return "PARAM";
        case SERIAL_SENSOR_ERR_TOO_SHORT:   return "TOO_SHORT";
        case SERIAL_SENSOR_ERR_SOF:         return "SOF";
        case SERIAL_SENSOR_ERR_PORT_CMD:    return "PORT_CMD";
        case SERIAL_SENSOR_ERR_TLV:         return "TLV";
        case SERIAL_SENSOR_ERR_INCOMPLETE:  return "INCOMPLETE";
        case SERIAL_SENSOR_ERR_CHECKSUM:    return "CHECKSUM";
        default:                            return "UNKNOWN";
    }
}

int serial_frame_is_sensor_report(const uint8_t* data, size_t len)
{
    if(!data || len < 5) {
        return 0;
    }
    return (data[0] == SERIAL_SOF0 && data[1] == SERIAL_SOF1 &&
            data[2] == SERIAL_FT_MCU_TO_SOC &&
            data[3] == 0xA5 && data[4] == 0x9B) ? 1 : 0;
}

static void sensor_report_clear_v4_fields(serial_sensor_report_t* out)
{
    out->layout = SERIAL_SENSOR_LAYOUT_LEGACY;
    out->inner_door_open = 0;
    out->env_ntc_c = 0.f;
    out->filter_ntc_c = 0.f;
    out->local_switch_bits = 0;
    out->adapter_input_bits = 0;
    out->powder_ir_bits = 0;
    out->validity_bits = 0;
    out->powder_left_low = 0;
    out->powder_right_low = 0;
}

/** V4 1ST：Tlv-Len=15，字段偏移与旧固件完全不同。 */
static int parse_1st_v4(const uint8_t* v, serial_sensor_report_t* out)
{
    const uint8_t valid = v[14];
    out->layout = SERIAL_SENSOR_LAYOUT_V4;
    out->validity_bits = valid;

    out->oxygen_percent = (valid & 0x01u) ? ((float)serial_read_le16(v + 0) / 10.0f) : -1.f;
    out->filter_pressure_x10_kpa = (valid & 0x02u) ? serial_read_le16(v + 2) : 0;
    out->substrate_temp_c = (valid & 0x04u) ? ((float)serial_read_le16(v + 4) / 10.0f) : -1.f;
    out->env_ntc_c = (valid & 0x08u) ? ((float)serial_read_le16(v + 6) / 10.0f) : -1.f;
    out->filter_ntc_c = (valid & 0x10u) ? ((float)serial_read_le16(v + 8) / 10.0f) : -1.f;

    out->local_switch_bits = (valid & 0x20u) ? v[10] : 0u;
    out->adapter_input_bits = (valid & 0x40u) ? serial_read_le16u(v + 11) : 0u;
    out->powder_ir_bits = (valid & 0x80u) ? v[13] : 0u;

    /* 舱门：bit2=1 关到位 → outer_door_open=0；内门 bit1=1 全开 / 0=非全开（关到位 bit0 未装） */
    if(valid & 0x20u) {
        out->outer_door_open = (uint8_t)((out->local_switch_bits & 0x04u) ? 0u : 1u);
        out->inner_door_open = (uint8_t)((out->local_switch_bits & 0x02u) ? 1u : 0u);
    } else {
        out->outer_door_open = 0;
        out->inner_door_open = 0;
    }

    /* 红外：1=受光无粉；OUTL=bit6 左缺粉，OUTR=bit7 右缺粉 */
    if(valid & 0x80u) {
        out->powder_left_low  = (uint8_t)((out->powder_ir_bits & 0x40u) ? 1u : 0u);
        out->powder_right_low = (uint8_t)((out->powder_ir_bits & 0x80u) ? 1u : 0u);
        /* 兼容旧 2bit：有粉=1，与 A1/A2 原始位无关 */
        out->powder_bits = (uint8_t)(
            ((out->powder_left_low == 0) ? 0x01u : 0u) |
            ((out->powder_right_low == 0) ? 0x02u : 0u));
        out->powder_present = (out->powder_left_low || out->powder_right_low) ? 0u : 1u;
    } else {
        out->powder_left_low = 0;
        out->powder_right_low = 0;
        out->powder_bits = 0;
        out->powder_present = 0;
    }

    /* V4 不再从 MCU 取舱温/湿度/PM/舱压；占位无效 */
    out->temperature_c = -1.f;
    out->humidity_percent = -1.f;
    out->pm25_ug_m3 = -1.f;
    out->pm1_ug_m3 = -1.f;
    out->pm10_ug_m3 = -1.f;
    out->pressure1_x10_kpa = 0;
    out->pressure2_x10_kpa = 0;
    out->hall_bitmap = out->adapter_input_bits;
    return 0;
}

static int parse_1st_legacy(const uint8_t* v, uint8_t tlv_len, serial_sensor_report_t* out)
{
    sensor_report_clear_v4_fields(out);
    out->layout = SERIAL_SENSOR_LAYOUT_LEGACY;

    const int16_t o2_raw   = serial_read_le16(v + 0);
    const int16_t press1   = serial_read_le16(v + 2);
    const int16_t temp_raw = serial_read_le16(v + 4);
    const int16_t hum_raw  = serial_read_le16(v + 6);
    const int16_t pm25_raw = serial_read_le16(v + 8);

    out->oxygen_percent     = (float)o2_raw / 10.0f;
    out->pressure1_x10_kpa  = press1;
    out->temperature_c      = (float)temp_raw / 10.0f;
    out->humidity_percent   = (float)hum_raw / 10.0f;
    out->pm25_ug_m3         = (float)pm25_raw;
    out->pm1_ug_m3          = 0.0f;
    out->pm10_ug_m3         = 0.0f;
    out->filter_pressure_x10_kpa = 0;
    out->pressure2_x10_kpa  = 0;
    out->powder_bits        = static_cast<uint8_t>(v[10] & 0x03u);
    out->powder_present     = (out->powder_bits == 0x03u) ? 1u : 0u;
    out->outer_door_open    = (uint8_t)((v[10] & 0x80u) ? 1u : 0u);
    out->substrate_temp_c   = (float)serial_read_le16(v + 11) / 10.0f;
    out->hall_bitmap        = 0;

    if(tlv_len >= 21) {
        out->pm1_ug_m3             = (float)serial_read_le16(v + 13);
        out->pm10_ug_m3            = (float)serial_read_le16(v + 15);
        out->filter_pressure_x10_kpa = serial_read_le16(v + 17);
        out->pressure2_x10_kpa     = serial_read_le16(v + 19);
    }
    return 0;
}

int serial_parse_sensor_report_detail(const uint8_t* data, size_t len,
                                      serial_sensor_report_t* out,
                                      serial_sensor_err_t* err_out)
{
    auto fail = [err_out](serial_sensor_err_t e) -> int {
        if(err_out) {
            *err_out = e;
        }
        return -1;
    };

    if(!data || !out) {
        return fail(SERIAL_SENSOR_ERR_PARAM);
    }
    /* 最短：旧 Len13 → 整帧 8+15=23；V4 Len15 → 8+17=25 */
    if(len < 23) {
        return fail(SERIAL_SENSOR_ERR_TOO_SHORT);
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return fail(SERIAL_SENSOR_ERR_SOF);
    }
    if(data[3] != 0xA5 || data[4] != 0x9B) {
        return fail(SERIAL_SENSOR_ERR_PORT_CMD);
    }

    const uint8_t payload_len = data[6];
    const uint8_t tlv_type = data[7];
    const uint8_t tlv_len = data[8];
    if(tlv_type != 0x02) {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }
    if(payload_len < static_cast<uint8_t>(tlv_len + 2)) {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }
    if(len < (size_t)(8 + payload_len)) {
        return fail(SERIAL_SENSOR_ERR_INCOMPLETE);
    }

    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return fail(SERIAL_SENSOR_ERR_CHECKSUM);
    }

    const uint8_t* v = &data[9];
    if(tlv_len == 15) {
        /* V4 重排：与旧 Len21 不可混用，凭长度区分即可 */
        if(payload_len < 17) {
            return fail(SERIAL_SENSOR_ERR_TLV);
        }
        parse_1st_v4(v, out);
    } else if(tlv_len >= 13) {
        parse_1st_legacy(v, tlv_len, out);
    } else {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }

    if(err_out) {
        *err_out = SERIAL_SENSOR_ERR_NONE;
    }
    return 0;
}

int serial_parse_sensor_report(const uint8_t* data, size_t len,
                               serial_sensor_report_t* out)
{
    return serial_parse_sensor_report_detail(data, len, out, nullptr);
}

int serial_parse_sensor_ext_detail(const uint8_t* data, size_t len,
                                   serial_sensor_ext_t* out,
                                   serial_sensor_err_t* err_out)
{
    auto fail = [err_out](serial_sensor_err_t e) -> int {
        if(err_out) {
            *err_out = e;
        }
        return -1;
    };

    if(!data || !out) {
        return fail(SERIAL_SENSOR_ERR_PARAM);
    }
    /* 8 + (2+26) = 36 */
    if(len < 36) {
        return fail(SERIAL_SENSOR_ERR_TOO_SHORT);
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return fail(SERIAL_SENSOR_ERR_SOF);
    }
    if(data[3] != 0xA5 || data[4] != 0x9B) {
        return fail(SERIAL_SENSOR_ERR_PORT_CMD);
    }

    const uint8_t payload_len = data[6];
    const uint8_t tlv_type = data[7];
    const uint8_t tlv_len = data[8];
    if(tlv_type != 0x06 || tlv_len < 26 || payload_len < static_cast<uint8_t>(tlv_len + 2)) {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }
    if(len < (size_t)(8 + payload_len)) {
        return fail(SERIAL_SENSOR_ERR_INCOMPLETE);
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return fail(SERIAL_SENSOR_ERR_CHECKSUM);
    }

    const uint8_t* v = &data[9];
    out->sieve_current_ma    = serial_read_le16u(v + 0);
    out->door_coil_ma        = serial_read_le16u(v + 2);
    out->inner_door_step_ma  = serial_read_le16u(v + 4);
    out->ptc_heat_ma         = serial_read_le16u(v + 6);
    out->drop_powder_ma      = serial_read_le16u(v + 8);
    out->soc_supply_ma       = serial_read_le16u(v + 10);
    out->cool_fan_ma         = serial_read_le16u(v + 12);
    out->soc_24v_mv          = serial_read_le16u(v + 14);
    out->elec_valid_bits     = v[16];
    out->fan_rpm             = serial_read_le16u(v + 17);
    out->fan_bus_v_x10       = serial_read_le16u(v + 19);
    out->fan_bus_i_ma        = serial_read_le16u(v + 21);
    out->fan_fault           = v[23];
    out->fan_state           = v[24];
    out->fan_status_bits     = v[25];

    if(err_out) {
        *err_out = SERIAL_SENSOR_ERR_NONE;
    }
    return 0;
}
