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

/** 扫描 /home/Gcode 目录，过滤 .gcode/.nc/.txt 后缀的文件 */
int galva_scan_files(gcode_file_list_t *out_list);

/** 组装完整路径：out = GCODE_DIR/filename */
int galva_build_full_path(const char *filename, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
