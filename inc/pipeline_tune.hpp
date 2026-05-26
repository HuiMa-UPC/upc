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
#define GCODE_PARSE_CHUNK_LINES         8000
/** 发送期每 N 行同步一次进度/Ring（避免长时间不刷新 UI） */
#define GCODE_PARSE_SEND_SYNC_LINES      1024

/** 预填阶段每块解析后的休眠 (ms)；发送期解析线程不休眠 */
#define GCODE_PARSE_CHUNK_INTERVAL_MS   0
#define GCODE_PRELOAD_IDLE_MS        0

/**
 * ① Ring：解析仅在满时阻塞（put_wait）；发送期不按占比提前停解析。
 * TCP 背压只看下位机 cmd/data。
 */
#define RB_PRODUCER_PAUSE_PCT         100
#define RB_PRELOAD_PAUSE_PCT          100
#define TCP_STAGE_MAX_PACKETS         RB_MAX_PACKETS

/** 每轮 ①→② 搬运包数上限 */
#define REFILL_STAGE_MAX_PACKETS      16384

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
/** M3000 换层 / 打印完成：cmd、data rate 须均为 0 才发串口换层或判定结束 */
#define MCU_LAYER_DRAIN_MAX_RATE      0
#define MCU_CACHE_RATE_MAX            65535

#ifdef __cplusplus
}
#endif

#endif
