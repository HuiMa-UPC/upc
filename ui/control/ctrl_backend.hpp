#ifndef CTRL_BACKEND_H
#define CTRL_BACKEND_H

#include "ring_buffer.hpp"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 初始化/反初始化 ---------- */
void ctrl_backend_init(void);

/* ---------- TCP ---------- */
int  ctrl_tcp_connect(const char *ip, int port);
void ctrl_tcp_disconnect(void);
int  ctrl_tcp_is_connected(void);

/* ---------- 串口 ---------- */
int  ctrl_serial_open(const char *device, int baud);
void ctrl_serial_close(void);
int  ctrl_serial_is_open(void);

/* ---------- RingBuffer 绑定 ---------- */
void ctrl_set_ring_buffer(RingBuffer *rb);

/* ---------- 发送控制 ---------- */
void ctrl_start_sending(void);
void ctrl_stop_sending(void);
void ctrl_pause_sending(void);
void ctrl_resume_sending(void);
int  ctrl_is_sending(void);
int  ctrl_is_paused(void);

/** 换层等待：1=处于换层/首层串口握手等待，0=否 */
int  ctrl_is_waiting_layer(void);

/* ---------- 复位 ---------- */
void ctrl_send_reset(void);

/* ---------- 参数设置 ---------- */
void ctrl_set_batch_size(int n);
void ctrl_set_interval_ms(int ms);
void ctrl_set_enable_layer_change(int en);

void ctrl_set_enable_cal_apply(int en);
int  ctrl_get_enable_cal_apply(void);

/* ---------- 板参数读写 ---------- */
void ctrl_request_read_params(void);
/** @return 0 已下发；<0 失败（-1 未连接 TCP；-2 曾处于发送态已先 Stop） */
int ctrl_commit_params(float a_max, uint8_t fly_en, float fly_gain,
                       uint8_t acc_en, float acc_delat,
                       int32_t laser_start, int32_t laser_stop);

#ifdef __cplusplus
}
#endif

#endif
