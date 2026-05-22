#ifndef TCPCLIENT_BACKEND_H
#define TCPCLIENT_BACKEND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TCP_STATE_DISCONNECTED = 0,
    TCP_STATE_CONNECTED,
    TCP_STATE_REMOTE_CLOSED,
    TCP_STATE_ERROR
} tcp_state_t;

typedef void (*tcp_state_cb_t)(tcp_state_t state, const char *status_msg, void *user_data);
typedef void (*tcp_data_cb_t)(const char *data, size_t len, void *user_data);

/** 初始化后端（创建轮询定时器），可重复调用 */
void tcp_backend_init(void);

/** 清理后端资源（关闭 socket、删除定时器） */
void tcp_backend_deinit(void);

/** 注册状态变化回调 */
void tcp_backend_set_state_cb(tcp_state_cb_t cb, void *user_data);

/** 注册数据接收回调 */
void tcp_backend_set_data_cb(tcp_data_cb_t cb, void *user_data);

/** 连接服务器（阻塞，最多 2s 超时）。返回 0 成功，负数表示各种错误 */
int  tcp_backend_connect(const char *ip, int port);

/** 断开连接 */
void tcp_backend_disconnect(void);

/** 发送数据。返回 0 成功，-1 失败 */
int  tcp_backend_send(const char *data, size_t len);

/** 是否已连接 */
int  tcp_backend_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
