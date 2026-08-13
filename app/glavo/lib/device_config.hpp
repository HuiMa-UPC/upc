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

/** 打印前基板准备（TLV 0x12/0x13）：0=跳过，直接发打印请求；1=发请求并等待 MCU 结果 */
#ifndef GLAVO_ENABLE_PLATE_PREPARE
#define GLAVO_ENABLE_PLATE_PREPARE  0
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

/** TCP 连接(connect)等待上限（毫秒）：非阻塞 connect + poll 等待，避免
 * 阻塞式 connect 卡住链路线程进而拖延串口 TX。振镜离线时单次重连最多阻塞此值。 */
#ifndef GLAVO_TCP_CONNECT_TIMEOUT_MS
#define GLAVO_TCP_CONNECT_TIMEOUT_MS  200
#endif

/** Ring → Stage 二级队列 */
#ifndef TCP_STAGE_MAX_PACKETS
#define TCP_STAGE_MAX_PACKETS  4096
#endif

//每轮从 RingBuffer 抽到 tcp_stage_ 的上限
#ifndef REFILL_STAGE_MAX_PACKETS
#define REFILL_STAGE_MAX_PACKETS  500
#endif

/** cmd 缓存背压：≥ 80% 暂停运动下发 */
#ifndef MCU_CMD_BACKPRESSURE_PCT
#define MCU_CMD_BACKPRESSURE_PCT  80
#endif

/** 小内存下位机（版本 >= GLAVO_SMALL_MEM_FW_VER_MIN）运行时参数档 */
#ifndef GLAVO_SMALL_MEM_FW_VER_MIN
#define GLAVO_SMALL_MEM_FW_VER_MIN  0x00030004u
#endif

#ifndef GLAVO_SMALL_MEM_TCP_BATCH_SIZE
#define GLAVO_SMALL_MEM_TCP_BATCH_SIZE  64
#endif

#ifndef GLAVO_SMALL_MEM_TCP_SEND_INTERVAL_MS
#define GLAVO_SMALL_MEM_TCP_SEND_INTERVAL_MS  1
#endif

#ifndef GLAVO_SMALL_MEM_TCP_HEARTBEAT_INTERVAL_MS
#define GLAVO_SMALL_MEM_TCP_HEARTBEAT_INTERVAL_MS  100
#endif

#ifndef GLAVO_SMALL_MEM_MCU_CMD_BACKPRESSURE_PCT
#define GLAVO_SMALL_MEM_MCU_CMD_BACKPRESSURE_PCT  55
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

/** 门控：0=保持旧占位（直接通过）；1=走真实串口门控流程 */
#ifndef GLAVO_DOOR_REAL_OPEN_ENABLE
#define GLAVO_DOOR_REAL_OPEN_ENABLE  0
#endif

/** 关门检测：0=保持旧占位（直接判已关）；1=按传感器上报外门锁状态判定 */
#ifndef GLAVO_DOOR_REAL_CLOSE_CHECK_ENABLE
#define GLAVO_DOOR_REAL_CLOSE_CHECK_ENABLE  0
#endif

/** 真实开门请求等待 MCU 结果超时（毫秒） */
#ifndef GLAVO_DOOR_OPEN_TIMEOUT_MS
#define GLAVO_DOOR_OPEN_TIMEOUT_MS  5000
#endif

/** MCU OTA 测试：固件目录与块大小 */
#ifndef GLAVO_OTA_IMAGE_DIR
#define GLAVO_OTA_IMAGE_DIR  "/home/image/glavo"
#endif

#ifndef GLAVO_OTA_CHUNK_SIZE
#define GLAVO_OTA_CHUNK_SIZE  1024
#endif

#ifndef MCU_OTA_IMAGE_DIR
#define MCU_OTA_IMAGE_DIR  "/home/image/mcu"
#endif

#ifndef MCU_OTA_CHUNK_SIZE
#define MCU_OTA_CHUNK_SIZE  128
#endif

/** OTA Success（0x13）后链路线程循环 connect_tcp 直至成功：0=关；1=开 */
#ifndef GLAVO_OTA_POST_RECONNECT_ENABLE
#define GLAVO_OTA_POST_RECONNECT_ENABLE  1
#endif

/** 换层排空后向 Camera 发 Layer_Snap_Ntf：0=关；1=每层拍照录像 */
#ifndef GLAVO_ENABLE_LAYER_SNAP
#define GLAVO_ENABLE_LAYER_SNAP  1
#endif

/** 打印文件目录（仅 Glavo 使用；UI 通过 PrintFileSource 0/1 选择） */
/** 须与 common/fs_manifest/gcode_manifest.hpp 的 UPC_LOCAL_GCODE_DIR 保持一致 */
#ifndef GLAVO_LOCAL_GCODE_DIR
#define GLAVO_LOCAL_GCODE_DIR  "/home/Gcode"
#endif

#ifndef GLAVO_USB_MOUNT_DIR
#define GLAVO_USB_MOUNT_DIR  "/media/udisk0"
#endif

#endif // DEVICE_CONFIG_HPP
