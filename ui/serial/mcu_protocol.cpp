#include "mcu_protocol.hpp"
#include <string.h>

/* ============================================================================
 *  TLV 私有工具：每个写入 N 字节到 p[]，返回写入字节数
 * ============================================================================ */

/** 无 Value 的 TLV（Tlv-Len=0），写入 2 字节 */
static inline uint8_t tlv_empty(uint8_t *p, uint8_t type)
{
    p[0] = type;
    p[1] = 0;
    return 2;
}

/** uint8 单字节 Value 的 TLV（Tlv-Len=1），写入 3 字节 */
static inline uint8_t tlv_uint8(uint8_t *p, uint8_t type, uint8_t v)
{
    p[0] = type;
    p[1] = 1;
    p[2] = v;
    return 3;
}

/** int16 小端 Value 的 TLV（Tlv-Len=2），写入 4 字节 */
static inline uint8_t tlv_int16_le(uint8_t *p, uint8_t type, int16_t v)
{
    p[0] = type;
    p[1] = 2;
    p[2] = (uint8_t)(v & 0xFF);
    p[3] = (uint8_t)((v >> 8) & 0xFF);
    return 4;
}

/* ============================================================================
 *  组帧：把 (port, cmd, payload, payload_len) 编码为完整串口帧到 out 缓冲
 *      out[0..return-1]  ←  SOF1 SOF2 FT Port Cmd Seq Len Payload Sum
 *  返回总字节数；失败返回 -1
 * ============================================================================ */
static int encode_frame(uint8_t *out, size_t cap,
                        uint8_t port, uint8_t cmd,
                        const uint8_t *payload, uint8_t payload_len)
{
    if(!out || cap < (size_t)(8 + payload_len)) {
        return -1;
    }
    size_t n = 0;
    out[n++] = MCU_SOF1;
    out[n++] = MCU_SOF2;
    out[n++] = MCU_FT_REQUEST;
    out[n++] = port;
    out[n++] = cmd;
    out[n++] = MCU_SEQ_FIXED;
    out[n++] = payload_len;
    if(payload && payload_len > 0) {
        memcpy(out + n, payload, payload_len);
        n += payload_len;
    }
    uint8_t sum = 0;
    for(size_t i = 0; i < n; i++) sum += out[i];
    out[n++] = sum;
    return (int)n;
}

/* ============================================================================
 *  TX 业务包：每个 pack 函数 → tlv_* + encode_frame
 * ============================================================================ */

/* ----- Z 轴 (CMD_MOTION_Z_MOVE = 0xB2) ----- */
int mcu_pack_z_jog_speed(uint8_t *out, size_t cap, int16_t mm_per_s)
{
    uint8_t pl[4];
    uint8_t n = tlv_int16_le(pl, MCU_TLV_Z_JOG, mm_per_s);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_Z_MOVE, pl, n);
}

int mcu_pack_z_jog_stop(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_Z_JOG_STOP);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_Z_MOVE, pl, n);
}

int mcu_pack_z_inc(uint8_t *out, size_t cap, int16_t mm)
{
    uint8_t pl[4];
    uint8_t n = tlv_int16_le(pl, MCU_TLV_Z_INC, mm);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_Z_MOVE, pl, n);
}

int mcu_pack_z_home(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_Z_HOME);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_Z_MOVE, pl, n);
}

int mcu_pack_z_set_zero(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_Z_SET_ZERO);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_Z_MOVE, pl, n);
}

/* ----- S 轴 (CMD_MOTION_S_MOVE = 0xB3) ----- */
int mcu_pack_s_jog_speed(uint8_t *out, size_t cap, int16_t mm_per_s)
{
    uint8_t pl[4];
    uint8_t n = tlv_int16_le(pl, MCU_TLV_S_JOG, mm_per_s);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_S_MOVE, pl, n);
}

int mcu_pack_s_jog_stop(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_S_JOG_STOP);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_S_MOVE, pl, n);
}

int mcu_pack_s_inc(uint8_t *out, size_t cap, int16_t mm)
{
    uint8_t pl[4];
    uint8_t n = tlv_int16_le(pl, MCU_TLV_S_INC, mm);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_S_MOVE, pl, n);
}

int mcu_pack_s_home(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_S_HOME);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_S_MOVE, pl, n);
}

/* ----- D 轴 (CMD_MOTION_D_MOVE = 0xB4) ----- */
int mcu_pack_d_jog_speed(uint8_t *out, size_t cap, int16_t deg_per_s)
{
    uint8_t pl[4];
    uint8_t n = tlv_int16_le(pl, MCU_TLV_D_JOG, deg_per_s);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_D_MOVE, pl, n);
}

int mcu_pack_d_jog_stop(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_D_JOG_STOP);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_D_MOVE, pl, n);
}

int mcu_pack_d_inc(uint8_t *out, size_t cap, int16_t deg)
{
    uint8_t pl[4];
    uint8_t n = tlv_int16_le(pl, MCU_TLV_D_INC, deg);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTION_D_MOVE, pl, n);
}

/* ----- 电机使能 (CMD_MOTOR_ENABLE = 0xB0) ----- */
int mcu_pack_motor_enable(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_MOTOR_ENABLE);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTOR_ENABLE, pl, n);
}

int mcu_pack_motor_disable(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_MOTOR_DISENABLE);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MOTOR_ENABLE, pl, n);
}

/* ----- 设备唤醒 (CMD_MACHINE_WAKE = 0xAA) ----- */
int mcu_pack_wake_active(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_WAKE_ACTIVE);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MACHINE_WAKE, pl, n);
}

int mcu_pack_wake_inactive(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_WAKE_INACTIVE);
    return encode_frame(out, cap, MCU_PORT_GENERAL, MCU_CMD_MACHINE_WAKE, pl, n);
}

/* ----- 三通推杆 / 激光 / 风机 / 照明 / 门控 (CMD_SENSOR_SET_PARA on PORT_SENSORS) ----- */
int mcu_pack_three_way_close(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_POWDER_TJ_CLOSED);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_three_way_open(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_POWDER_TJ_OPEN);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_laser_enable(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_LASER_ENABLE);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_laser_disable(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_LASER_DISABLE);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_fan_on(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_FAN_SWITCH_ON);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_fan_off(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_FAN_SWITCH_OFF);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_fan_duty(uint8_t *out, size_t cap, uint8_t pwm_duty)
{
    uint8_t pl[3];
    uint8_t n = tlv_uint8(pl, MCU_TLV_FAN_PWM_PARA, pwm_duty);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_light_on(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_LIGHT_ON);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_light_off(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_LIGHT_OFF);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_door_open(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_DOOR_OPEN);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

int mcu_pack_door_close(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_DOOR_CLOSE);
    return encode_frame(out, cap, MCU_PORT_SENSORS, MCU_CMD_SENSOR_SET_PARA, pl, n);
}

/* ----- 洗气 (CMD_GAS_MANUAL_CONTROL on PORT_GAS) ----- */
int mcu_pack_gas_on(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_GAS_SCRUB_ON);
    return encode_frame(out, cap, MCU_PORT_GAS, MCU_CMD_GAS_MANUAL_CONTROL, pl, n);
}

int mcu_pack_gas_off(uint8_t *out, size_t cap)
{
    uint8_t pl[2];
    uint8_t n = tlv_empty(pl, MCU_TLV_GAS_SCRUB_OFF);
    return encode_frame(out, cap, MCU_PORT_GAS, MCU_CMD_GAS_MANUAL_CONTROL, pl, n);
}

/* ----- 粉桶 (CMD_POWDER_SUPPLY on PORT_POWDER) ----- */
int mcu_pack_bucket_on(uint8_t *out, size_t cap, uint8_t idx)
{
    uint8_t pl[3];
    uint8_t n = tlv_uint8(pl, MCU_TLV_POWDER_SWITCH_ON, idx);
    return encode_frame(out, cap, MCU_PORT_POWDER, MCU_CMD_POWDER_SUPPLY, pl, n);
}

int mcu_pack_bucket_off(uint8_t *out, size_t cap, uint8_t idx)
{
    uint8_t pl[3];
    uint8_t n = tlv_uint8(pl, MCU_TLV_POWDER_SWITCH_OFF, idx);
    return encode_frame(out, cap, MCU_PORT_POWDER, MCU_CMD_POWDER_SUPPLY, pl, n);
}

/* ============================================================================
 *  RX 字节状态机
 *  ----------------------------------------------------------------------------
 *  状态流转：
 *      SOF1 → (0x5A?) → SOF2
 *      SOF2 → (0xA5?) → FT；若收到 0x5A 仍停留在 SOF2；其余回退 SOF1
 *      FT/Port/Cmd/Seq/Len → 顺序填字段，累加 sum
 *      PAYLOAD → 收 Len 字节
 *      SUM → 与累加值对比，匹配则回调，无论成败回到 SOF1
 * ============================================================================ */
typedef enum {
    RX_S_SOF1 = 0,
    RX_S_SOF2,
    RX_S_FT,
    RX_S_PORT,
    RX_S_CMD,
    RX_S_SEQ,
    RX_S_LEN,
    RX_S_PAYLOAD,
    RX_S_SUM,
} rx_state_t;

static struct {
    rx_state_t   state;
    mcu_frame_t  frm;
    uint16_t     pl_idx;
    uint8_t      sum_calc;   /* SOF1..Payload 末字节累加（不含 Sum 自身） */
} g_rx = { RX_S_SOF1, {0, 0, 0, 0, 0, {0}}, 0, 0 };

void mcu_proto_reset_rx(void)
{
    g_rx.state   = RX_S_SOF1;
    g_rx.pl_idx  = 0;
    g_rx.sum_calc = 0;
}

void mcu_proto_feed_byte(uint8_t b, mcu_frame_cb_t cb, void *user)
{
    switch(g_rx.state) {
    case RX_S_SOF1:
        if(b == MCU_SOF1) {
            g_rx.sum_calc = b;
            g_rx.state    = RX_S_SOF2;
        }
        break;

    case RX_S_SOF2:
        if(b == MCU_SOF2) {
            g_rx.sum_calc += b;
            g_rx.state     = RX_S_FT;
        } else if(b == MCU_SOF1) {
            /* 连续 5A：把当前 5A 当成新帧起点继续等 SOF2 */
            g_rx.sum_calc = b;
        } else {
            g_rx.state = RX_S_SOF1;
        }
        break;

    case RX_S_FT:
        g_rx.frm.frame_type = b;
        g_rx.sum_calc       += b;
        g_rx.state           = RX_S_PORT;
        break;

    case RX_S_PORT:
        g_rx.frm.port  = b;
        g_rx.sum_calc += b;
        g_rx.state     = RX_S_CMD;
        break;

    case RX_S_CMD:
        g_rx.frm.cmd   = b;
        g_rx.sum_calc += b;
        g_rx.state     = RX_S_SEQ;
        break;

    case RX_S_SEQ:
        g_rx.frm.seq   = b;
        g_rx.sum_calc += b;
        g_rx.state     = RX_S_LEN;
        break;

    case RX_S_LEN:
        g_rx.frm.payload_len = b;
        g_rx.sum_calc       += b;
        g_rx.pl_idx          = 0;
        if(b > MCU_FRAME_PAYLOAD_MAX) {
            /* 越界保护：丢弃，重新找帧头 */
            g_rx.state = RX_S_SOF1;
        } else {
            g_rx.state = (b == 0) ? RX_S_SUM : RX_S_PAYLOAD;
        }
        break;

    case RX_S_PAYLOAD:
        g_rx.frm.payload[g_rx.pl_idx++] = b;
        g_rx.sum_calc                   += b;
        if(g_rx.pl_idx >= g_rx.frm.payload_len) {
            g_rx.state = RX_S_SUM;
        }
        break;

    case RX_S_SUM:
        if(g_rx.sum_calc == b && cb) {
            cb(&g_rx.frm, user);
        }
        /* 不论校验成败，开始下一帧 */
        g_rx.state = RX_S_SOF1;
        break;
    }
}
