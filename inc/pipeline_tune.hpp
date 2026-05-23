#ifndef PIPELINE_TUNE_H
#define PIPELINE_TUNE_H

#include "ring_buffer.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析分块：预填每轮最多读行数；**发送期无行数上限**（Ring 满时在 put_wait 阻塞）。
 * 包数不固定：一行 G1 通常 1 个运动包。
 */
#define GCODE_PARSE_CHUNK_LINES         2000
/** 发送期每 N 行同步一次进度/Ring（避免长时间不刷新 UI） */
#define GCODE_PARSE_SEND_SYNC_LINES      512

/** 预填阶段每块解析后的休眠 (ms)；发送期解析线程不休眠 */
#define GCODE_PARSE_CHUNK_INTERVAL_MS   1
#define GCODE_PRELOAD_IDLE_MS        1

/**
 * ① Ring：解析仅在满时阻塞（put_wait）；发送期不按占比提前停解析。
 * TCP 背压只看下位机 cmd/data。
 */
#define RB_PRODUCER_PAUSE_PCT         100
#define RB_PRELOAD_PAUSE_PCT          100
#define TCP_STAGE_MAX_PACKETS         RB_MAX_PACKETS

/** 发送线程每轮最多连续组帧（对齐 Qt flushPacketBatch） */
#define LINK_SEND_BURST_MAX_BATCHES   24
/** 常态连发（未断粮、未高压） */
#define LINK_SEND_BURST_DEFAULT       4
/** Cmd 偏高时降速连发 */
#define LINK_SEND_BURST_CMD_PRESSURE  2
#define LINK_SEND_BURST_UNTIL_MCU_REPORT 1
/** 每轮 ①→② 搬运包数上限 */
#define REFILL_STAGE_MAX_PACKETS      8192
/** 解析仍在跑且 ①+② 低于此值时 TCP 降连发，给 Ring 蓄水 */
#define RING_TCP_STARVE_MIN_PKTS      512

/**
 * 下位机 TCP 上报（tcpServerRAW.c ETH_report_Fun）：
 *   cmd_rate  = cmd_cnt  / 16777216 * 65535   （cmdq 字数，每条运动约 5 word）
 *   data_rate = data_cnt / 262144   * 65535   （dataq 字数，每点约 4 word）
 * 上位机 parse_tcp_response 小端读 [4..7]，UI 用 rate/65535 显示百分比。
 *
 * 背压须按 65535 满量程的百分比理解；勿用过小绝对 rate（如 200≈0.3%≈仅 5 万 word）。
 */
#define MCU_CMD_BACKPRESSURE_ON_PCT   90
#define MCU_CMD_BACKPRESSURE_OFF_PCT  85
/** 与 ON/OFF_PCT 对应的 rate 阈值（65535×pct/100） */
#define MCU_CMD_BACKPRESSURE_ON_RATE  ((65535 * 90) / 100)   /* 58981 ≈ 90.0% */
#define MCU_CMD_BACKPRESSURE_OFF_RATE ((65535 * 85) / 100)   /* 55704 ≈ 85.0% */
/** ≥此 rate（≈20%）开始降连发，早于 90% TX hold */
#define MCU_CMD_BACKPRESSURE_WARN_RATE ((65535 * 20) / 100)  /* 13107 ≈ 20.0% */
/** dataq 断粮加发：须 cmd 已>此值（流水线在跑），避免 Reset 后 data=0 误判 */
#define MCU_DATA_FEED_MIN_CMD_RATE    1000
/** dataq 低于此 rate（≈49%）且 cmd 在合理范围时可用 MAX 连发 */
#define MCU_DATA_FEED_MIN_RATE        32000
/** M3000 换层：cmd/data rate 低于此阈值视为队列已排空（避免永远等精确 0） */
#define MCU_LAYER_DRAIN_MAX_RATE      32
#define MCU_CACHE_RATE_MAX            65535

#ifdef __cplusplus
}
#endif

#endif
