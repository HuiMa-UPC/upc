#ifndef PIPELINE_TUNE_H
#define PIPELINE_TUNE_H

#include "ring_buffer.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析分块（对齐 Qt：定时器每 tick 推一批，而非连续跑满 CPU）
 */
#define GCODE_PARSE_CHUNK_LINES        200
#define GCODE_PARSE_CHUNK_LINES_SEND    40

/** 每解析完一块行数后的休眠 (ms) */
#define GCODE_PARSE_CHUNK_INTERVAL_MS   25
#define GCODE_PARSE_SEND_YIELD_MS       15
#define GCODE_PARSE_RING_HIGH_YIELD_MS  30
#define GCODE_PARSE_MCU_PRESSURE_MS     60
/** 解析减速：Ring 占用接近目标水位时 */
#define GCODE_PARSE_RING_SLOW_PCT       52

/**
 * 发送期 Ring 稳态目标占用（按 Batch/Intv 实测约 1% 系消费过快；调至 ~50%）
 * 解析在 PRODUCER_PAUSE 暂停入队，消费者在 CONSUMER_RESUME 以下暂停出队（滞回）。
 */
#define RB_TARGET_FILL_PCT            50
/** 发送期解析暂停入队线 */
#define RB_PRODUCER_PAUSE_PCT         55
#define RB_CONSUMER_RESUME_PCT        45
/** 未 Start（无 TCP 消费）时允许预填至满 */
#define RB_PRELOAD_PAUSE_PCT          100

/** 发送线程每轮最多连续组帧次数 */
#define LINK_SEND_BURST_MAX_BATCHES  24
/** 队列已达目标水位后，每轮最多连发批次数（避免抽干至 1%） */
#define LINK_SEND_BURST_ABOVE_TARGET   4

#define GCODE_PRELOAD_IDLE_MS        8

/** 下位机 Cmd 队列背压 */
#define MCU_CMD_BACKPRESSURE_ON_PCT   90
#define MCU_CMD_BACKPRESSURE_OFF_PCT  85
#define MCU_CACHE_RATE_MAX            65535

#ifdef __cplusplus
}
#endif

#endif
