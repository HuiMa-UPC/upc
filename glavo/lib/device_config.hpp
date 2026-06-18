#ifndef DEVICE_CONFIG_HPP
#define DEVICE_CONFIG_HPP

/** TCP / 串口默认连接参数 */
#ifndef GLAVO_TCP_DEFAULT_IP
#define GLAVO_TCP_DEFAULT_IP  "192.168.1.30"
#endif

#ifndef GLAVO_TCP_DEFAULT_PORT
#define GLAVO_TCP_DEFAULT_PORT  1502
#endif

#ifndef GLAVO_SERIAL_DEFAULT_DEVICE
#define GLAVO_SERIAL_DEFAULT_DEVICE  "/dev/ttyS3"
#endif

#ifndef GLAVO_SERIAL_DEFAULT_BAUD
#define GLAVO_SERIAL_DEFAULT_BAUD  115200
#endif

/** 换层：0=跳过 M3000 等待与串口换层；1=完整换层流程 */
#ifndef GLAVO_ENABLE_LAYER_CHANGE
#define GLAVO_ENABLE_LAYER_CHANGE  1
#endif

/** 运动下发：每轮 batch 包数与轮询间隔 */
#ifndef TCP_BATCH_SIZE
#define TCP_BATCH_SIZE  100
#endif

#ifndef TCP_SEND_INTERVAL_MS
#define TCP_SEND_INTERVAL_MS  5
#endif

#ifndef TCP_HEARTBEAT_INTERVAL_MS
#define TCP_HEARTBEAT_INTERVAL_MS  200
#endif

#ifndef TCP_RECONNECT_INTERVAL_MS
#define TCP_RECONNECT_INTERVAL_MS  1000
#endif

/** Ring → Stage 二级队列 */
#ifndef TCP_STAGE_MAX_PACKETS
#define TCP_STAGE_MAX_PACKETS  4096
#endif

#ifndef REFILL_STAGE_MAX_PACKETS
#define REFILL_STAGE_MAX_PACKETS  200
#endif

/** cmd 缓存背压：≥ 80% 暂停运动下发 */
#ifndef MCU_CMD_BACKPRESSURE_PCT
#define MCU_CMD_BACKPRESSURE_PCT  80
#endif

/** 完成判定：cmd/data rate ≤ 此值视为下位机缓存已排空（0=严格归零） */
#ifndef MCU_DRAIN_MAX_RATE
#define MCU_DRAIN_MAX_RATE  0
#endif

/** 主机包已发完但 rate 长期不降时的强制完成超时（毫秒） */
#ifndef MCU_DRAIN_TIMEOUT_MS
#define MCU_DRAIN_TIMEOUT_MS  60000
#endif

/** 串口 ACK：0=关闭（兼容旧 MCU）；1=AA 等 BA + BB 回 AB + 超时重传 */
#ifndef GLAVO_SERIAL_ACK_ENABLE
#define GLAVO_SERIAL_ACK_ENABLE  0
#endif

#ifndef GLAVO_SERIAL_ACK_TIMEOUT_MS
#define GLAVO_SERIAL_ACK_TIMEOUT_MS  500
#endif

/** 首次发送后的重传次数（不含首发） */
#ifndef GLAVO_SERIAL_ACK_MAX_RETRY
#define GLAVO_SERIAL_ACK_MAX_RETRY  3
#endif

/** 换层排空后向 Camera 发 Layer_Snap_Ntf：0=关；1=每层拍照录像 */
#ifndef GLAVO_ENABLE_LAYER_SNAP
#define GLAVO_ENABLE_LAYER_SNAP  1
#endif

#endif // DEVICE_CONFIG_HPP
