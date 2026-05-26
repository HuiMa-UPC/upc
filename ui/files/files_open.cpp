#include "files_open.hpp"
#include "gcode_parse.hpp"
#include "job_status.hpp"

#include <dirent.h>
#include <errno.h>
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

files_storage_kind_t galva_get_storage_kind(void)
{
    return s_gcode_storage;
}

void cal_set_storage_kind(files_storage_kind_t kind)
{
    s_cal_storage = kind;
}

files_storage_kind_t cal_get_storage_kind(void)
{
    return s_cal_storage;
}

static bool is_safe_basename(const char *name)
{
    if(!name || name[0] == '\0') {
        return false;
    }
    if(strchr(name, '/') != nullptr || strstr(name, "..") != nullptr) {
        return false;
    }
    return true;
}

static int ensure_dir_exists(const char *path)
{
    struct stat st;
    if(stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

static int copy_file_overwrite(const char *src, const char *dst,
                               files_copy_progress_cb progress, void *progress_user,
                               char *err, size_t err_size)
{
    struct stat st;
    long total = 0;
    if(stat(src, &st) == 0 && S_ISREG(st.st_mode)) {
        total = (long)st.st_size;
    }

    FILE *in = fopen(src, "rb");
    if(!in) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "cannot read source: %s", strerror(errno));
        }
        return -1;
    }

    FILE *out = fopen(dst, "wb");
    if(!out) {
        fclose(in);
        if(err && err_size > 0) {
            snprintf(err, err_size, "cannot write destination: %s", strerror(errno));
        }
        return -1;
    }

    if(progress) {
        progress(0, total, progress_user);
    }

    char buf[8192];
    size_t n;
    long done = 0;
    while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if(fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            if(err && err_size > 0) {
                snprintf(err, err_size, "write failed: %s", strerror(errno));
            }
            return -1;
        }
        done += (long)n;
        if(progress) {
            progress(done, total, progress_user);
        }
    }
    if(ferror(in)) {
        fclose(in);
        fclose(out);
        if(err && err_size > 0) {
            snprintf(err, err_size, "read failed: %s", strerror(errno));
        }
        return -1;
    }

    fclose(in);
    fclose(out);
    if(progress && total > 0) {
        progress(total, total, progress_user);
    }
    return 0;
}

static int copy_usb_to_local_dir(const char *filename, const char *local_dir,
                                 files_copy_progress_cb progress, void *progress_user,
                                 char *err, size_t err_size)
{
    if(!is_safe_basename(filename)) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "invalid file name");
        }
        return -1;
    }
    if(ensure_dir_exists(local_dir) != 0) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "cannot create %s", local_dir);
        }
        return -1;
    }

    char src[512];
    char dst[512];
    if(snprintf(src, sizeof(src), "%s/%s", USB_DIR, filename) >= (int)sizeof(src) ||
       snprintf(dst, sizeof(dst), "%s/%s", local_dir, filename) >= (int)sizeof(dst)) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "path too long");
        }
        return -1;
    }

    return copy_file_overwrite(src, dst, progress, progress_user, err, err_size);
}

static int delete_local_dir_file(const char *filename, const char *local_dir,
                                 char *err, size_t err_size)
{
    if(!is_safe_basename(filename)) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "invalid file name");
        }
        return -1;
    }

    char path[512];
    if(snprintf(path, sizeof(path), "%s/%s", local_dir, filename) >= (int)sizeof(path)) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "path too long");
        }
        return -1;
    }

    if(unlink(path) != 0) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "delete failed: %s", strerror(errno));
        }
        return -1;
    }
    return 0;
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

int galva_copy_usb_to_local(const char *filename, char *err, size_t err_size,
                            files_copy_progress_cb progress, void *progress_user)
{
    if(s_gcode_storage != FILES_STORAGE_USB) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "Load is only available on USB view");
        }
        return -1;
    }
    return copy_usb_to_local_dir(filename, GCODE_DIR_LOCAL, progress, progress_user,
                                 err, err_size);
}

int cal_copy_usb_to_local(const char *filename, char *err, size_t err_size,
                          files_copy_progress_cb progress, void *progress_user)
{
    if(s_cal_storage != FILES_STORAGE_USB) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "Load is only available on USB view");
        }
        return -1;
    }
    return copy_usb_to_local_dir(filename, CAL_DIR_LOCAL, progress, progress_user,
                                 err, err_size);
}

int galva_delete_local_file(const char *filename, char *err, size_t err_size)
{
    if(s_gcode_storage != FILES_STORAGE_LOCAL) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "Delete is only available on local view");
        }
        return -1;
    }
    return delete_local_dir_file(filename, GCODE_DIR_LOCAL, err, err_size);
}

int cal_delete_local_file(const char *filename, char *err, size_t err_size)
{
    if(s_cal_storage != FILES_STORAGE_LOCAL) {
        if(err && err_size > 0) {
            snprintf(err, err_size, "Delete is only available on local view");
        }
        return -1;
    }
    return delete_local_dir_file(filename, CAL_DIR_LOCAL, err, err_size);
}
