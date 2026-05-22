#include "files_open.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char *GCODE_DIR = "/home/Gcode";

static bool has_gcode_ext(const char *name)
{
    size_t len = strlen(name);
    return (len > 6 && strcasecmp(name + len - 6, ".gcode") == 0) ||
           (len > 3 && strcasecmp(name + len - 3, ".nc") == 0) ||
           (len > 4 && strcasecmp(name + len - 4, ".txt") == 0);
}

int galva_scan_files(gcode_file_list_t *out)
{
    if(!out) return -1;
    out->count = 0;

    DIR *dir = opendir(GCODE_DIR);
    if(!dir) return -1;

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL && out->count < GCODE_MAX_FILES) {
        if(ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;

        if(!has_gcode_ext(ent->d_name)) continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", GCODE_DIR, ent->d_name);

        struct stat st;
        if(stat(fullpath, &st) != 0) continue;
        if(!S_ISREG(st.st_mode)) continue;

        gcode_file_info_t *info = &out->files[out->count];
        snprintf(info->name, sizeof(info->name), "%s", ent->d_name);
        info->size = st.st_size;
        out->count++;
    }
    closedir(dir);
    return 0;
}

int galva_count_lines(const char *filename)
{
    char path[512];
    if(galva_build_full_path(filename, path, sizeof(path)) != 0) return -1;

    FILE *fp = fopen(path, "r");
    if(!fp) return -1;

    int lines = 0;
    int c;
    while((c = fgetc(fp)) != EOF) {
        if(c == '\n') lines++;
    }
    fclose(fp);

    if(lines == 0) {
        struct stat st;
        if(stat(path, &st) == 0 && st.st_size > 0) lines = 1;
    }
    return lines;
}

int galva_build_full_path(const char *filename, char *out, size_t out_size)
{
    if(!filename || !out || out_size == 0) return -1;
    int n = snprintf(out, out_size, "%s/%s", GCODE_DIR, filename);
    if(n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}
