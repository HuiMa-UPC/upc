#ifndef DEVICE_CONFIG_HPP
#define DEVICE_CONFIG_HPP

/** TCP / 串口默认连接参数 */
#ifndef GLAVO_TCP_DEFAULT_IP
#define GLAVO_TCP_DEFAULT_IP  "192.168.1.112"
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
#define GLAVO_ENABLE_LAYER_CHANGE  0
#endif

#ifndef GCODE_DEFAULT_LAYER_UM
#define GCODE_DEFAULT_LAYER_UM  30u
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

#endif // DEVICE_CONFIG_HPP
