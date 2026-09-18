#ifndef DEVICE_CONFIG_HPP
#define DEVICE_CONFIG_HPP

/* -------------------------------------------------------------------------- */
/* 连接参数（TCP/串口）                                                        */
/* -------------------------------------------------------------------------- */
#ifndef GLAVO_TCP_DEFAULT_IP
#define GLAVO_TCP_DEFAULT_IP  "192.168.1.30"
#endif

#ifndef GLAVO_TCP_DEFAULT_PORT
#define GLAVO_TCP_DEFAULT_PORT  1502
#endif

#ifndef GLAVO_TCP_CONNECT_TIMEOUT_MS
#define GLAVO_TCP_CONNECT_TIMEOUT_MS  200
#endif

#ifndef TCP_RECONNECT_INTERVAL_MS
#define TCP_RECONNECT_INTERVAL_MS  1000
#endif

#ifndef GLAVO_SERIAL_DEFAULT_DEVICE
#define GLAVO_SERIAL_DEFAULT_DEVICE  "/dev/ttyS4"
#endif

#ifndef GLAVO_SERIAL_DEFAULT_BAUD
#define GLAVO_SERIAL_DEFAULT_BAUD  115200
#endif

/* -------------------------------------------------------------------------- */
/* 打印流程开关                                                                */
/* -------------------------------------------------------------------------- */
/** 换层：0=跳过 M3000 等待与串口换层；1=完整换层流程 */
#ifndef GLAVO_ENABLE_LAYER_CHANGE
#define GLAVO_ENABLE_LAYER_CHANGE  1
#endif

/** 换层排空后向 Camera 发 Layer_Snap_Ntf：0=关；1=每层拍照录像 */
#ifndef GLAVO_ENABLE_LAYER_SNAP
#define GLAVO_ENABLE_LAYER_SNAP  1
#endif

/* -------------------------------------------------------------------------- */
/* 运动下发参数（legacy 档）                                                   */
/* -------------------------------------------------------------------------- */
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

/** cmd 缓存背压：>= 阈值时暂停运动下发 */
#ifndef MCU_CMD_BACKPRESSURE_PCT
#define MCU_CMD_BACKPRESSURE_PCT  80
#endif

/* -------------------------------------------------------------------------- */
/* 运动下发参数（small-mem 档）                                                */
/* -------------------------------------------------------------------------- */
/** 小内存下位机（版本 >= GLAVO_SMALL_MEM_FW_VER_MIN）运行时参数档 */
#ifndef GLAVO_SMALL_MEM_FW_VER_MIN
#define GLAVO_SMALL_MEM_FW_VER_MIN  0x00030004u
#endif

#ifndef GLAVO_SMALL_MEM_TCP_BATCH_SIZE
#define GLAVO_SMALL_MEM_TCP_BATCH_SIZE  100
#endif

#ifndef GLAVO_SMALL_MEM_TCP_SEND_INTERVAL_MS
#define GLAVO_SMALL_MEM_TCP_SEND_INTERVAL_MS  5
#endif

#ifndef GLAVO_SMALL_MEM_TCP_HEARTBEAT_INTERVAL_MS
#define GLAVO_SMALL_MEM_TCP_HEARTBEAT_INTERVAL_MS  100
#endif

#ifndef GLAVO_SMALL_MEM_MCU_CMD_BACKPRESSURE_PCT
#define GLAVO_SMALL_MEM_MCU_CMD_BACKPRESSURE_PCT  70
#endif

/* -------------------------------------------------------------------------- */
/* 队列与完成判定                                                              */
/* -------------------------------------------------------------------------- */
/** Ring -> Stage 二级队列 */
#ifndef TCP_STAGE_MAX_PACKETS
#define TCP_STAGE_MAX_PACKETS  4096
#endif

/** 每轮从 RingBuffer 抽到 tcp_stage_ 的上限 */
#ifndef REFILL_STAGE_MAX_PACKETS
#define REFILL_STAGE_MAX_PACKETS  500
#endif

/** 完成判定：cmd/data rate <= 此值视为下位机缓存已排空（0=严格归零） */
#ifndef MCU_DRAIN_MAX_RATE
#define MCU_DRAIN_MAX_RATE  0
#endif

/** 主机包已发完但 rate 长期不降时的强制完成超时（毫秒） */
#ifndef MCU_DRAIN_TIMEOUT_MS
#define MCU_DRAIN_TIMEOUT_MS  60000
#endif

/**
 * 排空判定的“未观测到缓存非空”兜底窗口（毫秒）。
 *
 * 排空判定默认要求本作业先观测到过 MCU 缓存非空，再接受 rate 归零，以防心跳
 * 尚未刷新时把初值 0 误判为已排空。但超薄层（如尖顶零件首层仅十几个包）可能在
 * 两帧心跳之间就被 MCU 执行完，rate 恒为 0，该前置条件永远无法满足，进而形成
 * “不下发 -> rate 恒 0 -> 不放行 -> 不下发”的自锁。下述两个窗口用于兜底放行：
 *
 * - MCU_DRAIN_SMALL_BATCH_MS：本次待排空包数不足一个 batch（小层/小文件）时，
 *   rate 持续为 0 且等满该时长即视为已排空；
 * - MCU_DRAIN_FALLBACK_MS：其余情况的兜底时长，更保守。
 *
 * 两者仅在“从未观测到缓存非空”时生效，一旦观测到过非空即按原逻辑立即放行，
 * 因此对正常层数/正常文件的打印行为零影响。设为 0 表示禁用对应兜底。
 */
#ifndef MCU_DRAIN_SMALL_BATCH_MS
#define MCU_DRAIN_SMALL_BATCH_MS  300
#endif

#ifndef MCU_DRAIN_FALLBACK_MS
#define MCU_DRAIN_FALLBACK_MS  3000
#endif

/* -------------------------------------------------------------------------- */
/* 串口可靠发送（ACK）                                                         */
/* -------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------- */
/* 门控                                                                         */
/* -------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------- */
/* SOC 本地传感器（MCU≥V4；背光在 UI，不经本配置）                              */
/* -------------------------------------------------------------------------- */
/** 舱内 PMSA003-A */
#ifndef GLAVO_PM_CABIN_UART
#define GLAVO_PM_CABIN_UART  "/dev/ttyS0"
#endif
/**
 * 舱外 PMSA003-A
 */
#ifndef GLAVO_PM_OUTDOOR_UART
#define GLAVO_PM_OUTDOOR_UART  "/dev/ttyS1"
#endif
#ifndef GLAVO_PM_VALVE_SYSFS
#define GLAVO_PM_VALVE_SYSFS  "/sys/class/leds/fa_valve/brightness"
#endif
/** 六路舱灯（调试页单独开关；不看 MCU 版本） */
#ifndef GLAVO_SOC_LED_LGT1L_SYSFS
#define GLAVO_SOC_LED_LGT1L_SYSFS  "/sys/class/leds/lgt1l/brightness"
#endif
#ifndef GLAVO_SOC_LED_LGT1R_SYSFS
#define GLAVO_SOC_LED_LGT1R_SYSFS  "/sys/class/leds/lgt1r/brightness"
#endif
#ifndef GLAVO_SOC_LED_LGT2L_SYSFS
#define GLAVO_SOC_LED_LGT2L_SYSFS  "/sys/class/leds/lgt2l/brightness"
#endif
#ifndef GLAVO_SOC_LED_LGT2R_SYSFS
#define GLAVO_SOC_LED_LGT2R_SYSFS  "/sys/class/leds/lgt2r/brightness"
#endif
#ifndef GLAVO_SOC_LED_LGT3L_SYSFS
#define GLAVO_SOC_LED_LGT3L_SYSFS  "/sys/class/leds/lgt3l/brightness"
#endif
#ifndef GLAVO_SOC_LED_LGT3R_SYSFS
#define GLAVO_SOC_LED_LGT3R_SYSFS  "/sys/class/leds/lgt3r/brightness"
#endif
/** 氧传感器：IIO 原始计数 → 3.3V 满幅；I[uA]=V/100Ω×1e6；O2%=100*(1-e^(-I/K)) */
#ifndef GLAVO_O2_IIO_RAW
#define GLAVO_O2_IIO_RAW  "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#endif
#ifndef GLAVO_O2_VREF
#define GLAVO_O2_VREF  3.3f
#endif
#ifndef GLAVO_O2_ADC_MAX
#define GLAVO_O2_ADC_MAX  1023  /**< RK SARADC 10bit */
#endif
#ifndef GLAVO_O2_RSENSE_OHM
#define GLAVO_O2_RSENSE_OHM  100.f
#endif
#ifndef GLAVO_O2_K
#define GLAVO_O2_K  480.f  /**< 标定系数；空气标定后可改 */
#endif
/** SPI 灯效板（WS，40 颗）；仅 MCU≥V4 打开 */
#ifndef GLAVO_SPI_STATUS_LED_DEV
#define GLAVO_SPI_STATUS_LED_DEV  "/dev/spidev1.0"
#endif

/* -------------------------------------------------------------------------- */
/* OTA                                                                          */
/* -------------------------------------------------------------------------- */
#include "image_paths.hpp"

/** MCU / Galvo OTA：固件目录与块大小（AB 文件名见 common/fs_manifest/OTA_IMAGE.md） */
#ifndef GALVO_OTA_IMAGE_DIR
#define GALVO_OTA_IMAGE_DIR  UPC_IMAGE_GALVO_DIR
#endif

#ifndef GLAVO_OTA_CHUNK_SIZE
#define GLAVO_OTA_CHUNK_SIZE  1024
#endif

#ifndef MCU_OTA_IMAGE_DIR
#define MCU_OTA_IMAGE_DIR  UPC_IMAGE_MCU_DIR
#endif

#ifndef MCU_OTA_CHUNK_SIZE
#define MCU_OTA_CHUNK_SIZE  128
#endif

/** OTA Success（0x13）后链路线程循环 connect_tcp 直至成功：0=关；1=开 */
#ifndef GLAVO_OTA_POST_RECONNECT_ENABLE
#define GLAVO_OTA_POST_RECONNECT_ENABLE  1
#endif

/* -------------------------------------------------------------------------- */
/* 路径                                                                         */
/* -------------------------------------------------------------------------- */
#include "gcode_manifest.hpp"

/** 打印文件目录（仅 Glavo 使用；UI 通过 PrintFileSource 0/1 选择） */
#ifndef GLAVO_LOCAL_GCODE_DIR
#define GLAVO_LOCAL_GCODE_DIR  UPC_LOCAL_GCODE_DIR
#endif

/** 调试页 Test image 固定路径（红光循环；无 M3000，不进 Local manifest） */
#ifndef GLAVO_TEST_GCODE_PATH
#define GLAVO_TEST_GCODE_PATH  "/userdata/test/test.gcode"
#endif

#ifndef GLAVO_USB_MOUNT_DIR
#define GLAVO_USB_MOUNT_DIR  "/media/udisk0"
#endif

#endif // DEVICE_CONFIG_HPP
