#include "serial_debug.hpp"
#include "serial_frame.hpp"

/* ---- TLV Type（与 doc/MCU串口通信协议文档_new.md / def_tlv.h 一致） ---- */
/* 轴运动（Cmd 0xB2/0xB3/0xB4 共用 Type 数值，靠 Cmd 区分轴） */
#define SDBG_TLV_AXIS_JOG       0x01
#define SDBG_TLV_AXIS_INC       0x02
#define SDBG_TLV_AXIS_JOG_STOP  0x03
#define SDBG_TLV_Z_HOME         0x11
#define SDBG_TLV_Z_SET_ZERO     0x12
#define SDBG_TLV_S_HOME         0x10

/* 设备唤醒（Cmd 0xAA） */
#define SDBG_TLV_WAKE_ACTIVE    0x01
#define SDBG_TLV_WAKE_INACTIVE  0x02
/* 电机使能（Cmd 0xB0） */
#define SDBG_TLV_MOTOR_ENABLE   0x01
#define SDBG_TLV_MOTOR_DISABLE  0x02

/* 传感器任务参数设置（Cmd 0x9A） */
#define SDBG_TLV_FAN_PWM        0x01
#define SDBG_TLV_FAN_ON         0x02
#define SDBG_TLV_FAN_OFF        0x03
#define SDBG_TLV_TJ_CLOSED      0x04  /* 三通堵粉 */
#define SDBG_TLV_TJ_OPEN        0x05  /* 三通过粉 */
#define SDBG_TLV_LIGHT_ON       0x06
#define SDBG_TLV_LIGHT_OFF      0x07
#define SDBG_TLV_DOOR_OPEN      0x08
#define SDBG_TLV_INNERDOOR_OPEN  0x0C  /* TLV_INNERDOOR_OPEN：内门开 */
#define SDBG_TLV_INNERDOOR_CLOSE 0x0D  /* TLV_INNERDOOR_CLOSED：内门关 */
#define SDBG_TLV_INNERDOOR_STOP  0x0E  /* TLV_INNERDOOR_STOP：内门停止 */
#define SDBG_TLV_HOT_OPEN       0x0A
#define SDBG_TLV_HOT_CLOSE      0x0B
#define SDBG_TLV_LASER_ENABLE   0x11
#define SDBG_TLV_LASER_DISABLE  0x12
/* 筛粉电机（原三通推杆改为筛粉电机；Cmd 0x9A，无 Value） */
#define SDBG_TLV_SIEVE_RUN      0x13  /* TLV_POWDER_STRAINER_ENABLE：筛粉运行(正转) */
#define SDBG_TLV_SIEVE_REVERSE  0x14  /* TLV_POWDER_STRAINER_DISABLE：反转倒渣 */
#define SDBG_TLV_SIEVE_STOP     0x15  /* TLV_POWDER_MOTOR_STOP：筛粉停止 */

/* 粉桶（Cmd 0x8A） */
#define SDBG_TLV_POWDER_ON      0x01
#define SDBG_TLV_POWDER_OFF     0x02
/* 洗气（Cmd 0x86） */
#define SDBG_TLV_GAS_ON         0x01
#define SDBG_TLV_GAS_OFF        0x02
/* 清粉测试（Cmd 0x81） */
#define SDBG_TLV_CLEAN_START    0x20
/* 打印层参数（Cmd 0xB5） */
#define SDBG_TLV_PRINT_LAYER    0x01
/* 激光器手动功率（Cmd 0x87） */
#define SDBG_TLV_LASER_PWR_SET    0x01
#define SDBG_TLV_LASER_PWR_RESET  0x02
/* 报警复位（Cmd 0x82） */
#define SDBG_TLV_ALARM_RESET      0x01
/* 激光器状态上报（Cmd 0x9B） */
#define SDBG_TLV_LASER_REPORT     0x03
/* 激光器总报警号 */
#define SDBG_LASER_ALARM_CODE     1301

/* ---- Cmd ---- */
#define SDBG_CMD_MACHINE_WAKE   0xAA
#define SDBG_CMD_MOTOR_ENABLE   0xB0
#define SDBG_CMD_Z_MOVE         0xB2
#define SDBG_CMD_S_MOVE         0xB3
#define SDBG_CMD_D_MOVE         0xB4
#define SDBG_CMD_SENSOR_SET     0x9A
#define SDBG_CMD_SENSOR_REPORT  0x9B
/* 运动轴位置上报：SensorsTask 复用 CMD_SNESOR_VALUE_REPORT(0x9B)，Port 0xA5 + TLV 0x05 */
#define SDBG_TLV_MOTION_POS_REPORT 0x05
#define SDBG_CMD_POWDER_SUPPLY  0x8A
#define SDBG_CMD_GAS_CONTROL    0x86
#define SDBG_CMD_PRINTING_CTRL  0x81
#define SDBG_CMD_PRINTING_LAYER 0xB5
#define SDBG_CMD_LASER_MANUAL   0x87
#define SDBG_CMD_ALARM_RESP     0x82

static void sdbg_put_le16(uint8_t* p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFu);
    p[1] = (uint8_t)(((uint16_t)v >> 8u) & 0xFFu);
}

static int32_t sdbg_read_le32(const uint8_t* p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
                     ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u));
}

/** axis(0=Z 1=S 2=D) → Cmd；非法轴返回 0 */
static uint8_t sdbg_axis_cmd(uint8_t axis)
{
    switch(axis) {
        case 0: return SDBG_CMD_Z_MOVE;
        case 1: return SDBG_CMD_S_MOVE;
        case 2: return SDBG_CMD_D_MOVE;
        default: return 0;
    }
}

/* ============================== Tab1 三轴 ============================== */

int serial_debug_build_axis_jog(uint8_t axis, int16_t speed,
                                uint8_t* out, size_t out_size)
{
    const uint8_t cmd = sdbg_axis_cmd(axis);
    if(cmd == 0) {
        return -1;
    }
    uint8_t v[2];
    sdbg_put_le16(v, speed);
    return serial_build_aa_frame(SDBG_PORT_GENERAL, cmd,
                                 SDBG_TLV_AXIS_JOG, 2, v, out, out_size);
}

int serial_debug_build_axis_jog_stop(uint8_t axis, uint8_t* out, size_t out_size)
{
    const uint8_t cmd = sdbg_axis_cmd(axis);
    if(cmd == 0) {
        return -1;
    }
    return serial_build_aa_frame(SDBG_PORT_GENERAL, cmd,
                                 SDBG_TLV_AXIS_JOG_STOP, 0, nullptr, out, out_size);
}

int serial_debug_build_axis_inc(uint8_t axis, int16_t delta,
                                uint8_t* out, size_t out_size)
{
    const uint8_t cmd = sdbg_axis_cmd(axis);
    if(cmd == 0) {
        return -1;
    }
    uint8_t v[2];
    sdbg_put_le16(v, delta);
    return serial_build_aa_frame(SDBG_PORT_GENERAL, cmd,
                                 SDBG_TLV_AXIS_INC, 2, v, out, out_size);
}

int serial_debug_build_axis_home(uint8_t axis, uint8_t* out, size_t out_size)
{
    /* Z→0x11，S→0x10；D 无回 Home */
    if(axis == 0) {
        return serial_build_aa_frame(SDBG_PORT_GENERAL, SDBG_CMD_Z_MOVE,
                                     SDBG_TLV_Z_HOME, 0, nullptr, out, out_size);
    }
    if(axis == 1) {
        return serial_build_aa_frame(SDBG_PORT_GENERAL, SDBG_CMD_S_MOVE,
                                     SDBG_TLV_S_HOME, 0, nullptr, out, out_size);
    }
    return -1;
}

int serial_debug_build_z_set_zero(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_GENERAL, SDBG_CMD_Z_MOVE,
                                 SDBG_TLV_Z_SET_ZERO, 0, nullptr, out, out_size);
}

/* ============================ 设备快捷行 ============================== */

int serial_debug_build_machine_wake(bool active, uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_GENERAL, SDBG_CMD_MACHINE_WAKE,
                                 active ? SDBG_TLV_WAKE_ACTIVE : SDBG_TLV_WAKE_INACTIVE,
                                 0, nullptr, out, out_size);
}

int serial_debug_build_door_open(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                 SDBG_TLV_DOOR_OPEN, 0, nullptr, out, out_size);
}

int serial_debug_build_motor_enable(bool enable, uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_GENERAL, SDBG_CMD_MOTOR_ENABLE,
                                 enable ? SDBG_TLV_MOTOR_ENABLE : SDBG_TLV_MOTOR_DISABLE,
                                 0, nullptr, out, out_size);
}

int serial_debug_build_light(bool on, uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                 on ? SDBG_TLV_LIGHT_ON : SDBG_TLV_LIGHT_OFF,
                                 0, nullptr, out, out_size);
}

/* ============================ Tab2 其他控制 =========================== */

int serial_debug_build_powder_bucket(uint8_t bucket_id, bool on,
                                     uint8_t* out, size_t out_size)
{
    const uint8_t v = bucket_id;
    return serial_build_aa_frame(SDBG_PORT_POWDER, SDBG_CMD_POWDER_SUPPLY,
                                 on ? SDBG_TLV_POWDER_ON : SDBG_TLV_POWDER_OFF,
                                 1, &v, out, out_size);
}

int serial_debug_build_sieve_motor(uint8_t action, uint8_t* out, size_t out_size)
{
    /* action 与 DebugMotorAction 对齐：0=停止 1=筛粉运行(正转) 2=反转倒渣 */
    uint8_t tlv;
    switch(action) {
        case 1:  tlv = SDBG_TLV_SIEVE_RUN;     break;
        case 2:  tlv = SDBG_TLV_SIEVE_REVERSE; break;
        default: tlv = SDBG_TLV_SIEVE_STOP;    break;
    }
    return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                 tlv, 0, nullptr, out, out_size);
}

int serial_debug_build_inner_door(uint8_t action, uint8_t* out, size_t out_size)
{
    /* action 与 DebugMotorAction 对齐：0=停止 1=开 2=关 */
    uint8_t tlv;
    switch(action) {
        case 1:  tlv = SDBG_TLV_INNERDOOR_OPEN;  break;
        case 2:  tlv = SDBG_TLV_INNERDOOR_CLOSE; break;
        default: tlv = SDBG_TLV_INNERDOOR_STOP;  break;
    }
    return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                 tlv, 0, nullptr, out, out_size);
}

int serial_debug_build_fan_switch(bool on, uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                 on ? SDBG_TLV_FAN_ON : SDBG_TLV_FAN_OFF,
                                 0, nullptr, out, out_size);
}

int serial_debug_build_fan_duty(uint8_t duty, uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                 SDBG_TLV_FAN_PWM, 1, &duty, out, out_size);
}

int serial_debug_build_switch(uint8_t target, bool on,
                              uint8_t* out, size_t out_size)
{
    switch(target) {
        case 0: /* 激光使能 */
            return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                         on ? SDBG_TLV_LASER_ENABLE : SDBG_TLV_LASER_DISABLE,
                                         0, nullptr, out, out_size);
        case 1: /* 洗气 */
            return serial_build_aa_frame(SDBG_PORT_GAS, SDBG_CMD_GAS_CONTROL,
                                         on ? SDBG_TLV_GAS_ON : SDBG_TLV_GAS_OFF,
                                         0, nullptr, out, out_size);
        case 2: /* 基板加热 */
            return serial_build_aa_frame(SDBG_PORT_SENSORS, SDBG_CMD_SENSOR_SET,
                                         on ? SDBG_TLV_HOT_OPEN : SDBG_TLV_HOT_CLOSE,
                                         0, nullptr, out, out_size);
        default:
            return -1;
    }
}

int serial_debug_build_clean_test_start(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_PRINTING, SDBG_CMD_PRINTING_CTRL,
                                 SDBG_TLV_CLEAN_START, 0, nullptr, out, out_size);
}

int serial_debug_build_print_layer_param(uint16_t layer_um, uint16_t balance_x1000,
                                         uint8_t* out, size_t out_size)
{
    uint8_t v[4];
    sdbg_put_le16(v + 0, static_cast<int16_t>(layer_um));
    sdbg_put_le16(v + 2, static_cast<int16_t>(balance_x1000));
    return serial_build_aa_frame(SDBG_PORT_PRINTING, SDBG_CMD_PRINTING_LAYER,
                                 SDBG_TLV_PRINT_LAYER, 4, v, out, out_size);
}

/* ============================== Laser ================================= */

int serial_debug_build_laser_power_set(uint8_t percent, uint8_t* out, size_t out_size)
{
    if(percent > 100) {
        percent = 100;
    }
    return serial_build_aa_frame(SDBG_PORT_LASER, SDBG_CMD_LASER_MANUAL,
                                 SDBG_TLV_LASER_PWR_SET, 1, &percent, out, out_size);
}

int serial_debug_build_laser_power_reset(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SDBG_PORT_LASER, SDBG_CMD_LASER_MANUAL,
                                 SDBG_TLV_LASER_PWR_RESET, 0, nullptr, out, out_size);
}

int serial_debug_build_laser_alarm_reset(uint8_t* out, size_t out_size)
{
    uint8_t v[2];
    sdbg_put_le16(v, static_cast<int16_t>(SDBG_LASER_ALARM_CODE));  /* 1301 → 15 05 */
    return serial_build_aa_frame(SDBG_PORT_ALARM, SDBG_CMD_ALARM_RESP,
                                 SDBG_TLV_ALARM_RESET, 2, v, out, out_size);
}

static uint16_t sdbg_read_le16(const uint8_t* p)
{
    return static_cast<uint16_t>((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

int serial_frame_is_laser_report(const uint8_t* data, size_t len)
{
    if(!data || len < 9) {
        return 0;
    }
    return (data[0] == SERIAL_SOF0 && data[1] == SERIAL_SOF1 &&
            data[2] == SERIAL_FT_MCU_TO_SOC &&
            data[3] == SDBG_PORT_SENSORS && data[4] == SDBG_CMD_SENSOR_REPORT &&
            data[7] == SDBG_TLV_LASER_REPORT) ? 1 : 0;
}

int serial_parse_laser_report(const uint8_t* data, size_t len,
                              serial_laser_report_t* out)
{
    if(!data || !out) {
        return -1;
    }
    if(len < 9) {
        return -2;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -3;
    }
    if(data[3] != SDBG_PORT_SENSORS || data[4] != SDBG_CMD_SENSOR_REPORT) {
        return -4;
    }
    const uint8_t payload_len = data[6];
    if(payload_len != 0x0A || data[7] != SDBG_TLV_LASER_REPORT || data[8] != 0x08) {
        return -5;
    }
    if(len < (size_t)(8 + payload_len)) {
        return -6;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -7;
    }
    const uint8_t* v = &data[9];
    out->status        = sdbg_read_le16(v + 0);
    out->alarm         = sdbg_read_le16(v + 2);
    out->power_raw     = sdbg_read_le16(v + 4);
    out->pump_temp_raw = sdbg_read_le16(v + 6);
    return 0;
}

/* ========= 运动轴位置上报解析（SensorsTask：0xA5 0x9B + TLV 0x05） ======= */
/* Value 16 字节，顺序 Z / Z工作坐标 / S / D。整帧 = 8 + payload_len(0x12) = 26。 */

int serial_frame_is_motion_pos_report(const uint8_t* data, size_t len)
{
    if(!data || len < 9) {
        return 0;
    }
    return (data[0] == SERIAL_SOF0 && data[1] == SERIAL_SOF1 &&
            data[2] == SERIAL_FT_MCU_TO_SOC &&
            data[3] == SDBG_PORT_SENSORS && data[4] == SDBG_CMD_SENSOR_REPORT &&
            data[7] == SDBG_TLV_MOTION_POS_REPORT) ? 1 : 0;
}

int serial_parse_motion_pos_report(const uint8_t* data, size_t len,
                                   serial_motion_pos_t* out)
{
    if(!data || !out) {
        return -1;
    }
    if(len < 9) {  /* 至少需读到 data[8]（TLV type/len）；整帧 = 8 + payload_len = 26 */
        return -2;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -3;
    }
    if(data[3] != SDBG_PORT_SENSORS || data[4] != SDBG_CMD_SENSOR_REPORT) {
        return -4;
    }
    const uint8_t payload_len = data[6];
    if(payload_len != 0x12 || data[7] != SDBG_TLV_MOTION_POS_REPORT || data[8] != 0x10) {
        return -5;
    }
    if(len < (size_t)(8 + payload_len)) {
        return -6;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -7;
    }

    /* Value 顺序：Z(0..3) / Z工作坐标(4..7) / S(8..11) / D(12..15) */
    const uint8_t* v = &data[9];
    out->z_pos_mm       = (float)sdbg_read_le32(v + 0)  / 10000.0f;
    out->z_work_zero_mm = (float)sdbg_read_le32(v + 4)  / 10000.0f;
    out->s_pos_mm       = (float)sdbg_read_le32(v + 8)  / 10.0f;
    out->d_pos_deg      = (float)sdbg_read_le32(v + 12) / 10.0f;
    return 0;
}
