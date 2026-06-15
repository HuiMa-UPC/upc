#include "protocol_mcu.hpp"
#include <cstring>

int build_motion_frame(const uint8_t* subpkts11, int n,
                       uint8_t* out, size_t out_size)
{
    if(!subpkts11 || n <= 0 || n > 5957 || !out ||
       out_size < (size_t)(3 + 11 * n)) {
        return -1;
    }

    out[0] = 0xBE;
    out[1] = 0xEF;
    out[2] = (uint8_t)n;
    std::memcpy(&out[3], subpkts11, (size_t)n * 11u);
    return 3 + n * 11;
}

int build_heartbeat_frame(uint8_t* out, size_t out_size)
{
    if(!out || out_size < 14) {
        return -1;
    }
    out[0] = 0xBE;
    out[1] = 0xEF;
    out[2] = 0x01;
    out[3] = 0x0B;
    std::memset(&out[4], 0, 10);
    return 14;
}

int build_reset_frame(uint8_t* out, size_t out_size)
{
    if(!out || out_size < 14) {
        return -1;
    }
    out[0] = 0xBE;
    out[1] = 0xEF;
    out[2] = 0x01;
    out[3] = 0x0A;
    std::memset(&out[4], 0, 10);
    return 14;
}

int parse_tcp_response(const uint8_t* data, size_t len, tcp_response_t* resp)
{
    if(!data || len < 19 || !resp) {
        return -1;
    }
    if(data[0] != 0xBE || data[1] != 0xEF || data[2] != 0x10 || data[3] != 0x01) {
        return -1;
    }

    resp->cmd_rate  = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    resp->data_rate = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    return 0;
}

static uint8_t serial_checksum(const uint8_t* data, size_t len)
{
    uint8_t sum = 0;
    for(size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static int serial_build_frame(uint8_t port, uint8_t cmd,
                              uint8_t type, uint8_t tlen,
                              const uint8_t* value,
                              uint8_t* out, size_t out_size)
{
    const size_t frame_len = 10u + (size_t)tlen;
    if(!out || out_size < frame_len) {
        return -1;
    }

    out[0] = 0x5A;
    out[1] = 0xA5;
    out[2] = 0xAA;
    out[3] = port;
    out[4] = cmd;
    out[5] = 0x01;
    out[6] = (uint8_t)(2 + tlen);
    out[7] = type;
    out[8] = tlen;
    if(tlen > 0 && value) {
        std::memcpy(&out[9], value, tlen);
    }
    out[9 + tlen] = serial_checksum(out, 9 + tlen);
    return (int)frame_len;
}

int serial_build_print_request(uint8_t* out, size_t out_size)
{
    return serial_build_frame(0xA2, 0x40, 0x01, 0, nullptr, out, out_size);
}

int serial_build_print_start(uint16_t layer_height_um, uint8_t* out, size_t out_size)
{
    uint8_t v[2] = { (uint8_t)(layer_height_um & 0xFF),
                     (uint8_t)(layer_height_um >> 8) };
    return serial_build_frame(0xA4, 0x01, 0x03, 2, v, out, out_size);
}

int serial_build_layer_done(uint16_t next_layer_height_um, uint8_t* out, size_t out_size)
{
    uint8_t v[2] = { (uint8_t)(next_layer_height_um & 0xFF),
                     (uint8_t)(next_layer_height_um >> 8) };
    return serial_build_frame(0xA4, 0x01, 0x05, 2, v, out, out_size);
}

int serial_build_print_finished(uint8_t* out, size_t out_size)
{
    return serial_build_frame(0xA4, 0x01, 0x06, 0, nullptr, out, out_size);
}

int serial_build_print_terminate(uint8_t* out, size_t out_size)
{
    return serial_build_frame(0xA2, 0x40, 0x0B, 0, nullptr, out, out_size);
}

static int16_t serial_read_le16(const uint8_t* p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

int serial_frame_is_sensor_report(const uint8_t* data, size_t len)
{
    if(!data || len < 5) {
        return 0;
    }
    return (data[0] == 0x5A && data[1] == 0xA5 && data[2] == 0xBB &&
            data[3] == 0xA5 && data[4] == 0x9B) ? 1 : 0;
}

int serial_frame_checksum_check(const uint8_t* data, size_t len,
                                uint8_t* calc_out, uint8_t* rx_out)
{
    if(!data || len < 8) {
        return -1;
    }
    const uint8_t payload_len = data[6];
    if(len < (size_t)(8 + payload_len)) {
        return -1;
    }
    const uint8_t calc = serial_checksum(data, 9 + payload_len - 2);
    const uint8_t rx   = data[9 + payload_len - 2];
    if(calc_out) {
        *calc_out = calc;
    }
    if(rx_out) {
        *rx_out = rx;
    }
    return calc == rx ? 0 : -2;
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
    if(len < 25) {
        return fail(SERIAL_SENSOR_ERR_TOO_SHORT);
    }
    if(data[0] != 0x5A || data[1] != 0xA5 || data[2] != 0xBB) {
        return fail(SERIAL_SENSOR_ERR_SOF);
    }
    if(data[3] != 0xA5 || data[4] != 0x9B) {
        return fail(SERIAL_SENSOR_ERR_PORT_CMD);
    }

    const uint8_t payload_len = data[6];
    if(payload_len != 0x11 || data[7] != 0x02 || data[8] != 0x0F) {
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
    out->hall_bitmap        = (uint16_t)serial_read_le16(v + 13);

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

serial_event_t serial_parse_frame(const uint8_t* data, size_t len)
{
    if(!data || len < 10) {
        return SERIAL_EVT_NONE;
    }
    if(data[0] != 0x5A || data[1] != 0xA5 || data[2] != 0xBB) {
        return SERIAL_EVT_NONE;
    }

    const uint8_t payload_len = data[6];
    if(len < (size_t)(8 + payload_len)) {
        return SERIAL_EVT_NONE;
    }

    const uint8_t calc = serial_checksum(data, 9 + payload_len - 2);
    if(calc != data[9 + payload_len - 2]) {
        return SERIAL_EVT_NONE;
    }

    const uint8_t port = data[3];
    const uint8_t cmd  = data[4];
    const uint8_t type = data[7];

    if(port == 0xA5 && cmd == 0x9B) {
        return SERIAL_EVT_NONE;
    }

    if(port == 0xA4 && cmd == 0x01 && type == 0x02) {
        return SERIAL_EVT_PRINT_CONDITION_OK;
    }
    if(port == 0xA4 && cmd == 0x01 && type == 0x04) {
        return SERIAL_EVT_PRINT_READY;
    }
    return SERIAL_EVT_NONE;
}
