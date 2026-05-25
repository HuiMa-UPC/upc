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

typedef enum {
    FILES_STORAGE_LOCAL = 0,
    FILES_STORAGE_USB   = 1,
} files_storage_kind_t;

/** G-code：本机 /home/Gcode，U 盘 /media/udisk0 */
void galva_set_storage_kind(files_storage_kind_t kind);

/** CAL：本机 /home/CAL，U 盘 /media/udisk0 */
void cal_set_storage_kind(files_storage_kind_t kind);

/** 扫描当前 G-code 目录，过滤 .gcode/.nc/.txt */
int galva_scan_files(gcode_file_list_t *out_list);

/** 组装完整路径：当前 G-code 目录 + filename */
int galva_build_full_path(const char *filename, char *out, size_t out_size);

/** 扫描当前 CAL 目录，过滤 .csv */
int cal_scan_files(gcode_file_list_t *out_list);

/** 组装完整路径：当前 CAL 目录 + filename */
int cal_build_full_path(const char *filename, char *out, size_t out_size);

/**
 * 加载并解析校准 CSV，更新 galvo_cal 全局与 job_status 镜像
 * @return 0 成功，-1 失败（err 可选）
 */
int cal_load_file(const char *filename, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
