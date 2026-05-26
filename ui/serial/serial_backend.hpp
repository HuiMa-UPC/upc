#ifndef SERIAL_BACKEND_H
#define SERIAL_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  Serial Backend  (/dev/ttyS1)
 *  ----------------------------------------------------------------------------
 *  本模块封装与下位 MCU 通过 /dev/ttyS1 之间的命令收发。所有 TX 函数构造一条
 *  ASCII 命令帧通过串口发出；后台读线程将接收到的数据解析后写入全局状态结构，
 *  UI 通过 serial_backend_get_status() 周期性查询并刷新显示。
 *
 *  命令帧采用 "TAG K=V K=V\n" 文本格式，便于使用串口示波器/逻辑分析仪调试，
 *  实际协议请根据 MCU 端实现自行替换 send_line() 中的 payload 构造。
 * ============================================================================ */

#define SERIAL_DEFAULT_DEVICE   "/dev/ttyS4"
#define SERIAL_DEFAULT_BAUD     115200

/* ---------------- 生命周期 ---------------- */
int   serial_backend_init(const char *device, int baud);
void  serial_backend_deinit(void);
bool  serial_backend_is_open(void);
const char *serial_backend_device(void);
int   serial_backend_baud(void);

/* ---------------- 全局状态（线程安全） ----------------
 *  UI 读取时只需调用 serial_backend_get_status() 一次拷贝快照即可，无须加锁。
 */
typedef struct {
    /* === Motor Tab：解析数据 === */
    int32_t  z_pos;
    int32_t  s_pos;
    int32_t  d_pos;
    bool     z_pos_valid;
    bool     s_pos_valid;
    bool     d_pos_valid;

    /* === Print Tab：传感器/状态 === */
    float    o2_pct;        /* 氧含量 % */
    float    chamber_kpa;   /* 仓内压力 kPa */
    float    temp_c;        /* 温度 ℃ */
    float    humidity_pct;  /* 湿度 % */
    float    pm25_ugm3;     /* PM2.5 μg/m³ */
    int32_t  z_work_pos;    /* Z 轴工作坐标 */
    bool     o2_valid;
    bool     chamber_valid;
    bool     temp_valid;
    bool     humidity_valid;
    bool     pm25_valid;
    bool     z_work_valid;

    /* === 状态指示 === */
    bool     laser_on;
    bool     wash_gas_on;
    bool     light_on;
    bool     door_closed;
    bool     enabled;       /* 使能/失能 */
    bool     awake;         /* 唤醒/睡眠 */
    bool     fan_running;
    uint8_t  fan_duty_pct;

    /* === Settings Tab：粉桶状态 === */
    int8_t   bucket1_state; /* -1 未知 / 0 关 / 1 开 */
    int8_t   bucket2_state;

    /* === 统计 === */
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_lines;
} serial_status_t;

void  serial_backend_get_status(serial_status_t *out);

/* ============================================================================
 *  Motor Tab 接口
 * ============================================================================ */
typedef enum {
    JOG_AXIS_Z = 0,
    JOG_AXIS_S = 1,
    JOG_AXIS_D = 2,
} jog_axis_t;

typedef enum {
    JOG_DIR_POS = 0,   /* Z: 上,  S: 左,  D: 顺时针 */
    JOG_DIR_NEG = 1,   /* Z: 下,  S: 右,  D: 逆时针 */
} jog_dir_t;

/** Jog 单次：瞬时下发一条命令（保留供其他场景调用） */
void serial_motor_jog(jog_axis_t axis, jog_dir_t dir, float speed);

/** Jog 按下：长按持续运动起点（UI 在 LV_EVENT_PRESSED 时调用） */
void serial_motor_jog_start(jog_axis_t axis, jog_dir_t dir, float speed);

/** Jog 松开：停止运动（UI 在 LV_EVENT_RELEASED 时调用） */
void serial_motor_jog_stop(jog_axis_t axis);

/** 单轴归零（Z/S 轴 reset）*/
void serial_motor_home(jog_axis_t axis);

/** 增量移动（distance 可正可负，单位与 MCU 端约定，例如 mm/步）*/
void serial_motor_increment(jog_axis_t axis, jog_dir_t dir, float distance);

/* ============================================================================
 *  Print Tab 接口
 * ============================================================================ */
void serial_print_set_z_work_zero(void);

/** 三通推杆：true=堵粉 / false=放粉 */
void serial_print_three_way(bool block);

/** 回粉电机：-1 反转 / 0 关 / +1 正转 */
void serial_print_powder_motor(int dir);

void serial_print_laser_set(bool on);
void serial_print_wash_gas_set(bool on);

/** 风机控制 */
void serial_print_fan_set(bool on);
void serial_print_fan_duty(uint8_t pct);

/* ============================================================================
 *  Settings Tab 接口
 * ============================================================================ */
/** 粉桶开关：idx=1/2，open=true 开 / false 关 */
void serial_settings_bucket_set(int idx, bool open);

/* ============================================================================
 *  右侧功能按键接口
 * ============================================================================ */
void serial_func_wake(void);
void serial_func_sleep(void);
void serial_func_enable(void);
void serial_func_disable(void);
void serial_func_light_set(bool on);
void serial_func_door_query(void);   /* 主动查询门状态 */
void serial_func_door_set(bool open); /* 电控门锁：true=打开/false=关闭 */

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_BACKEND_H */
