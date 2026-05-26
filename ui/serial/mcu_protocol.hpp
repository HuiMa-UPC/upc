#ifndef MCU_PROTOCOL_H
#define MCU_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  MCU UART 协议编解码（纯字节级，零 I/O）
 *  ----------------------------------------------------------------------------
 *  帧格式：SOF1 SOF2 FrameType Port Cmd Seq Len Payload(TLV...) CheckSum
 *      Sum = (uint8_t)Σ(SOF1 .. Payload 末字节)
 *  当前阶段：固定 FrameType=0xAA (上位机请求)、Seq=0x01
 *  详见 docs/MCU串口通信协议.md
 * ============================================================================ */

/* ---- 帧头 / FrameType / 固定字段 ---- */
#define MCU_SOF1                    0x5A
#define MCU_SOF2                    0xA5
#define MCU_FT_REQUEST              0xAA   /* 上位机→MCU 业务命令 */
#define MCU_FT_REQ_ACK              0xBA   /* MCU→上位机 ACK */
#define MCU_FT_REPORT               0xBB   /* MCU→上位机 主动上报 */
#define MCU_FT_REPORT_ACK           0xAB   /* 上位机→MCU 对上报 ACK */
#define MCU_SEQ_FIXED               0x01   /* 当前阶段固定 Seq=1 */

/* ---- Port ---- */
#define MCU_PORT_GAS                0xA1   /* PORT_GAS_TASK */
#define MCU_PORT_GENERAL            0xA2   /* PORT_GENERAL_TASK */
#define MCU_PORT_ALARM              0xA3   /* PORT_ALARM_TASK */
#define MCU_PORT_SENSORS            0xA5   /* PORT_SENSORS_TASK */
#define MCU_PORT_MOTION             0xA7   /* PORT_MOTION_TASK */
#define MCU_PORT_POWDER             0xA9   /* PORT_POWDER_TASK */

/* ---- Cmd ---- */
#define MCU_CMD_GAS_MANUAL_CONTROL  0x81   /* on MCU_PORT_GAS */
#define MCU_CMD_POWDER_SUPPLY       0x81   /* on MCU_PORT_POWDER */
#define MCU_CMD_ALARM_EVT_REPORT    0x81   /* on MCU_PORT_ALARM */
#define MCU_CMD_SENSOR_SET_PARA     0x9A   /* on MCU_PORT_SENSORS */
#define MCU_CMD_SENSOR_VALUE_REPORT 0x9B   /* on MCU_PORT_SENSORS */
#define MCU_CMD_POST_RESULT         0xA1   /* on MCU_PORT_GENERAL */
#define MCU_CMD_MACHINE_WAKE        0xAA   /* on MCU_PORT_GENERAL */
#define MCU_CMD_MOTOR_ENABLE        0xB0   /* on MCU_PORT_GENERAL */
#define MCU_CMD_MOTION_STATUS       0xB1   /* on MCU_PORT_GENERAL/MOTION */
#define MCU_CMD_MOTION_Z_MOVE       0xB2
#define MCU_CMD_MOTION_S_MOVE       0xB3
#define MCU_CMD_MOTION_D_MOVE       0xB4

/* ---- TLV-Type （按 Cmd 分组） ---- */
/* CMD_MOTOR_ENABLE */
#define MCU_TLV_MOTOR_ENABLE         0x01
#define MCU_TLV_MOTOR_DISENABLE      0x02
#define MCU_TLV_MOTOR_ENABLED        0x03   /* RX 用 */
#define MCU_TLV_MOTOR_DISENABLED     0x04   /* RX 用 */
#define MCU_TLV_MOTOR_ENABLE_FAILED  0x05   /* RX 用 */

/* CMD_MOTION_STATUS（多为 RX） */
#define MCU_TLV_MOTION_ACTIVE        0x01
#define MCU_TLV_MOTION_INACTIVE      0x02
#define MCU_TLV_MOTION_ACTUAL_POS    0x03

/* CMD_MOTION_Z_MOVE */
#define MCU_TLV_Z_JOG                0x01
#define MCU_TLV_Z_INC                0x02
#define MCU_TLV_Z_JOG_STOP           0x03
#define MCU_TLV_Z_HOME               0x11
#define MCU_TLV_Z_SET_ZERO           0x12

/* CMD_MOTION_S_MOVE */
#define MCU_TLV_S_JOG                0x01
#define MCU_TLV_S_INC                0x02
#define MCU_TLV_S_JOG_STOP           0x03
#define MCU_TLV_S_HOME               0x10   /* 注意：S=0x10 / Z=0x11 不同 */

/* CMD_MOTION_D_MOVE */
#define MCU_TLV_D_JOG                0x01
#define MCU_TLV_D_INC                0x02
#define MCU_TLV_D_JOG_STOP           0x03

/* CMD_MACHINE_WAKE */
#define MCU_TLV_WAKE_ACTIVE          0x01
#define MCU_TLV_WAKE_INACTIVE        0x02
#define MCU_TLV_WAKE_PARAMETER       0x03

/* CMD_GAS_MANUAL_CONTROL */
#define MCU_TLV_GAS_SCRUB_ON         0x01
#define MCU_TLV_GAS_SCRUB_OFF        0x02

/* CMD_SENSOR_SET_PARA */
#define MCU_TLV_FAN_PWM_PARA         0x01   /* Tlv-Len=1, uint8 [35,100] */
#define MCU_TLV_FAN_SWITCH_ON        0x02
#define MCU_TLV_FAN_SWITCH_OFF       0x03
#define MCU_TLV_POWDER_TJ_CLOSED     0x04   /* 三通堵粉（Block） */
#define MCU_TLV_POWDER_TJ_OPEN       0x05   /* 三通过粉（Release） */
#define MCU_TLV_LIGHT_ON             0x06
#define MCU_TLV_LIGHT_OFF            0x07
#define MCU_TLV_DOOR_OPEN            0x08
#define MCU_TLV_DOOR_CLOSE           0x09
#define MCU_TLV_LASER_ENABLE         0x11
#define MCU_TLV_LASER_DISABLE        0x12

/* CMD_SENSOR_VALUE_REPORT（多为 RX） */
#define MCU_TLV_SENSOR_REPORT_1ST    0x02

/* CMD_POWDER_SUPPLY */
#define MCU_TLV_POWDER_SWITCH_ON     0x01   /* Tlv-Len=1, value[0]=1/2 桶号 */
#define MCU_TLV_POWDER_SWITCH_OFF    0x02

/* ---- 容量 ---- */
#define MCU_FRAME_MAX_LEN           64   /* 整帧上限，调用方应至少传入此长度的 out 缓冲 */

/* ============================================================================
 *  TX：每个 pack 函数把整帧字节写入 out[0..return-1]，返回总字节数
 *      失败返回 -1（out=NULL / cap 不足 / 参数越界）
 * ============================================================================ */

/* ---- Z 轴 ---- */
int mcu_pack_z_jog_speed   (uint8_t *out, size_t cap, int16_t mm_per_s);
int mcu_pack_z_jog_stop    (uint8_t *out, size_t cap);
int mcu_pack_z_inc         (uint8_t *out, size_t cap, int16_t mm);
int mcu_pack_z_home        (uint8_t *out, size_t cap);
int mcu_pack_z_set_zero    (uint8_t *out, size_t cap);

/* ---- S 轴 ---- */
int mcu_pack_s_jog_speed   (uint8_t *out, size_t cap, int16_t mm_per_s);
int mcu_pack_s_jog_stop    (uint8_t *out, size_t cap);
int mcu_pack_s_inc         (uint8_t *out, size_t cap, int16_t mm);
int mcu_pack_s_home        (uint8_t *out, size_t cap);

/* ---- D 轴 ---- */
int mcu_pack_d_jog_speed   (uint8_t *out, size_t cap, int16_t deg_per_s);
int mcu_pack_d_jog_stop    (uint8_t *out, size_t cap);
int mcu_pack_d_inc         (uint8_t *out, size_t cap, int16_t deg);

/* ---- 电机使能 ---- */
int mcu_pack_motor_enable  (uint8_t *out, size_t cap);
int mcu_pack_motor_disable (uint8_t *out, size_t cap);

/* ---- 设备唤醒 ---- */
int mcu_pack_wake_active   (uint8_t *out, size_t cap);
int mcu_pack_wake_inactive (uint8_t *out, size_t cap);

/* ---- 三通推杆 / 激光 / 风机 / 照明 / 门控（PORT_SENSORS） ---- */
int mcu_pack_three_way_close (uint8_t *out, size_t cap);
int mcu_pack_three_way_open  (uint8_t *out, size_t cap);
int mcu_pack_laser_enable    (uint8_t *out, size_t cap);
int mcu_pack_laser_disable   (uint8_t *out, size_t cap);
int mcu_pack_fan_on          (uint8_t *out, size_t cap);
int mcu_pack_fan_off         (uint8_t *out, size_t cap);
int mcu_pack_fan_duty        (uint8_t *out, size_t cap, uint8_t pwm_duty);
int mcu_pack_light_on        (uint8_t *out, size_t cap);
int mcu_pack_light_off       (uint8_t *out, size_t cap);
int mcu_pack_door_open       (uint8_t *out, size_t cap);
int mcu_pack_door_close      (uint8_t *out, size_t cap);

/* ---- 洗气（PORT_GAS） ---- */
int mcu_pack_gas_on          (uint8_t *out, size_t cap);
int mcu_pack_gas_off         (uint8_t *out, size_t cap);

/* ---- 粉桶（PORT_POWDER） ---- */
int mcu_pack_bucket_on       (uint8_t *out, size_t cap, uint8_t idx);
int mcu_pack_bucket_off      (uint8_t *out, size_t cap, uint8_t idx);

/* ============================================================================
 *  RX：完整帧字节级状态机
 *  ----------------------------------------------------------------------------
 *  调用方按字节顺序投喂 mcu_proto_feed_byte()；当完整帧通过 SOF + 校验和
 *  验证后，回调 cb(frame, user)。回调里直接读 frame->frame_type/port/cmd/
 *  seq/payload_len/payload[] 即可。
 *
 *  线程安全：状态机内部用单一 static 状态，需调用方在同一线程喂字节
 *  （serial_backend 的 RX 线程是唯一调用点 → 满足）。
 * ============================================================================ */

#define MCU_FRAME_PAYLOAD_MAX   64   /* Payload 上限：协议 Tlv-Len 上限远小于此 */

typedef struct {
    uint8_t  frame_type;                       /* 0xAA/0xBA/0xBB/0xAB */
    uint8_t  port;
    uint8_t  cmd;
    uint8_t  seq;
    uint8_t  payload_len;
    uint8_t  payload[MCU_FRAME_PAYLOAD_MAX];
} mcu_frame_t;

typedef void (*mcu_frame_cb_t)(const mcu_frame_t *frm, void *user);

/** 向状态机喂入一字节；解析到完整有效帧时调用 cb */
void mcu_proto_feed_byte(uint8_t b, mcu_frame_cb_t cb, void *user);

/** 重置状态机（断开/重连串口时调用，避免半帧残留） */
void mcu_proto_reset_rx(void);

/* ---- RX 小工具：字节流转主机端整型（解析 TLV Value 用） ---- */
static inline int16_t mcu_rd_i16_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline int32_t mcu_rd_i32_le(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0]
                   | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16)
                   | ((uint32_t)p[3] << 24));
}

#ifdef __cplusplus
}
#endif

#endif /* MCU_PROTOCOL_H */
