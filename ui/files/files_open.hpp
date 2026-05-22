#ifndef FILES_OPEN_H
#define FILES_OPEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCODE_MAX_FILES 64
#define GCODE_NAME_LEN  256

typedef struct {
    char name[GCODE_NAME_LEN];
    long size;          /* bytes */
} gcode_file_info_t;

typedef struct {
    int count;
    gcode_file_info_t files[GCODE_MAX_FILES];
} gcode_file_list_t;

/** 扫描 /home/Gcode 目录，过滤 .gcode/.nc/.txt */
int galva_scan_files(gcode_file_list_t *out_list);

/** 统计指定文件的行数，失败返回 -1 */
int galva_count_lines(const char *filename);

/** 组装完整路径 */
int galva_build_full_path(const char *filename, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
