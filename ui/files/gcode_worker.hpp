#ifndef GCODE_WORKER_H
#define GCODE_WORKER_H

#include "ring_buffer.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开并预处理文件（统计行数等） */
int  gcode_open_file(const char *filepath);

/*
 * 启动解析线程（从文件头开始解析并写入 RingBuffer）
 * G-code 行解析使用 std::regex，规则与 docs/Qt/gcodeparserworker.cpp 一致
 */
void gcode_start_parse(RingBuffer *rb);

/* 停止解析 */
void gcode_stop_parse(void);

/* 查询进度 */
int  gcode_get_total_lines(void);
int  gcode_get_parsed_lines(void);
bool gcode_is_parsing(void);

/** 文件行已读完（未必所有包都已入队，可能仍在等待 Ring 空位） */
bool gcode_is_file_read_exhausted(void);

/** 是否已 Open 文件（解析线程是否可重启） */
bool gcode_is_file_open(void);

/* 关闭文件 */
void gcode_close_file(void);

#ifdef __cplusplus
}
#endif

#endif
