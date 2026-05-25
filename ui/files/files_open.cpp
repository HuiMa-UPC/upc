#include "files_open.hpp"
#include "gcode_parse.hpp"
#include "job_status.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char *const GCODE_DIR_LOCAL = "/home/Gcode";
static const char *const CAL_DIR_LOCAL   = "/home/CAL";
static const char *const USB_DIR         = "/media/udisk0";

static files_storage_kind_t s_gcode_storage = FILES_STORAGE_LOCAL;
static files_storage_kind_t s_cal_storage   = FILES_STORAGE_LOCAL;

static const char *gcode_dir_path(void)
{
    return s_gcode_storage == FILES_STORAGE_USB ? USB_DIR : GCODE_DIR_LOCAL;
}

static const char *cal_dir_path(void)
{
    return s_cal_storage == FILES_STORAGE_USB ? USB_DIR : CAL_DIR_LOCAL;
}

void galva_set_storage_kind(files_storage_kind_t kind)
{
    s_gcode_storage = kind;
}

void cal_set_storage_kind(files_storage_kind_t kind)
{
    s_cal_storage = kind;
}

static bool has_gcode_ext(const char *name)
{
    size_t len = strlen(name);
    return (len > 6 && strcasecmp(name + len - 6, ".gcode") == 0) ||
           (len > 3 && strcasecmp(name + len - 3, ".nc") == 0) ||
           (len > 4 && strcasecmp(name + len - 4, ".txt") == 0);
}

static bool has_csv_ext(const char *name)
{
    const size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".csv") == 0;
}

static int scan_dir_files(const char *dir_path, gcode_file_list_t *out_list,
                          bool (*match_ext)(const char *))
{
    if(!out_list || !dir_path || !match_ext) {
        return -1;
    }
    out_list->count = 0;

    DIR *dir = opendir(dir_path);
    if(!dir) {
        return -1;
    }

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL && out_list->count < GCODE_MAX_FILES) {
        if(ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) {
            continue;
        }
        if(!match_ext(ent->d_name)) {
            continue;
        }

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if(stat(fullpath, &st) != 0) {
            continue;
        }
        if(!S_ISREG(st.st_mode)) {
            continue;
        }

        gcode_file_info_t *info = &out_list->files[out_list->count];
        snprintf(info->name, sizeof(info->name), "%s", ent->d_name);
        info->size = st.st_size;
        out_list->count++;
    }
    closedir(dir);
    return 0;
}

int galva_scan_files(gcode_file_list_t *out_list)
{
    return scan_dir_files(gcode_dir_path(), out_list, has_gcode_ext);
}

int galva_build_full_path(const char *filename, char *out, size_t out_size)
{
    if(!filename || !out || out_size == 0) {
        return -1;
    }
    const int n = snprintf(out, out_size, "%s/%s", gcode_dir_path(), filename);
    if(n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return 0;
}

int cal_scan_files(gcode_file_list_t *out_list)
{
    return scan_dir_files(cal_dir_path(), out_list, has_csv_ext);
}

int cal_build_full_path(const char *filename, char *out, size_t out_size)
{
    if(!filename || !out || out_size == 0) {
        return -1;
    }
    const int n = snprintf(out, out_size, "%s/%s", cal_dir_path(), filename);
    if(n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return 0;
}

int cal_load_file(const char *filename, char *err, size_t err_size)
{
    if(!filename || filename[0] == '\0') {
        if(err && err_size > 0) {
            snprintf(err, err_size, "no file selected");
        }
        return -1;
    }

    char path[512];
    if(cal_build_full_path(filename, path, sizeof(path)) != 0) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "path too long");
        }
        return -1;
    }

    galvo_cal_data_t parsed;
    if(galvo_cal_parse_csv(path, &parsed, err, err_size) != 0) {
        return -1;
    }

    galvo_cal_store(&parsed);

    JOB_LOCK();
    snprintf(g_job_status.cal_file, sizeof(g_job_status.cal_file), "%s", filename);
    g_job_status.cal_file_loaded = true;
    g_job_status.cal_has_gains = parsed.has_cal_gains;
    g_job_status.cal_grid_nx = parsed.nx;
    g_job_status.cal_grid_ny = parsed.ny;
    if(parsed.has_cal_gains) {
        g_job_status.cal_image_scale_gain = parsed.cal_image_scale_gain;
        g_job_status.cal_image_rang_gain  = parsed.cal_image_rang_gain;
        g_job_status.cal_speed_gain       = parsed.cal_speed_gain;
    } else {
        g_job_status.cal_image_scale_gain = 0.f;
        g_job_status.cal_image_rang_gain  = 0.f;
        g_job_status.cal_speed_gain       = 0.f;
    }
    if(!g_job_status.tcp_connected || !g_job_status.cal_file_loaded) {
        g_job_status.enable_cal_apply = false;
    }
    return 0;
}
