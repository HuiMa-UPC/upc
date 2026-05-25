#ifndef PROTOCOL_MCU_H
#define PROTOCOL_MCU_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- TCP 帧 ---------- */

/* 构造运动帧：subpkts11 为 n 个连续 11B 子包. 返回帧总长度 */
int build_motion_frame(const uint8_t *subpkts11, int n,
                       uint8_t *out, size_t out_size);

/* 构造心跳帧 */
int build_heartbeat_frame(uint8_t *out, size_t out_size);

/* 构造复位帧 */
int build_reset_frame(uint8_t *out, size_t out_size);

/* 构造读参帧 */
int build_readparam_frame(uint8_t *out, size_t out_size);

/* 构造写参帧 */
int build_writeparam_frame(uint8_t obj,
                           uint16_t p0, uint16_t p1,
                           uint16_t p2, uint16_t p3,
                           uint8_t *out, size_t out_size);

/* 解析常态应答(19B). 返回0成功 */
typedef struct {
    uint16_t cmd_rate;      /* 0..65535 */
    uint16_t data_rate;     /* 0..65535 */
    uint8_t  flash_flag;    /* 0x0D = saved */
    uint16_t current_x;     /* little-endian */
    uint16_t current_y;
    uint16_t current_z;
    uint16_t laser_state;
} tcp_response_t;

int parse_tcp_response(const uint8_t *data, size_t len, tcp_response_t *resp);

/* 解析读参应答(81B). 返回0成功 */
typedef struct {
    float    a_max;
    uint8_t  fly_enable;
    float    fly_gain;
    uint8_t  acc_enable;
    float    acc_delat;
    int32_t  laser_start;
    int32_t  laser_stop;
} param_set_t;

int parse_readparam_response(const uint8_t *data, size_t len, param_set_t *out);

/* float <-> uint16 pair (IEEE754, little-endian pair) */
void float_to_u16_pair(float f, uint16_t *lo, uint16_t *hi);
float float_from_u16_pair(uint16_t lo, uint16_t hi);

/* ---------- 串口换层帧 ---------- */

/* 串口帧打包. 返回帧长度(含校验和), <=0表示错误 */
int serial_build_print_request (uint8_t *out, size_t out_size);
int serial_build_print_start    (uint16_t layer_height_um, uint8_t *out, size_t out_size);
int serial_build_layer_done     (uint16_t next_layer_height_um, uint8_t *out, size_t out_size);
int serial_build_print_finished (uint8_t *out, size_t out_size);

/* 串口帧解析. 返回命令类型, <0表示未识别或校验失败 */
typedef enum {
    SERIAL_EVT_NONE = 0,
    SERIAL_EVT_PRINT_READY,         /* 打印准备完成(可以出光) */
    SERIAL_EVT_PRINT_CONDITION_OK,  /* 打印条件满足 */
} serial_event_t;

serial_event_t serial_parse_frame(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
