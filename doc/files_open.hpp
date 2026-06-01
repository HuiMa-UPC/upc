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

/** 文件拷贝进度回调（done/total 字节数） */
typedef void (*files_copy_progress_cb)(long done_bytes, long total_bytes, void *user_data);

/** G-code：本机 /home/Gcode，U 盘 /media/udisk0 */
void galva_set_storage_kind(files_storage_kind_t kind);
files_storage_kind_t galva_get_storage_kind(void);

/** CAL：本机 /home/CAL，U 盘 /media/udisk0 */
void cal_set_storage_kind(files_storage_kind_t kind);
files_storage_kind_t cal_get_storage_kind(void);

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

/** U 盘 G-code 拷贝到 /home/Gcode，同名覆盖 */
int galva_copy_usb_to_local(const char *filename, char *err, size_t err_size,
                            files_copy_progress_cb progress, void *progress_user);

/** U 盘 CAL 拷贝到 /home/CAL，同名覆盖 */
int cal_copy_usb_to_local(const char *filename, char *err, size_t err_size,
                          files_copy_progress_cb progress, void *progress_user);

/** 删除本机 G-code 目录中的文件（仅 LOCAL 视图） */
int galva_delete_local_file(const char *filename, char *err, size_t err_size);

/** 删除本机 CAL 目录中的文件（仅 LOCAL 视图） */
int cal_delete_local_file(const char *filename, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
