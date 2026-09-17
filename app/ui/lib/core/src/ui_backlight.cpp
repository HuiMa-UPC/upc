#include "ui_backlight.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace ui {
namespace {

constexpr const char *kSysfsRoot = "/sys/class/backlight";
constexpr const char *kPreferred = "/sys/class/backlight/backlight";
constexpr const char *kPersistPath = "/userdata/ui_backlight_pct";
constexpr int kMinPercent = 5;

bool read_int_file(const char *path, int *out)
{
    if(!path || !out) {
        return false;
    }
    FILE *fp = std::fopen(path, "r");
    if(!fp) {
        return false;
    }
    int v = 0;
    const int n = std::fscanf(fp, "%d", &v);
    std::fclose(fp);
    if(n != 1) {
        return false;
    }
    *out = v;
    return true;
}

bool write_int_file(const char *path, int v)
{
    if(!path) {
        return false;
    }
    FILE *fp = std::fopen(path, "w");
    if(!fp) {
        return false;
    }
    const int n = std::fprintf(fp, "%d\n", v);
    const int rc = std::fclose(fp);
    return n > 0 && rc == 0;
}

bool join_sysfs(const char *dir, const char *leaf, char *out, size_t out_sz)
{
    if(!dir || !leaf || !out || out_sz < 8) {
        return false;
    }
    const int n = std::snprintf(out, out_sz, "%s/%s", dir, leaf);
    return n > 0 && static_cast<size_t>(n) < out_sz;
}

/** 优先文档节点 `backlight`；否则取第一颗非 SPI 小屏的 backlight。 */
bool resolve_device_dir(char *dir, size_t dir_sz)
{
    char bri[288];
    if(join_sysfs(kPreferred, "brightness", bri, sizeof(bri)) &&
       access(bri, F_OK) == 0) {
        if(dir_sz == 0) {
            return false;
        }
        const size_t n = std::strlen(kPreferred);
        if(n + 1 > dir_sz) {
            return false;
        }
        std::memcpy(dir, kPreferred, n + 1);
        return true;
    }

    DIR *d = opendir(kSysfsRoot);
    if(!d) {
        return false;
    }
    bool ok = false;
    while(const dirent *ent = readdir(d)) {
        if(ent->d_name[0] == '.') {
            continue;
        }
        if(std::strstr(ent->d_name, "spi") != nullptr) {
            continue;
        }
        /* d_name 最长 NAME_MAX；路径缓冲按上限预留，截断则跳过该条目 */
        char cand[288];
        if(!join_sysfs(kSysfsRoot, ent->d_name, cand, sizeof(cand))) {
            continue;
        }
        if(!join_sysfs(cand, "brightness", bri, sizeof(bri)) || access(bri, F_OK) != 0) {
            continue;
        }
        const size_t n = std::strlen(cand);
        if(n + 1 > dir_sz) {
            continue;
        }
        std::memcpy(dir, cand, n + 1);
        ok = true;
        break;
    }
    closedir(d);
    return ok;
}

int clamp_percent(int p)
{
    if(p < kMinPercent) {
        return kMinPercent;
    }
    if(p > 100) {
        return 100;
    }
    return p;
}

void persist_percent(int percent)
{
    write_int_file(kPersistPath, clamp_percent(percent));
}

int load_persisted_percent()
{
    int p = 0;
    if(!read_int_file(kPersistPath, &p)) {
        return -1;
    }
    if(p < 1 || p > 100) {
        return -1;
    }
    return clamp_percent(p);
}

} // namespace

int backlight_raw_max()
{
    char dir[288];
    if(!resolve_device_dir(dir, sizeof(dir))) {
        return 0;
    }
    char path[320];
    if(!join_sysfs(dir, "max_brightness", path, sizeof(path))) {
        return 0;
    }
    int mx = 0;
    if(!read_int_file(path, &mx) || mx <= 0) {
        return 0;
    }
    return mx;
}

int backlight_get_percent()
{
    char dir[288];
    if(!resolve_device_dir(dir, sizeof(dir))) {
        return -1;
    }
    char path[320];
    if(!join_sysfs(dir, "brightness", path, sizeof(path))) {
        return -1;
    }
    int raw = 0;
    if(!read_int_file(path, &raw)) {
        return -1;
    }
    const int mx = backlight_raw_max();
    if(mx <= 0) {
        return -1;
    }
    if(raw < 0) {
        raw = 0;
    }
    if(raw > mx) {
        raw = mx;
    }
    int pct = static_cast<int>((static_cast<long>(raw) * 100 + mx / 2) / mx);
    if(pct < 1) {
        pct = 1;
    }
    if(pct > 100) {
        pct = 100;
    }
    return pct;
}

bool backlight_set_percent(int percent)
{
    percent = clamp_percent(percent);
    char dir[288];
    if(!resolve_device_dir(dir, sizeof(dir))) {
        persist_percent(percent);
        return false;
    }
    const int mx = backlight_raw_max();
    if(mx <= 0) {
        persist_percent(percent);
        return false;
    }
    int raw = static_cast<int>((static_cast<long>(percent) * mx + 50) / 100);
    if(raw < 1) {
        raw = 1;
    }
    if(raw > mx) {
        raw = mx;
    }
    char path[320];
    if(!join_sysfs(dir, "brightness", path, sizeof(path))) {
        return false;
    }
    const bool ok = write_int_file(path, raw);
    persist_percent(percent);
    return ok;
}

void backlight_apply_saved()
{
    const int saved = load_persisted_percent();
    if(saved > 0) {
        (void)backlight_set_percent(saved);
    }
}

} // namespace ui
