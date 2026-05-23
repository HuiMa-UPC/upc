#include "protocol_mcu.hpp"
#include <cstring>

/* ---------- TCP 帧构造 ---------- */

int build_motion_frame(const uint8_t *subpkts11, int n,
                       uint8_t *out, size_t out_size)
{
    if(!subpkts11 || n <= 0 || n > 5957 || !out || out_size < (size_t)(3 + 11 * n)) {
        return -1;
    }

    out[0] = 0xBE;
    out[1] = 0xEF;
    out[2] = (uint8_t)n;
    memcpy(&out[3], subpkts11, (size_t)n * 11u);
    return 3 + n * 11;
}

int build_heartbeat_frame(uint8_t *out, size_t out_size)
{
    if (!out || out_size < 14) return -1;
    out[0] = 0xBE; out[1] = 0xEF; out[2] = 0x01; out[3] = 0x0B;
    memset(&out[4], 0, 10);
    return 14;
}

int build_reset_frame(uint8_t *out, size_t out_size)
{
    if (!out || out_size < 14) return -1;
    out[0] = 0xBE; out[1] = 0xEF; out[2] = 0x01; out[3] = 0x0A;
    memset(&out[4], 0, 10);
    return 14;
}

int build_readparam_frame(uint8_t *out, size_t out_size)
{
    if (!out || out_size < 14) return -1;
    out[0] = 0xBE; out[1] = 0xEF; out[2] = 0x01; out[3] = 0x0E;
    memset(&out[4], 0, 10);
    return 14;
}

int build_writeparam_frame(uint8_t obj,
                           uint16_t p0, uint16_t p1,
                           uint16_t p2, uint16_t p3,
                           uint8_t *out, size_t out_size)
{
    if (!out || out_size < 14) return -1;
    out[0] = 0xBE; out[1] = 0xEF; out[2] = 0x01; out[3] = 0x0D;
    out[4] = obj;
    out[5] = (uint8_t)(p0 & 0xFF); out[6] = (uint8_t)(p0 >> 8);
    out[7] = (uint8_t)(p1 & 0xFF); out[8] = (uint8_t)(p1 >> 8);
    out[9] = (uint8_t)(p2 & 0xFF); out[10] = (uint8_t)(p2 >> 8);
    out[11] = (uint8_t)(p3 & 0xFF); out[12] = (uint8_t)(p3 >> 8);
    out[13] = 0;
    return 14;
}

/* ---------- TCP 应答解析 ---------- */

int parse_tcp_response(const uint8_t *data, size_t len, tcp_response_t *resp)
{
    if (!data || len < 19 || !resp) return -1;
    if (data[0] != 0xBE || data[1] != 0xEF || data[2] != 0x10 || data[3] != 0x01)
        return -1;

    resp->cmd_rate  = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    resp->data_rate = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    resp->flash_flag = data[8];
    resp->current_x  = (uint16_t)data[11] | ((uint16_t)data[12] << 8);
    resp->current_y  = (uint16_t)data[13] | ((uint16_t)data[14] << 8);
    resp->current_z  = (uint16_t)data[15] | ((uint16_t)data[16] << 8);
    resp->laser_state = (uint16_t)data[17] | ((uint16_t)data[18] << 8);
    return 0;
}

int parse_readparam_response(const uint8_t *data, size_t len, param_set_t *out)
{
    if (!data || len < 81 || !out) return -1;
    if (data[0] != 0xBE || data[1] != 0xEF || data[3] != 0x0E)
        return -1;

    const uint8_t *base = &data[4]; /* skip header+ID */
    for (int obj = 0; obj < 7; obj++) {
        const uint8_t *p = base + obj * 11;
        uint16_t p0 = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
        uint16_t p1 = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
        switch (obj) {
            /* 与下位机一致：线上为 a_max/1e8 的 float，UI 直接显示该值 */
            case 0: out->a_max = float_from_u16_pair(p0, p1); break;
            case 1: out->fly_enable = (p0 != 0); break;
            case 2: out->fly_gain = float_from_u16_pair(p0, p1); break;
            case 3: out->acc_enable = (p0 != 0); break;
            case 4: out->acc_delat = float_from_u16_pair(p0, p1); break;
            case 5: out->laser_start = (int32_t)((uint32_t)p0 | ((uint32_t)p1 << 16)); break;
            case 6: out->laser_stop  = (int32_t)((uint32_t)p0 | ((uint32_t)p1 << 16)); break;
        }
    }
    return 0;
}

/* ---------- float / uint16 转换 ---------- */

void float_to_u16_pair(float f, uint16_t *lo, uint16_t *hi)
{
    uint32_t u;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size");
    memcpy(&u, &f, sizeof(u));
    *lo = (uint16_t)(u & 0xFFFFu);
    *hi = (uint16_t)(u >> 16);
}

float float_from_u16_pair(uint16_t lo, uint16_t hi)
{
    uint32_t u = (uint32_t)lo | ((uint32_t)hi << 16);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* ---------- 串口换层帧 ---------- */

static uint8_t serial_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

static int serial_build_frame(uint8_t port, uint8_t cmd,
                              uint8_t type, uint8_t tlen,
                              const uint8_t *value,
                              uint8_t *out, size_t out_size)
{
    size_t frame_len = 9 + tlen; /* SOF(2)+FrameType+Port+Cmd+Seq+Len+Type+Tlen+Value+Sum */
    if (!out || out_size < frame_len) return -1;

    out[0] = 0x5A;
    out[1] = 0xA5;
    out[2] = 0xAA;       /* SOC->MCU data */
    out[3] = port;
    out[4] = cmd;
    out[5] = 0x01;       /* Seq */
    out[6] = 2 + tlen;   /* Len = Type + Tlen + Value */
    out[7] = type;
    out[8] = tlen;
    if (tlen > 0 && value) memcpy(&out[9], value, tlen);
    out[9 + tlen] = serial_checksum(out, 9 + tlen);
    return (int)frame_len;
}

int serial_build_print_request(uint8_t *out, size_t out_size)
{
    return serial_build_frame(0xA2, 0x40, 0x01, 0, nullptr, out, out_size);
}

int serial_build_print_start(uint16_t layer_height_um, uint8_t *out, size_t out_size)
{
    uint8_t v[2] = { (uint8_t)(layer_height_um & 0xFF), (uint8_t)(layer_height_um >> 8) };
    return serial_build_frame(0xA4, 0x01, 0x03, 2, v, out, out_size);
}

int serial_build_layer_done(uint16_t next_layer_height_um, uint8_t *out, size_t out_size)
{
    uint8_t v[2] = { (uint8_t)(next_layer_height_um & 0xFF), (uint8_t)(next_layer_height_um >> 8) };
    return serial_build_frame(0xA4, 0x01, 0x05, 2, v, out, out_size);
}

int serial_build_print_done(uint8_t *out, size_t out_size)
{
    return serial_build_frame(0xA4, 0x01, 0x06, 0, nullptr, out, out_size);
}

int serial_build_print_abort(uint8_t *out, size_t out_size)
{
    return serial_build_frame(0xA2, 0x40, 0x0B, 0, nullptr, out, out_size);
}

/* 解析 MCU->SOC 数据帧 */
serial_event_t serial_parse_frame(const uint8_t *data, size_t len)
{
    if (!data || len < 10) return SERIAL_EVT_NONE;
    if (data[0] != 0x5A || data[1] != 0xA5) return SERIAL_EVT_NONE;
    if (data[2] != 0xBB) return SERIAL_EVT_NONE; /* 只识别 MCU->SOC 数据帧 */

    uint8_t payload_len = data[6];
    if (len < (size_t)(9 + payload_len - 2)) return SERIAL_EVT_NONE;

    uint8_t calc = serial_checksum(data, 9 + payload_len - 2);
    if (calc != data[9 + payload_len - 2]) return SERIAL_EVT_NONE;

    uint8_t port = data[3];
    uint8_t cmd  = data[4];
    uint8_t type = data[7];

    if (port == 0xA2 && cmd == 0x40 && type == 0x02)
        return SERIAL_EVT_PRINT_PREPARING;

    if (port == 0xA4 && cmd == 0x01 && type == 0x02)
        return SERIAL_EVT_PRINT_CONDITION_OK;

    if (port == 0xA4 && cmd == 0x01 && type == 0x04)
        return SERIAL_EVT_PRINT_READY;

    return SERIAL_EVT_NONE;
}
