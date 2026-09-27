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

/** 1ST：Tlv-Len=15。舱温/湿度/PM/舱压不在本帧，由 SOC 采集。 */
static void parse_1st(const uint8_t* v, serial_sensor_report_t* out)
{
    const uint8_t valid = v[14];
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
        /* bit0/bit1：左/右匣仍有粉，与 A1/A2 原始位无关 */
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
    /* Tlv-Len=15 → payload≥17，整帧 ≥ 8+17=25 */
    if(len < 25) {
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

    if(tlv_len != 15 || payload_len < 17) {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }
    parse_1st(&data[9], out);

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

int serial_parse_sensor_fanboard_detail(const uint8_t* data, size_t len,
                                        serial_sensor_fanboard_t* out,
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
    /* 最短：Type+Len+value[0..8] → payload≥11，整帧 ≥ 19 */
    if(len < 19) {
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
    if(tlv_type != 0x08 || tlv_len < 9 ||
       payload_len < static_cast<uint8_t>(tlv_len + 2)) {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }
    if(len < (size_t)(8 + payload_len)) {
        return fail(SERIAL_SENSOR_ERR_INCOMPLETE);
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return fail(SERIAL_SENSOR_ERR_CHECKSUM);
    }

    const uint8_t* v = &data[9];
    *out = {};
    out->fan_status_bits = v[0];
    out->fan_fault       = v[1];
    out->fan_state       = v[2];
    out->fan_rpm         = serial_read_le16u(v + 3);
    out->fan_bus_v_x10   = serial_read_le16u(v + 5);
    out->fan_bus_i_ma    = serial_read_le16u(v + 7);
    if(tlv_len >= 13) {
        out->ldo_temp_x10_c = serial_read_le16(v + 9);
        out->mcu_temp_x10_c = serial_read_le16(v + 11);
    }

    if(err_out) {
        *err_out = SERIAL_SENSOR_ERR_NONE;
    }
    return 0;
}

int serial_parse_sensor_adc_frame(const uint8_t* data, size_t len)
{
    if(!data || len < 12) {
        return -1;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -1;
    }
    if(data[3] != 0xA5 || data[4] != 0x9B) {
        return -1;
    }
    const uint8_t payload_len = data[6];
    const uint8_t tlv_type = data[7];
    const uint8_t tlv_len = data[8];
    if(tlv_type != 0x07 || tlv_len < 2 ||
       payload_len < static_cast<uint8_t>(tlv_len + 2)) {
        return -1;
    }
    if(len < (size_t)(8 + payload_len)) {
        return -1;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -1;
    }
    return 0;
}
