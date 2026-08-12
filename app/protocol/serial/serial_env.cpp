#include "serial_env.hpp"
#include "serial_frame.hpp"

static int16_t serial_read_le16(const uint8_t* p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
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
    /* 兼容旧版 value=13 与新版 value=21；最短整帧 = 8 + 15 = 23 */
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
    if(tlv_type != 0x02 || tlv_len < 13 || payload_len < static_cast<uint8_t>(tlv_len + 2)) {
        return fail(SERIAL_SENSOR_ERR_TLV);
    }
    if(len < (size_t)(8 + payload_len)) {
        return fail(SERIAL_SENSOR_ERR_INCOMPLETE);
    }

    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return fail(SERIAL_SENSOR_ERR_CHECKSUM);
    }

    const uint8_t* v = &data[9];
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
    out->hall_bitmap        = 0;   /* 当前固件不上报 Hall（value 仅 13 字节） */

    if(tlv_len >= 21) {
        out->pm1_ug_m3             = (float)serial_read_le16(v + 13);
        out->pm10_ug_m3            = (float)serial_read_le16(v + 15);
        out->filter_pressure_x10_kpa = serial_read_le16(v + 17);
        out->pressure2_x10_kpa     = serial_read_le16(v + 19);
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
