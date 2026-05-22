#ifndef GCODE_WORKER_H
#define GCODE_WORKER_H

#include "ring_buffer.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开并预处理文件（统计行数等） */
int  gcode_open_file(const char *filepath);

/* 启动解析线程（从文件头开始解析并写入RingBuffer） */
void gcode_start_parse(RingBuffer *rb);

/* 停止解析 */
void gcode_stop_parse(void);

/* 查询进度 */
int  gcode_get_total_lines(void);
int  gcode_get_parsed_lines(void);
bool gcode_is_parsing(void);

/** 文件行已读完（未必所有包都已入队，可能仍在等待 Ring 空位） */
bool gcode_is_file_read_exhausted(void);

/* 关闭文件 */
void gcode_close_file(void);

/** 发送期目标稳态占用（包数，约 RB_TARGET_FILL_PCT） */
size_t gcode_ring_target_fill(void);

/** 解析暂停入队阈值（包数，略高于目标） */
size_t gcode_ring_high_water_mark(void);

/** 发送恢复阈值（包数，略低于目标；低于此暂停 TCP 运动下发） */
size_t gcode_ring_consumer_low_water(void);

#ifdef __cplusplus
}
#endif

#endif
