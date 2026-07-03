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
    /* 当前 MCU 上报为 13 字节 Value（无 Hall）：整帧 = 8 + payload_len(0x0F) = 23 */
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
    if(payload_len != 0x0F || data[7] != 0x02 || data[8] != 0x0D) {
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
    const int16_t press    = serial_read_le16(v + 2);
    const int16_t temp_raw = serial_read_le16(v + 4);
    const int16_t hum_raw  = serial_read_le16(v + 6);
    const int16_t pm_raw   = serial_read_le16(v + 8);

    out->oxygen_percent     = (float)o2_raw / 10.0f;
    out->pressure_mbar      = press;
    out->temperature_c      = (float)temp_raw / 10.0f;
    out->humidity_percent   = (float)hum_raw / 10.0f;
    out->pm25_ug_m3         = (float)pm_raw;
    out->powder_present     = v[10];
    out->substrate_temp_c   = (float)serial_read_le16(v + 11) / 10.0f;
    out->hall_bitmap        = 0;   /* 当前固件不上报 Hall（value 仅 13 字节） */

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
