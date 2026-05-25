#include "files_open.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char *GCODE_DIR = "/home/Gcode";

/** 判断文件名是否以 .gcode / .nc / .txt 结尾（不区分大小写） */
static bool has_gcode_ext(const char *name)
{
    size_t len = strlen(name);
    return (len > 6 && strcasecmp(name + len - 6, ".gcode") == 0) ||
           (len > 3 && strcasecmp(name + len - 3, ".nc") == 0) ||
           (len > 4 && strcasecmp(name + len - 4, ".txt") == 0);
}

/**
 * @brief 扫描 G-code 目录，收集合法文件信息
 * @param out_list 输出文件列表（count 会被重置为 0 后重新填充）
 * @return 0 成功，-1 失败（目录无法打开）
 *
 * 遍历 /home/Gcode，跳过非普通文件和非法后缀，填充文件名与大小。
 */
int galva_scan_files(gcode_file_list_t *out_list)
{
    if(!out_list) return -1;
    out_list->count = 0;

    DIR *dir = opendir(GCODE_DIR);
    if(!dir) return -1;

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL && out_list->count < GCODE_MAX_FILES) {
        /* 跳过目录等非普通文件（DT_UNKNOWN 留给 stat 二次确认） */
        if(ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;

        if(!has_gcode_ext(ent->d_name)) continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", GCODE_DIR, ent->d_name);

        struct stat st;
        if(stat(fullpath, &st) != 0) continue;
        if(!S_ISREG(st.st_mode)) continue;

        gcode_file_info_t *info = &out_list->files[out_list->count];
        snprintf(info->name, sizeof(info->name), "%s", ent->d_name);
        info->size = st.st_size;
        out_list->count++;
    }
    closedir(dir);
    return 0;
}

/**
 * @brief 组装完整文件路径
 * @param filename  纯文件名（不含目录）
 * @param out       输出缓冲区
 * @param out_size  缓冲区大小
 * @return 0 成功，-1 失败（参数非法或路径截断）
 */
int galva_build_full_path(const char *filename, char *out, size_t out_size)
{
    if(!filename || !out || out_size == 0) return -1;
    int n = snprintf(out, out_size, "%s/%s", GCODE_DIR, filename);
    if(n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}
